#include "auth.hpp"
#include "costlog.hpp"
#include "mock_server.hpp"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using mock::MockServer;
using mock::RecordedRequest;
using mock::Reply;
using mock::Scenario;
using nlohmann::json;

constexpr const char* binary_path = ORCLI_BINARY;
constexpr const char* creds_tool_path = ORCLI_FAKE_CREDS_TOOL;

constexpr int exit_ok = 0;
constexpr int exit_usage = 2;
constexpr int exit_refused = 3;
constexpr int exit_model = 4;
constexpr int exit_api = 5;
constexpr int exit_incomplete = 6;
constexpr int exit_config = 7;
constexpr int exit_interrupted = 8;
constexpr int exit_not_saved = 9;
constexpr int exit_exec_failed = 127;
constexpr int signal_exit_base = 128;

constexpr int http_ok = 200;
constexpr int http_unauthorized = 401;
constexpr int http_forbidden = 403;
constexpr int http_not_found = 404;
constexpr int http_server_error = 500;
constexpr int idle_timeout_seconds = 2;
constexpr int total_timeout_seconds = 30;
constexpr long long alpha_max_tokens = 8000;
constexpr mode_t private_file_mode = S_IRUSR | S_IWUSR;
constexpr std::size_t verifier_length = 86;
constexpr int members_page_size = 100;
constexpr int members_page_cap = 50;
// Above the cost log's few records, far below the answer of Scenario::large.
constexpr rlim_t answer_file_limit = 4096;

constexpr std::chrono::milliseconds poll_interval{20};
constexpr std::chrono::seconds output_wait{15};
// The persistent-404 case waits out the CLI's 30 s generation lookup window.
constexpr std::chrono::seconds child_time_limit{90};
constexpr std::chrono::seconds idle_hold{5};
constexpr std::chrono::seconds drip_hold{3};

constexpr std::string_view alpha = "test/alpha-1";

std::string inference_key() {
    return mock::fake_key('1');
}

std::string management_key() {
    return mock::fake_key('2');
}

std::string bearer(std::string_view key) {
    return "Bearer " + std::string(key);
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_text(const fs::path& path, std::string_view text) {
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
        REQUIRE(out.good());
    }
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write);
}

void make_private_dir(const fs::path& dir) {
    fs::create_directories(dir);
    fs::permissions(dir, fs::perms::owner_all);
}

std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> lines;
    std::istringstream stream{std::string(text)};
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

// The first line starting with prefix, or an empty string.
std::string line_starting(std::string_view text, std::string_view prefix) {
    for (const std::string& line : lines_of(text)) {
        if (line.starts_with(prefix)) {
            return line;
        }
    }
    return {};
}

std::string text_at(const json& object, const char* key) {
    return object.at(key).get<std::string>();
}

int offset_param(const RecordedRequest& request) {
    const std::string text = request.param("offset");
    int offset = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), offset);
    return parsed.ec == std::errc{} ? offset : 0;
}

Reply data_reply(const json& data) {
    return {.status = http_ok, .body = json{{"data", data}}.dump()};
}

// A full members page of people other than the key's creator.
json other_members(int offset, int count) {
    json page = json::array();
    for (int index = 0; index < count; ++index) {
        const int number = offset + index;
        page.push_back(mock::member(std::format("user_other_{}", number),
                                    std::format("other{}@example.com", number)));
    }
    return page;
}

class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    ~UniqueFd() { reset(); }

    [[nodiscard]] int get() const { return fd_; }
    void reset() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

UniqueFd open_fd(const fs::path& path, int flags) {
    UniqueFd fd(::open(path.c_str(), flags | O_CLOEXEC, private_file_mode));
    REQUIRE(fd.get() >= 0);
    return fd;
}

// A started openrouter-cli process; one that is never waited for is killed and reaped.
class Child {
public:
    Child(pid_t pid, fs::path out_path, fs::path err_path)
        : pid_(pid), out_(std::move(out_path)), err_(std::move(err_path)) {}
    Child(Child&& other) noexcept
        : pid_(std::exchange(other.pid_, -1)), out_(std::move(other.out_)),
          err_(std::move(other.err_)) {}
    Child& operator=(Child&&) = delete;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    ~Child() {
        if (pid_ > 0) {
            ::kill(-pid_, SIGKILL);
            ::kill(pid_, SIGKILL);
            int status = 0;
            while (::waitpid(pid_, &status, 0) < 0) {
                if (errno != EINTR) {
                    break;
                }
            }
        }
    }

    [[nodiscard]] pid_t pid() const { return pid_; }
    [[nodiscard]] std::string out() const { return read_text(out_); }
    [[nodiscard]] std::string err() const { return read_text(err_); }

    // Exit status, or 128 + signal number for a process killed by a signal.
    int wait() {
        REQUIRE(pid_ > 0);
        const auto deadline = Clock::now() + child_time_limit;
        int status = 0;
        while (true) {
            const pid_t reaped = ::waitpid(pid_, &status, WNOHANG);
            if (reaped == pid_) {
                break;
            }
            REQUIRE((reaped == 0 || errno == EINTR));
            REQUIRE_MESSAGE(Clock::now() < deadline, "openrouter-cli did not exit in time");
            std::this_thread::sleep_for(poll_interval);
        }
        pid_ = -1;
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        if (WIFSIGNALED(status)) {
            return signal_exit_base + WTERMSIG(status);
        }
        return -1;
    }

    // Polls stdout until it contains text, so a test acts only once the CLI has streamed it.
    [[nodiscard]] bool wait_for_output(std::string_view text) const {
        const auto deadline = Clock::now() + output_wait;
        while (Clock::now() < deadline) {
            if (out().contains(text)) {
                return true;
            }
            std::this_thread::sleep_for(poll_interval);
        }
        return false;
    }

private:
    pid_t pid_;
    fs::path out_;
    fs::path err_;
};

struct RunResult {
    int code = -1;
    std::string out;
    std::string err;

    [[nodiscard]] std::string all() const { return out + err; }
};

struct Launch {
    std::vector<std::string> args{};
    std::optional<std::string> stdin_text{};
    std::optional<fs::path> stdin_path{};
    std::optional<int> stdin_descriptor{};
    std::optional<int> stdout_descriptor{}; // borrowed; use spawn and capture in the parent
    bool failed_stderr = false;
    bool closed_stdout = false; // stdout is a pipe whose read end is already closed
    bool creds_tool = true;     // false: no test credential tool and nothing on PATH
    std::optional<fs::path> home{};
    std::optional<rlim_t> file_size_limit{}; // RLIMIT_FSIZE, with SIGXFSZ ignored
    std::vector<std::string> extra_env{};    // NAME=value entries added to the environment
};

std::vector<char*> pointers_to(std::vector<std::string>& strings) {
    std::vector<char*> pointers;
    pointers.reserve(strings.size() + 1);
    for (std::string& text : strings) {
        pointers.push_back(text.data());
    }
    pointers.push_back(nullptr);
    return pointers;
}

struct ChildSetup {
    int in = -1;
    int out = -1;
    int err = -1;
    const char* workdir = nullptr;
    char* const* argv = nullptr;
    char* const* envp = nullptr;
    bool limit_file_size = false;
    rlim_t file_size_limit = 0;
};

// Runs between fork and execve in a multithreaded parent, so only async-signal-safe calls.
[[noreturn]] void exec_child(const ChildSetup& setup) noexcept {
    if (::setpgid(0, 0) != 0) {
        ::_exit(exit_exec_failed);
    }
    if (::dup2(setup.in, STDIN_FILENO) < 0 || ::dup2(setup.out, STDOUT_FILENO) < 0 ||
        ::dup2(setup.err, STDERR_FILENO) < 0 || ::chdir(setup.workdir) != 0) {
        ::_exit(exit_exec_failed);
    }
    ::close_range(static_cast<unsigned int>(STDERR_FILENO + 1),
                  std::numeric_limits<unsigned int>::max(), 0);
    // The mock server ignores SIGPIPE in this process; the CLI must set that up for itself.
    struct sigaction default_action{};
    default_action.sa_handler = SIG_DFL;
    sigemptyset(&default_action.sa_mask);
    ::sigaction(SIGPIPE, &default_action, nullptr);
    if (setup.limit_file_size) {
        // Ignored, so a write past the limit fails with EFBIG instead of killing the CLI.
        struct sigaction ignore_action{};
        ignore_action.sa_handler = SIG_IGN;
        sigemptyset(&ignore_action.sa_mask);
        ::sigaction(SIGXFSZ, &ignore_action, nullptr);
        const struct rlimit limit{.rlim_cur = setup.file_size_limit,
                                  .rlim_max = setup.file_size_limit};
        if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) {
            ::_exit(exit_exec_failed);
        }
    }
    sigset_t no_signals;
    sigemptyset(&no_signals);
    ::pthread_sigmask(SIG_SETMASK, &no_signals, nullptr);
    ::execve(binary_path, setup.argv, setup.envp);
    ::_exit(exit_exec_failed);
}

// A private temporary HOME and project directory wired to one mock server. Keys go in through
// `auth set` with the fake credential tool, never through .env.
class Sandbox {
public:
    explicit Sandbox(int port, bool legacy = false)
        : root_(make_root()), port_(port), legacy_(legacy) {
        for (const fs::path& dir :
             {home(), project(), work(), empty_bin(), tools_bin(), config_dir()}) {
            make_private_dir(dir);
        }
        fs::create_symlink(creds_tool_path, tools_bin() / "systemd-creds");
        write_env("");
    }
    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;
    Sandbox(Sandbox&&) = delete;
    Sandbox& operator=(Sandbox&&) = delete;
    ~Sandbox() {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    [[nodiscard]] fs::path root() const { return root_; }
    [[nodiscard]] fs::path home() const { return root_ / "home"; }
    [[nodiscard]] fs::path project() const { return root_ / "project"; }
    [[nodiscard]] fs::path work() const { return root_ / "work"; }
    [[nodiscard]] fs::path empty_bin() const { return root_ / "empty-bin"; }
    [[nodiscard]] fs::path tools_bin() const { return root_ / "tools"; }
    [[nodiscard]] fs::path config_dir() const { return home() / ".config" / "openrouter-cli"; }
    [[nodiscard]] fs::path settings_file() const {
        return legacy_ ? project() / ".env" : config_dir() / "config.env";
    }
    [[nodiscard]] fs::path state_dir() const {
        return legacy_ ? project() / "state" : cost_log().parent_path();
    }
    [[nodiscard]] fs::path api_credential() const {
        return state_dir() / "credentials" / "api-key.cred";
    }
    [[nodiscard]] fs::path management_credential() const {
        return state_dir() / "credentials" / "management-key.cred";
    }
    [[nodiscard]] fs::path cost_log() const {
        return home() / ".local" / "state" / "openrouter-cli" / "costs.jsonl";
    }

    // The settings every test needs, followed by extra_lines.
    void write_env(std::string_view extra_lines) const {
        write_text(settings_file(),
                   std::format("OPENROUTER_CLI_BASE_URL=http://127.0.0.1:{}/api/v1\n"
                               "OPENROUTER_CLI_IDLE_TIMEOUT_SECONDS={}\n"
                               "OPENROUTER_CLI_TOTAL_TIMEOUT_SECONDS={}\n{}",
                               port_, idle_timeout_seconds, total_timeout_seconds, extra_lines));
    }

    Child spawn(const Launch& launch) {
        REQUIRE_FALSE((launch.closed_stdout && launch.stdout_descriptor.has_value()));
        ++runs_;
        const fs::path in_path = root_ / std::format("run-{}.in", runs_);
        const fs::path out_path = root_ / std::format("run-{}.out", runs_);
        const fs::path err_path = root_ / std::format("run-{}.err", runs_);

        if (launch.stdin_text.has_value()) {
            write_text(in_path, *launch.stdin_text);
        }
        UniqueFd in;
        if (!launch.stdin_descriptor.has_value()) {
            in = open_fd(launch.stdin_text.has_value() ? in_path
                                                       : launch.stdin_path.value_or("/dev/null"),
                         O_RDONLY);
        }
        UniqueFd out;
        if (launch.closed_stdout) {
            std::array<int, 2> ends{-1, -1};
            REQUIRE(::pipe2(ends.data(), O_CLOEXEC) == 0);
            const UniqueFd read_end(ends[0]);
            out = UniqueFd(ends[1]);
        } else if (!launch.stdout_descriptor.has_value()) {
            out = open_fd(out_path, O_WRONLY | O_CREAT | O_TRUNC);
        }
        const UniqueFd err = launch.failed_stderr ? open_fd("/dev/full", O_WRONLY)
                                                  : open_fd(err_path, O_WRONLY | O_CREAT | O_TRUNC);

        std::vector<std::string> arg_strings{binary_path};
        std::ranges::copy(launch.args, std::back_inserter(arg_strings));
        std::vector<std::string> env_strings{
            "HOME=" + launch.home.value_or(home()).string(),
            "XDG_CONFIG_HOME=" + (home() / ".config").string(),
            "XDG_STATE_HOME=" + (home() / ".local/state").string(),
        };
        if (legacy_) {
            env_strings.push_back("OPENROUTER_CLI_HOME=" + project().string());
        }
        if (launch.creds_tool) {
            env_strings.push_back("PATH=" + tools_bin().string() + ":/usr/bin:/bin");
        } else {
            env_strings.push_back("PATH=" + empty_bin().string());
        }
        std::ranges::copy(launch.extra_env, std::back_inserter(env_strings));
        const std::vector<char*> argv = pointers_to(arg_strings);
        const std::vector<char*> envp = pointers_to(env_strings);
        const std::string workdir = work().string();
        const ChildSetup setup{.in = launch.stdin_descriptor.value_or(in.get()),
                               .out = launch.stdout_descriptor.value_or(out.get()),
                               .err = err.get(),
                               .workdir = workdir.c_str(),
                               .argv = argv.data(),
                               .envp = envp.data(),
                               .limit_file_size = launch.file_size_limit.has_value(),
                               .file_size_limit = launch.file_size_limit.value_or(0)};

        const pid_t pid = ::fork();
        REQUIRE(pid >= 0);
        if (pid == 0) {
            exec_child(setup);
        }
        return {pid, out_path, err_path};
    }

    RunResult run_with(const Launch& launch) {
        REQUIRE_FALSE(launch.stdout_descriptor.has_value());
        Child child = spawn(launch);
        const int code = child.wait();
        return {.code = code, .out = child.out(), .err = child.err()};
    }

    RunResult run(std::vector<std::string> args) {
        return run_with(Launch{.args = std::move(args)});
    }

    // `printf 'KEY\n' | openrouter-cli auth set [flags]`.
    RunResult auth_set(std::string_view key, std::vector<std::string> flags = {}) {
        std::vector<std::string> args{"auth", "set"};
        std::ranges::move(flags, std::back_inserter(args));
        return run_with(Launch{.args = std::move(args), .stdin_text = std::string(key) + "\n"});
    }

    void store_api_key() {
        const RunResult stored = auth_set(inference_key());
        INFO(stored.err);
        REQUIRE(stored.code == exit_ok);
    }

    void store_management_key() {
        const RunResult stored = auth_set(management_key(), {"--management"});
        INFO(stored.err);
        REQUIRE(stored.code == exit_ok);
    }

    // Every cost log record in file order; a missing log has none.
    [[nodiscard]] std::vector<json> records() const {
        std::vector<json> parsed;
        for (const std::string& line : lines_of(read_text(cost_log()))) {
            if (line.empty()) {
                continue;
            }
            json record = json::parse(line, nullptr, false);
            REQUIRE_FALSE(record.is_discarded());
            parsed.push_back(std::move(record));
        }
        return parsed;
    }

    [[nodiscard]] std::vector<json> records_of(std::string_view type) const {
        const std::vector<json> all = records();
        std::vector<json> matching;
        std::ranges::copy_if(all, std::back_inserter(matching), [type](const json& record) {
            return record.value("type", std::string()) == type;
        });
        return matching;
    }

    [[nodiscard]] std::vector<fs::path> answer_files() const {
        std::vector<fs::path> files;
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(state_dir() / "answers", error)) {
            files.push_back(entry.path());
        }
        std::ranges::sort(files);
        return files;
    }

    [[nodiscard]] std::string only_answer() const {
        const std::vector<fs::path> files = answer_files();
        REQUIRE(files.size() == 1U);
        return read_text(files.front());
    }

private:
    static fs::path make_root() {
        std::string pattern = (fs::temp_directory_path() / "orcli-it-XXXXXX").string();
        REQUIRE(::mkdtemp(pattern.data()) != nullptr);
        return pattern;
    }

    fs::path root_;
    int port_;
    bool legacy_ = false;
    int runs_ = 0;
};

std::vector<std::string> ask(std::string query) {
    return {"-m", std::string(alpha), std::move(query)};
}

std::string last_user_text(const json& body) {
    return text_at(body.at("messages").back(), "content");
}

std::string result_outcome(const Sandbox& sandbox) {
    const std::vector<json> results = sandbox.records_of("result");
    REQUIRE(results.size() == 1U);
    return text_at(results.front(), "outcome");
}

bool file_exists(const fs::path& path) {
    std::error_code error;
    return fs::exists(path, error);
}

// Appends one finished call whose cost is unknown, as an older run of the CLI would have.
void log_unpriced_call(const Sandbox& sandbox, const std::string& call_id,
                       const std::string& generation) {
    make_private_dir(sandbox.cost_log().parent_path());
    const orcli::CostLog ledger(sandbox.cost_log(),
                                sandbox.cost_log().parent_path() / "costs.lock");
    orcli::PendingInfo pending;
    pending.call_id = call_id;
    pending.time = orcli::utc_now_iso();
    pending.cwd = sandbox.work().string();
    pending.requested_model = std::string(alpha);
    pending.model_id = std::string(alpha);
    pending.privacy = "zdr";
    pending.answer_path = (sandbox.state_dir() / "answers" / (call_id + ".md")).string();
    orcli::ResultInfo result;
    result.call_id = call_id;
    result.outcome = "complete";
    REQUIRE(ledger.append(orcli::pending_record(pending)).has_value());
    REQUIRE(
        ledger.append(orcli::generation_record({.call_id = call_id, .generation_id = generation}))
            .has_value());
    REQUIRE(ledger.append(orcli::result_record(result)).has_value());
}

// Removes the SGR color sequences the help pages use.
std::string strip_escapes(std::string_view text) {
    std::string plain;
    bool in_escape = false;
    for (const char character : text) {
        if (in_escape) {
            in_escape = character != 'm';
            continue;
        }
        if (character == '\x1b') {
            in_escape = true;
            continue;
        }
        plain += character;
    }
    return plain;
}

} // namespace

// ----- asking a model ----------------------------------------------------------------------

TEST_CASE("a streamed answer is printed, saved and logged with its cost") {
    MockServer server;
    Sandbox sandbox(server.port());
    sandbox.store_api_key();

    const RunResult result = sandbox.run(ask("Review this plan."));

    CHECK(result.code == exit_ok);
    CHECK(result.out == "Hello world\n");
    const std::string cost_line = line_starting(result.err, "openrouter-cli: cost $0.0042");
    CHECK_FALSE(cost_line.empty());
    CHECK(cost_line.contains("via MockProvider"));
    CHECK(server.post_count() == 1);
    CHECK(server.generation_count() == 0);
    CHECK(server.last_authorization() == bearer(inference_key()));
    CHECK(sandbox.only_answer() == "Hello world");

    const std::vector<json> pending = sandbox.records_of("pending");
    const std::vector<json> generation = sandbox.records_of("generation");
    const std::vector<json> results = sandbox.records_of("result");
    REQUIRE(pending.size() == 1U);
    REQUIRE(generation.size() == 1U);
    REQUIRE(results.size() == 1U);
    const std::string call_id = text_at(pending.front(), "call_id");
    CHECK(text_at(generation.front(), "call_id") == call_id);
    CHECK(text_at(results.front(), "call_id") == call_id);
    CHECK(text_at(results.front(), "outcome") == "complete");
    CHECK(sandbox.answer_files().front().stem().string() == call_id);

    const json body = server.last_post();
    CHECK(text_at(body, "model") == alpha);
    CHECK(body.at("stream").get<bool>());
    CHECK(body.at("max_tokens").get<long long>() <= alpha_max_tokens);
    const json& provider = body.at("provider");
    CHECK(text_at(provider, "data_collection") == "deny");
    CHECK(provider.at("zdr").get<bool>());
    CHECK(provider.at("require_parameters").get<bool>());
    CHECK_FALSE(provider.contains("only"));
    CHECK(body.at("plugins").dump() ==
          R"([{"enabled":false,"id":"web"},{"enabled":false,"id":"context-compression"}])");
    for (const char* absent : {"temperature", "top_p", "session_id", "cache_control"}) {
        INFO("field ", absent);
        CHECK_FALSE(body.contains(absent));
    }
}

TEST_CASE("-p slugs are forwarded unchanged as provider.only without an endpoints lookup") {
    MockServer server;
    Sandbox sandbox(server.port());
    sandbox.store_api_key();

    const RunResult result = sandbox.run({"-m", std::string(alpha), "-p", "mockcloud/fp8", "-p",
                                          "Other-Cloud", "-p", "mockcloud/fp8", "question"});

    CHECK(result.code == exit_ok);
    CHECK(server.last_post().at("provider").at("only").dump() ==
          R"(["mockcloud/fp8","Other-Cloud"])");
    for (const RecordedRequest& request : server.requests()) {
        INFO(request.path);
        CHECK_FALSE(request.path.ends_with("/endpoints"));
    }
    CHECK(server.post_count() == 1);
}

TEST_CASE("an OpenRouter no-endpoints error exits 5 with a hint to inspect the model") {
    MockServer server;
    server.set_route("/chat/completions", [](const RecordedRequest&) {
        return Reply{.status = http_not_found,
                     .body = R"({"error":{"code":404,"message":"No endpoints found matching your )"
                             R"(data policy (Zero data retention)."}})"};
    });
    Sandbox sandbox(server.port());
    sandbox.store_api_key();

    const RunResult result = sandbox.run({"-m", std::string(alpha), "-p", "nowhere", "question"});

    CHECK(result.code == exit_api);
    CHECK(result.err.contains("HTTP 404"));
    CHECK_FALSE(result.err.contains("No endpoints found"));
    CHECK(result.err.contains("models show"));
    CHECK(server.post_count() == 1);
}

TEST_CASE("a plain model name resolves without saving an alias") {
    MockServer server;
    Sandbox sandbox(server.port());
    sandbox.store_api_key();

    const RunResult first = sandbox.run({"-m", "alpha 1", "first question"});
    CHECK(first.code == exit_ok);
    CHECK_FALSE(first.err.contains("saved alias"));
    CHECK_FALSE(file_exists(sandbox.state_dir() / "models.json"));
    CHECK(text_at(server.last_post(), "model") == alpha);

    const RunResult second = sandbox.run({"-m", "alpha-1", "second question"});
    CHECK(second.code == exit_ok);
    CHECK(text_at(server.last_post(), "model") == alpha);
    CHECK(server.post_count() == 2);
}

TEST_CASE("an ambiguous model name lists the candidates and sends nothing") {
    MockServer server;
    Sandbox sandbox(server.port());
    sandbox.store_api_key();

    const RunResult result = sandbox.run({"-m", "beta", "question"});

    CHECK(result.code == exit_model);
    CHECK(result.err.contains("test/beta-2"));
    CHECK(result.err.contains("test/beta-3"));
    CHECK(result.out.empty());
    CHECK(server.post_count() == 0);
}

TEST_CASE("an unknown cost is looked up until the generation record appears") {
    MockServer server;
    server.set_scenario(Scenario::success_no_cost);
    server.set_generation_statuses({http_not_found, http_not_found}, http_ok);
    Sandbox sandbox(server.port());
    sandbox.store_api_key();

    const RunResult result = sandbox.run(ask("question"));

    CHECK(result.code == exit_ok);
    CHECK(result.out == "Hello world\n");
    CHECK_FALSE(line_starting(result.err, "openrouter-cli: cost $0.0123").empty());
    CHECK(server.generation_count() == 3);
    CHECK(server.last_generation_id() == "gen-test-1");
    for (const RecordedRequest& lookup : server.requests_to("/generation")) {
        CHECK(lookup.authorization == bearer(inference_key()));
    }
    CHECK(sandbox.records_of("resolved").size() == 1U);
    CHECK(server.post_count() == 1);
}

TEST_CASE("a generation record that never appears leaves the cost unknown") {
    MockServer server;
    server.set_scenario(Scenario::success_no_cost);
    server.set_generation_statuses({}, http_not_found);
    Sandbox sandbox(server.port());
    sandbox.store_api_key();

    const RunResult result = sandbox.run(ask("question"));

    CHECK(result.code == exit_ok);
    CHECK_FALSE(line_starting(result.err, "openrouter-cli: cost unknown").empty());
    CHECK(server.generation_count() > 1);
    CHECK(sandbox.records_of("resolved").empty());
    CHECK(server.post_count() == 1);
}

TEST_CASE("an HTTP error status is reported by type without echoing secrets") {
    MockServer server;
    server.set_scenario(Scenario::http_error);
    Sandbox sandbox(server.port());
    sandbox.store_api_key();

    const RunResult result = sandbox.run(ask("question"));

    CHECK(result.code == exit_api);
    CHECK(result.err.contains("402"));
    CHECK(result.err.contains("provider_error"));
    CHECK_FALSE(result.err.contains(mock::fake_api_key()));
    CHECK_FALSE(result.err.contains("[redacted]"));
    CHECK(result.out.empty());
    CHECK(server.post_count() == 1);
    CHECK(result_outcome(sandbox) == "error");
}

TEST_CASE("an error body served with status 200 is an API error") {
    MockServer server;
    server.set_scenario(Scenario::error_in_200);
    Sandbox sandbox(server.port());
    sandbox.store_api_key();

    const RunResult result = sandbox.run(ask("question"));

    CHECK(result.code == exit_api);
    CHECK(result.err.contains("provider_error"));
    CHECK(server.post_count() == 1);
    CHECK(result_outcome(sandbox) == "error");
}

TEST_CASE("an error in the middle of the stream keeps the partial answer and its usage") {
    MockServer server;
    server.set_scenario(Scenario::midstream_error);
    Sandbox sandbox(server.port());
    sandbox.store_api_key();

    const RunResult result = sandbox.run(ask("question"));

    CHECK(result.code == exit_api);
    CHECK(result.err.contains("provider_error"));
    CHECK(sandbox.only_answer() == "Partial");
    CHECK_FALSE(line_starting(result.err, "openrouter-cli: cost $0.0031").empty());
    CHECK(server.generation_count() == 0);
    CHECK(server.post_count() == 1);
    CHECK(result_outcome(sandbox) == "error");
}

TEST_CASE("a stream that ends before the model finishes is incomplete") {
    MockServer server;
    server.set_scenario(Scenario::early_eof);
    Sandbox sandbox(server.port());
    sandbox.store_api_key();

    const RunResult result = sandbox.run(ask("question"));

    CHECK(result.code == exit_incomplete);
    CHECK(result.err.contains("ended before the model finished"));
    CHECK(sandbox.only_answer() == "Hello");
    CHECK(server.post_count() == 1);
    CHECK(result_outcome(sandbox) == "incomplete");
}

TEST_CASE("an output budget spent on reasoning is explained") {
    MockServer server;
    server.set_scenario(Scenario::length_empty);
    Sandbox sandbox(server.port());
    sandbox.store_api_key();

    const RunResult result = sandbox.run(ask("question"));

    CHECK(result.code == exit_incomplete);
    CHECK(result.err.contains("output budget ran out"));
    CHECK(result.err.contains("8000 reasoning tokens"));
    CHECK(result.out.empty());
    CHECK(server.post_count() == 1);
    CHECK(result_outcome(sandbox) == "incomplete");
}

TEST_CASE("a secret in an attached file stops the call before anything is sent") {
    MockServer server;
    Sandbox sandbox(server.port());
    sandbox.store_api_key();
    const fs::path notes = sandbox.work() / "notes.txt";
    write_text(notes, "first line\nkey " + mock::fake_api_key() + "\n");

    const RunResult result = sandbox.run({"-m", std::string(alpha), "-f", notes.string(), "q"});

    CHECK(result.code == exit_refused);
    CHECK(result.err.contains("notes.txt:2"));
    CHECK_FALSE(result.err.contains(mock::fake_api_key()));
    CHECK(server.post_count() == 0);
    CHECK(sandbox.records_of("pending").empty());
}

TEST_CASE("a cost log that cannot be written stops the call before anything is sent") {
    MockServer server;
    Sandbox sandbox(server.port());
    sandbox.store_api_key();
    for (fs::path dir = sandbox.cost_log(); dir != sandbox.home(); dir = dir.parent_path()) {
        make_private_dir(dir);
    }

    const RunResult result = sandbox.run(ask("question"));

    CHECK(result.code == exit_refused);
    CHECK(result.err.contains("nothing was sent"));
    CHECK(sandbox.answer_files().empty());
    CHECK(server.post_count() == 0);
}

TEST_CASE("a session carries completed turns only and pins its model and system text") {
    MockServer server;
    Sandbox sandbox(server.port());
    sandbox.store_api_key();

    const RunResult first = sandbox.run({"--session", "s1", "-m", std::string(alpha), "q1"});
    REQUIRE(first.code == exit_ok);
    const json first_body = server.last_post();
    REQUIRE(first_body.contains("session_id"));
    const std::string session_id = text_at(first_body, "session_id");

    const RunResult second = sandbox.run({"--session", "s1", "q2"});
    REQUIRE(second.code == exit_ok);
    const json second_body = server.last_post();
    CHECK(text_at(second_body, "model") == alpha);
    CHECK(text_at(second_body, "session_id") == session_id);
    const json& messages = second_body.at("messages");
    REQUIRE(messages.size() == 4U);
    CHECK(text_at(messages.at(0), "role") == "system");
    CHECK(text_at(messages.at(1), "role") == "user");
    CHECK(text_at(messages.at(1), "content") == "q1");
    CHECK(text_at(messages.at(2), "role") == "assistant");
    CHECK(text_at(messages.at(2), "content") == "Hello world");
    CHECK(text_at(messages.at(3), "content") == "q2");

    server.set_scenario(Scenario::early_eof);
    CHECK(sandbox.run({"--session", "s1", "q3"}).code == exit_incomplete);

    server.set_scenario(Scenario::success);
    REQUIRE(sandbox.run({"--session", "s1", "q4"}).code == exit_ok);
    const json after_failure = server.last_post().at("messages");
    REQUIRE(after_failure.size() == 6U);
    CHECK(text_at(after_failure.at(3), "content") == "q2");
    CHECK(text_at(after_failure.at(5), "content") == "q4");

    CHECK(sandbox.run({"--session", "s1", "-m", "test/beta-2", "q5"}).code == exit_usage);
    CHECK(sandbox.run({"--session", "s1", "--role", "plain", "q5"}).code == exit_usage);
    CHECK(sandbox.run({"--session", "s1", "-s", "extra system text", "q5"}).code == exit_usage);
    CHECK(server.post_count() == 4);

    const RunResult other_session =
        sandbox.run({"--session", "s2", "-m", "anthropic/claude-test", "q1"});
    CHECK(other_session.code == exit_ok);
    CHECK(server.last_post().contains("session_id"));
    const RunResult no_session = sandbox.run({"-m", "anthropic/claude-test", "q1"});
    CHECK(no_session.code == exit_ok);
    CHECK_FALSE(server.last_post().contains("session_id"));
    CHECK(server.post_count() == 6);

    // Prompt caching is left to OpenRouter and the provider, so no model ever gets cache_control.
    for (const json& body : server.posts()) {
        INFO("model ", text_at(body, "model"));
        CHECK_FALSE(body.contains("cache_control"));
    }
}

TEST_CASE("the query can come from stdin") {
    MockServer server;
    Sandbox sandbox(server.port());
    sandbox.store_api_key();

    const RunResult result = sandbox.run_with(
        Launch{.args = ask("-"), .stdin_text = std::string("question from stdin")});

    CHECK(result.code == exit_ok);
    CHECK(last_user_text(server.last_post()) == "question from stdin");
    CHECK(server.post_count() == 1);
}

TEST_CASE("models commands work without a stored key and spend totals the log") {
    MockServer server;
    Sandbox sandbox(server.port());

    const RunResult found = sandbox.run({"models", "find", "beta"});
    CHECK(found.code == exit_ok);
    CHECK(found.out.contains("test/beta-2"));
    CHECK(found.out.contains("test/beta-3"));

    const RunResult shown = sandbox.run({"models", "show", std::string(alpha)});
    CHECK(shown.code == exit_ok);
    CHECK(shown.out.contains(alpha));
    CHECK(shown.out.contains("2.5"));
    CHECK(shown.out.contains("mockcloud/fp8"));
    CHECK(shown.out.contains("othercloud"));
    CHECK(server.requests_to("/models/test/alpha-1/endpoints").size() == 1U);

    const RunResult empty_list = sandbox.run({"models", "list"});
    CHECK(empty_list.code == exit_ok);
    CHECK(empty_list.out.contains("no saved aliases"));

    const RunResult saved = sandbox.run({"models", "save", "mine", "test/beta-2"});
    CHECK(saved.code == exit_ok);
    const RunResult listed = sandbox.run({"models", "list"});
    CHECK(listed.code == exit_ok);
    CHECK(listed.out.contains("mine -> test/beta-2"));

    for (const RecordedRequest& request : server.requests()) {
        INFO(request.path);
        CHECK(request.authorization.empty());
    }
    CHECK(server.post_count() == 0);

    sandbox.store_api_key();
    REQUIRE(sandbox.run(ask("question")).code == exit_ok);
    const RunResult spend = sandbox.run({"spend"});
    CHECK(spend.code == exit_ok);
    CHECK(spend.out.contains("0.0042"));
    CHECK(server.post_count() == 1);
}

TEST_CASE("a silent stream hits the idle timeout and keeps the partial answer") {
    MockServer server;
    server.set_scenario(Scenario::slow);
    server.set_hold(idle_hold);
    Sandbox sandbox(server.port());
    sandbox.store_api_key();

    const auto started = Clock::now();
    const RunResult result = sandbox.run(ask("question"));
    const auto elapsed = Clock::now() - started;

    CHECK(result.code == exit_api);
    CHECK(result.err.contains("idle timeout"));
    CHECK(elapsed < idle_hold);
    CHECK(sandbox.only_answer() == "Hello");
    CHECK(server.generation_count() == 0);
    CHECK(server.post_count() == 1);
    CHECK(result_outcome(sandbox) == "error");
    CHECK(sandbox.records_of("generation").size() == 1U);
}

TEST_CASE("keepalive comments keep a quiet call alive past the idle limit") {
    MockServer server;
    server.set_scenario(Scenario::drip);
    server.set_hold(drip_hold);
    Sandbox sandbox(server.port());
    sandbox.store_api_key();

    const RunResult result = sandbox.run(ask("question"));

    CHECK(result.code == exit_ok);
    CHECK(result.out == "Hello world\n");
    CHECK(server.post_count() == 1);
}

TEST_CASE("SIGTERM mid-stream keeps the partial answer, and the busy session is refused") {
    MockServer server;
    server.set_scenario(Scenario::gated);
    Sandbox sandbox(server.port());
    sandbox.store_api_key();

    Child child =
        sandbox.spawn(Launch{.args = {"--session", "busy", "-m", std::string(alpha), "q"}});
    REQUIRE(child.wait_for_output("Hello"));

    const RunResult second = sandbox.run({"--session", "busy", "q2"});
    CHECK(second.code == exit_refused);
    CHECK(server.post_count() == 1);

    REQUIRE(::kill(child.pid(), SIGTERM) == 0);
    const int code = child.wait();

    CHECK(code == exit_interrupted);
    CHECK(child.err().contains("interrupted"));
    CHECK(sandbox.only_answer() == "Hello");
    CHECK(result_outcome(sandbox) == "interrupted");
    CHECK(server.generation_count() == 0);
    CHECK(server.post_count() == 1);
}

TEST_CASE("command line grammar") {
    MockServer server;
    Sandbox sandbox(server.port());
    sandbox.store_api_key();

    CHECK(sandbox.run({"models"}).code == exit_usage);
    CHECK(sandbox.run({"account"}).code == exit_usage);
    CHECK(sandbox.run({"auth"}).code == exit_usage);
    CHECK(sandbox.run({"auth", "login"}).code == exit_usage);
    CHECK(sandbox.run({"auth", "logout"}).code == exit_usage);
    CHECK(sandbox.run({"-m", "x", "login"}).code == exit_usage);
    CHECK(sandbox.run({"-m", "x", "logout"}).code == exit_usage);
    CHECK(file_exists(sandbox.api_credential()));
    CHECK(sandbox.run({"-m", "x", "spend"}).code == exit_usage);
    CHECK(sandbox.run({"-p", "mockcloud", "status"}).code == exit_usage);
    CHECK(sandbox.run({}).code == exit_usage);
    CHECK(server.post_count() == 0);

    const RunResult literal = sandbox.run({"-m", std::string(alpha), "--", "models"});
    CHECK(literal.code == exit_ok);
    CHECK(last_user_text(server.last_post()) == "models");
    CHECK(server.post_count() == 1);
}

TEST_CASE("help is shown for -h and --help on every command") {
    MockServer server;
    Sandbox sandbox(server.port());

    struct Page {
        std::vector<std::string> path;
        std::string usage;
    };
    const std::vector<Page> pages{
        {.path = {}, .usage = "  openrouter-cli [OPTIONS] QUERY\n  openrouter-cli COMMAND\n"},
        {.path = {"models"}, .usage = "  openrouter-cli models COMMAND\n"},
        {.path = {"models", "find"}, .usage = "  openrouter-cli models find [OPTIONS] NAME\n"},
        {.path = {"models", "show"}, .usage = "  openrouter-cli models show MODEL\n"},
        {.path = {"models", "save"}, .usage = "  openrouter-cli models save ALIAS MODEL_ID\n"},
        {.path = {"models", "list"}, .usage = "  openrouter-cli models list\n"},
        {.path = {"spend"}, .usage = "  openrouter-cli spend [OPTIONS]\n"},
        {.path = {"login"}, .usage = "  openrouter-cli login [OPTIONS]\n"},
        {.path = {"logout"}, .usage = "  openrouter-cli logout [OPTIONS]\n"},
        {.path = {"auth"}, .usage = "  openrouter-cli auth COMMAND\n"},
        {.path = {"auth", "set"}, .usage = "  openrouter-cli auth set [OPTIONS]\n"},
        {.path = {"status"}, .usage = "  openrouter-cli status\n"},
        {.path = {"account"}, .usage = "  openrouter-cli account COMMAND\n"},
        {.path = {"account", "balance"}, .usage = "  openrouter-cli account balance\n"},
        {.path = {"account", "keys"}, .usage = "  openrouter-cli account keys\n"},
        {.path = {"account", "credits"}, .usage = "  openrouter-cli account credits\n"},
        {.path = {"account", "activity"}, .usage = "  openrouter-cli account activity [OPTIONS]\n"},
    };
    for (const Page& page : pages) {
        INFO(page.usage);
        std::vector<std::string> long_form = page.path;
        long_form.emplace_back("--help");
        std::vector<std::string> short_form = page.path;
        short_form.emplace_back("-h");
        const RunResult by_long = sandbox.run(long_form);
        const RunResult by_short = sandbox.run(short_form);
        CHECK(by_long.code == exit_ok);
        CHECK(by_short.code == exit_ok);
        CHECK(by_long.err.empty());
        CHECK(by_short.out == by_long.out);
        CHECK(by_long.out.find("\nUSAGE\n" + page.usage) != std::string::npos);
        CHECK(by_long.out.find('\x1b') == std::string::npos);
    }

    const std::string root = sandbox.run({"--help"}).out;
    CHECK(root.starts_with("openrouter-cli "));
    CHECK(line_starting(root, "  -h, --help ").ends_with("Show this help and exit"));
    CHECK(line_starting(root, "      --version ").ends_with("Show the version and exit"));
    CHECK(line_starting(root, "  -m, --model MODEL ").ends_with("~author/family-latest, or name"));
    CHECK(root.find("HELP") == std::string::npos);
    CHECK(root.find("VERSION") == std::string::npos);
    CHECK(root.find("\nEXAMPLES\n") != std::string::npos);
    const std::string find_page = sandbox.run({"models", "find", "--help"}).out;
    CHECK(line_starting(find_page, "      --refresh ").ends_with("Refetch the catalog first"));
    CHECK(find_page.find("REFRESH") == std::string::npos);

    for (const std::size_t width : {std::size_t{60}, std::size_t{80}, std::size_t{100}}) {
        INFO(width);
        const RunResult sized = sandbox.run_with(
            Launch{.args = {"--help"}, .extra_env = {std::format("COLUMNS={}", width)}});
        CHECK(sized.code == exit_ok);
        for (const std::string& line : lines_of(sized.out)) {
            CHECK(line.size() <= width);
        }
    }
    // Example commands are never broken, so only they may run past a 40-column page.
    constexpr std::size_t narrow = 40;
    const RunResult narrow_page =
        sandbox.run_with(Launch{.args = {"--help"}, .extra_env = {"COLUMNS=40"}});
    for (const std::string& line : lines_of(narrow_page.out)) {
        if (!line.starts_with("    openrouter-cli ") && !line.starts_with("    git diff ")) {
            CHECK(line.size() <= narrow);
        }
    }

    const RunResult forced =
        sandbox.run_with(Launch{.args = {"--help"}, .extra_env = {"CLICOLOR_FORCE=1"}});
    CHECK(forced.code == exit_ok);
    CHECK(forced.out.find("\x1b[") != std::string::npos);
    CHECK(strip_escapes(forced.out) == root);
    const RunResult refused = sandbox.run_with(
        Launch{.args = {"--help"}, .extra_env = {"NO_COLOR=1", "CLICOLOR_FORCE=1"}});
    CHECK(refused.out.find('\x1b') == std::string::npos);
    CHECK(refused.out == root);

    CHECK(server.requests().empty());
}

TEST_CASE("a closed stdout still leaves a complete answer file") {
    MockServer server;
    Sandbox sandbox(server.port());
    sandbox.store_api_key();

    const RunResult result =
        sandbox.run_with(Launch{.args = ask("question"), .closed_stdout = true});

    CHECK(result.code == exit_ok);
    CHECK(sandbox.only_answer() == "Hello world");
    CHECK(result_outcome(sandbox) == "complete");
    CHECK(server.post_count() == 1);
}

// ----- exit 9: the call completed but saving failed -----------------------------------------

TEST_CASE("an answer file that cannot be written after a completed call exits 9") {
    MockServer server;
    Sandbox sandbox(server.port());
    sandbox.store_api_key();
    // Keep catalog publication outside the file-size fault being injected into response writes.
    REQUIRE(sandbox.run({"models", "find", "alpha"}).code == exit_ok);
    server.set_scenario(Scenario::large);

    // A pipe isolates stdout from RLIMIT_FSIZE; drain concurrently without assuming its capacity.
    std::array<int, 2> ends{-1, -1};
    REQUIRE(::pipe2(ends.data(), O_CLOEXEC) == 0);
    UniqueFd reader(ends.front());
    UniqueFd writer(ends.back());
    constexpr std::size_t max_capture_bytes = mock::large_answer_bytes + 1;
    std::string captured;
    captured.reserve(max_capture_bytes);
    bool capture_ok = true;
    std::jthread collector([&, read_end = std::move(reader)](const std::stop_token& stop) {
        constexpr int wait_milliseconds = 100;
        constexpr std::size_t chunk_bytes = 4096;
        std::array<char, chunk_bytes> buffer{};
        while (!stop.stop_requested()) {
            pollfd input{.fd = read_end.get(), .events = POLLIN, .revents = 0};
            const int ready = ::poll(&input, 1, wait_milliseconds);
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            if (ready < 0) {
                capture_ok = false;
                return;
            }
            if (ready == 0) {
                continue;
            }
            const auto count = ::read(read_end.get(), buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0) {
                capture_ok = false;
                return;
            }
            if (count == 0) {
                return;
            }
            const auto size = static_cast<std::size_t>(count);
            if (size > max_capture_bytes - captured.size()) {
                capture_ok = false;
                return;
            }
            captured.append(buffer.data(), size);
        }
    });
    Child child = sandbox.spawn(Launch{.args = ask("question"),
                                       .stdout_descriptor = writer.get(),
                                       .file_size_limit = answer_file_limit});
    writer.reset();
    const int code = child.wait();
    collector.join();
    INFO(code);
    INFO(child.err());
    REQUIRE(capture_ok);
    const RunResult result{.code = code, .out = std::move(captured), .err = child.err()};

    CHECK(result.code == exit_not_saved);
    CHECK(result.err.contains("the answer file"));
    CHECK(result.out == std::string(mock::large_answer_bytes, 'x') + "\n");
    const std::string retained(static_cast<std::size_t>(answer_file_limit), 'x');
    CHECK(sandbox.only_answer() == retained);
    const auto results = sandbox.records_of("result");
    REQUIRE(results.size() == 1U);
    CHECK(text_at(results.front(), "outcome") == "complete");
    CHECK(results.front().at("cost").get<double>() == doctest::Approx(0.0042));
    CHECK(server.generation_count() == 0);
    CHECK(server.post_count() == 1);
}

TEST_CASE("a result record that cannot be written after a completed call exits 9") {
    MockServer server;
    server.set_scenario(Scenario::gated);
    Sandbox sandbox(server.port());
    sandbox.store_api_key();

    Child child = sandbox.spawn(Launch{.args = ask("question")});
    REQUIRE(child.wait_for_output("Hello"));
    fs::permissions(sandbox.cost_log(), fs::perms::owner_read);
    server.release_gate();
    const int code = child.wait();

    CHECK(code == exit_not_saved);
    CHECK(child.out() == "Hello world\n");
    CHECK(sandbox.only_answer() == "Hello world");
    CHECK(sandbox.records_of("result").empty());
    CHECK(server.post_count() == 1);
}

TEST_CASE("a session that cannot be saved after a completed call exits 9") {
    MockServer server;
    server.set_scenario(Scenario::gated);
    Sandbox sandbox(server.port());
    sandbox.store_api_key();

    Child child =
        sandbox.spawn(Launch{.args = {"--session", "s9", "-m", std::string(alpha), "question"}});
    REQUIRE(child.wait_for_output("Hello"));
    const fs::path session_file = sandbox.state_dir() / "sessions" / "s9.json";
    std::error_code ignored;
    fs::remove(session_file, ignored);
    fs::create_directories(session_file / "blocker");
    server.release_gate();
    const int code = child.wait();

    CHECK(code == exit_not_saved);
    CHECK(child.out() == "Hello world\n");
    CHECK(child.err().contains("session"));
    CHECK(result_outcome(sandbox) == "complete");
    CHECK(server.post_count() == 1);
}

// ----- spend --resolve -------------------------------------------------------------------------

TEST_CASE("spend reads empty logs and records across read-buffer boundaries") {
    MockServer server;
    Sandbox sandbox(server.port());
    const std::string call_id(32, 'c');
    log_unpriced_call(sandbox, call_id, "gen-unpriced");
    const std::string records = read_text(sandbox.cost_log());
    const RunResult baseline = sandbox.run({"spend"});
    REQUIRE(baseline.code == exit_ok);
    REQUIRE(baseline.out.contains(call_id));

    constexpr std::size_t chunk_bytes = 65536;
    for (const std::size_t bytes :
         {chunk_bytes - 1, chunk_bytes, chunk_bytes + 1, chunk_bytes * 2}) {
        INFO(bytes);
        REQUIRE(records.size() < bytes);
        std::string contents(bytes - records.size(), '\n');
        contents += records;
        write_text(sandbox.cost_log(), contents);
        const RunResult result = sandbox.run({"spend"});
        CHECK(result.code == exit_ok);
        CHECK(result.out == baseline.out);
    }

    write_text(sandbox.cost_log(), "");
    const RunResult empty = sandbox.run({"spend"});
    CHECK(empty.code == exit_ok);
    CHECK(empty.out.contains("no calls logged yet"));
    CHECK(server.requests().empty());
}

TEST_CASE("spend --resolve prices calls whose charge has appeared and keeps the rest") {
    MockServer server;
    server.set_route("/generation", [](const RecordedRequest& request) {
        if (request.param("id") == "gen-found") {
            return Reply{.status = http_ok,
                         .body = R"({"data":{"id":"gen-found","total_cost":0.0123,)"
                                 R"("provider_name":"MockProvider","model":"test/alpha-1",)"
                                 R"("native_tokens_prompt":10,"native_tokens_completion":5}})"};
        }
        return Reply{.status = http_not_found,
                     .body = R"({"error":{"code":404,"message":"Generation not found"}})"};
    });
    Sandbox sandbox(server.port());
    sandbox.store_api_key();
    const std::string found_call(32, 'a');
    const std::string missing_call(32, 'b');
    log_unpriced_call(sandbox, found_call, "gen-found");
    log_unpriced_call(sandbox, missing_call, "gen-missing");

    const RunResult first = sandbox.run({"spend", "--resolve"});

    CHECK(first.code == exit_ok);
    CHECK(first.out.contains(found_call));
    const std::vector<json> resolved = sandbox.records_of("resolved");
    REQUIRE(resolved.size() == 1U);
    CHECK(text_at(resolved.front(), "call_id") == found_call);
    CHECK(server.generation_count() == 2);
    for (const RecordedRequest& lookup : server.requests_to("/generation")) {
        CHECK(lookup.authorization == bearer(inference_key()));
    }

    const RunResult second = sandbox.run({"spend", "--resolve"});

    CHECK(second.code == exit_ok);
    const std::vector<RecordedRequest> lookups = server.requests_to("/generation");
    REQUIRE(lookups.size() == 3U);
    CHECK(lookups.back().param("id") == "gen-missing");
    CHECK(sandbox.records_of("resolved").size() == 1U);
    CHECK(server.post_count() == 0);
}

// ----- auth ------------------------------------------------------------------------------------

TEST_CASE("auth set stores the key encrypted and asks use it") {
    MockServer server;
    Sandbox sandbox(server.port());

    const RunResult stored = sandbox.auth_set(inference_key());

    CHECK(stored.code == exit_ok);
    REQUIRE(file_exists(sandbox.api_credential()));
    const std::string ciphertext = read_text(sandbox.api_credential());
    CHECK(ciphertext.starts_with("FAKECRED:openrouter-cli-api-key:"));
    CHECK_FALSE(ciphertext.contains(inference_key()));
    CHECK_FALSE(stored.all().contains(inference_key()));
    const std::vector<RecordedRequest> checks = server.requests_to("/key");
    REQUIRE(checks.size() == 1U);
    CHECK(checks.front().authorization == bearer(inference_key()));

    REQUIRE(sandbox.run(ask("question")).code == exit_ok);
    CHECK(server.last_authorization() == bearer(inference_key()));
}

TEST_CASE("auth set keeps a stored key unless --replace passes verification") {
    MockServer server;
    const std::string accepted = inference_key();
    const std::string rejected = mock::fake_key('4');
    const std::string replacement = mock::fake_key('5');
    server.set_route("/key", [rejected](const RecordedRequest& request) {
        if (request.authorization == bearer(rejected)) {
            return Reply{.status = http_unauthorized,
                         .body = R"({"error":{"code":401,"message":"User not found."}})"};
        }
        return data_reply(mock::key_data());
    });
    Sandbox sandbox(server.port());
    sandbox.store_api_key();
    const std::string original = read_text(sandbox.api_credential());

    const RunResult again = sandbox.auth_set(replacement);
    CHECK(again.code == exit_refused);
    CHECK(again.err.contains("--replace"));
    CHECK(read_text(sandbox.api_credential()) == original);

    const RunResult bad = sandbox.auth_set(rejected, {"--replace"});
    CHECK(bad.code != exit_ok);
    CHECK(read_text(sandbox.api_credential()) == original);
    REQUIRE(sandbox.run(ask("question")).code == exit_ok);
    CHECK(server.last_authorization() == bearer(accepted));

    const RunResult good = sandbox.auth_set(replacement, {"--replace"});
    CHECK(good.code == exit_ok);
    REQUIRE(sandbox.run(ask("question")).code == exit_ok);
    CHECK(server.last_authorization() == bearer(replacement));
}

TEST_CASE("auth set stores an API key only when /key says is_management_key is false") {
    struct Case {
        const char* name;
        json value; // null: field left out
        int status;
    };
    const std::vector<Case> cases{
        {.name = "true", .value = true, .status = http_ok},
        {.name = "missing", .value = nullptr, .status = http_ok},
        {.name = "string", .value = "false", .status = http_ok},
        {.name = "number", .value = 0, .status = http_ok},
        {.name = "401", .value = false, .status = http_unauthorized},
        {.name = "403", .value = false, .status = http_forbidden},
        {.name = "false", .value = false, .status = http_ok},
    };
    for (const Case& entry : cases) {
        INFO("is_management_key ", entry.name);
        MockServer server;
        server.set_route("/key", [entry](const RecordedRequest&) {
            json data = mock::key_data();
            data.erase("is_management_key");
            if (!entry.value.is_null()) {
                data["is_management_key"] = entry.value;
            }
            return Reply{.status = entry.status, .body = json{{"data", data}}.dump()};
        });
        Sandbox sandbox(server.port());

        const RunResult result = sandbox.auth_set(inference_key());

        const bool should_store = std::string_view(entry.name) == "false";
        CHECK((result.code == exit_ok) == should_store);
        CHECK(file_exists(sandbox.api_credential()) == should_store);
        if (std::string_view(entry.name) == "true") {
            CHECK(result.err.contains("auth set --management"));
        }
        CHECK(server.post_count() == 0);
    }
}

TEST_CASE("auth set --management verifies with /credits and never touches inference endpoints") {
    MockServer server;
    Sandbox sandbox(server.port());

    sandbox.store_management_key();

    CHECK(file_exists(sandbox.management_credential()));
    CHECK_FALSE(file_exists(sandbox.api_credential()));
    const std::vector<RecordedRequest> checks = server.requests_to("/credits");
    REQUIRE(checks.size() == 1U);
    CHECK(checks.front().authorization == bearer(management_key()));
    CHECK(server.requests_to("/key").empty());
}

TEST_CASE("login exchanges the pasted code without an Authorization header") {
    MockServer server;
    Sandbox sandbox(server.port());

    const RunResult login =
        sandbox.run_with(Launch{.args = {"login"}, .stdin_text = "mock-code\n"});

    INFO(login.err);
    CHECK(login.code == exit_ok);
    const std::vector<RecordedRequest> exchanges = server.requests_to("/auth/keys");
    REQUIRE(exchanges.size() == 1U);
    const RecordedRequest& exchange = exchanges.front();
    CHECK(exchange.method == "POST");
    CHECK(exchange.authorization.empty());
    const json body = json::parse(exchange.body, nullptr, false);
    REQUIRE(body.is_object());
    CHECK(text_at(body, "code") == "mock-code");
    CHECK(text_at(body, "code_challenge_method") == "S256");
    const std::string verifier = text_at(body, "code_verifier");
    CHECK(verifier.size() == verifier_length);

    const auto challenge = orcli::pkce_challenge(verifier);
    REQUIRE(challenge.has_value());
    CHECK(login.out.contains("https://openrouter.ai/auth?"));
    CHECK(login.out.contains("code_challenge=" + *challenge));
    CHECK(login.out.contains("code_challenge_method=S256"));
    CHECK(login.out.contains("key_label=openrouter-cli-on-"));
    CHECK_FALSE(login.out.contains("%20"));
    CHECK_FALSE(login.out.contains("callback_url"));
    CHECK_FALSE(login.all().contains(mock::login_key()));
    CHECK_FALSE(login.all().contains("mock-code"));
    CHECK_FALSE(login.out.contains("Paste the code here"));
    const auto instructions = login.err.find("3. Copy the code");
    const auto prompt = login.err.find("Paste the code here, then press Enter: ");
    const auto progress = login.err.find("Completing sign-in with OpenRouter...");
    const auto success = login.err.find("logged in;");
    REQUIRE(instructions != std::string::npos);
    REQUIRE(prompt != std::string::npos);
    REQUIRE(progress != std::string::npos);
    REQUIRE(success != std::string::npos);
    CHECK(instructions < prompt);
    CHECK(prompt < progress);
    CHECK(progress < success);

    const std::vector<RecordedRequest> checks = server.requests_to("/key");
    REQUIRE(checks.size() == 1U);
    CHECK(checks.front().authorization == bearer(mock::login_key()));
    REQUIRE(file_exists(sandbox.api_credential()));
    REQUIRE(sandbox.run(ask("question")).code == exit_ok);
    CHECK(server.last_authorization() == bearer(mock::login_key()));
}

TEST_CASE("login shows its prompt before the user supplies a code") {
    MockServer server;
    Sandbox sandbox(server.port());
    const fs::path input_path = sandbox.root() / "login-input";
    REQUIRE(::mkfifo(input_path.c_str(), private_file_mode) == 0);
    // Keep a writer open so login waits for input instead of seeing EOF.
    const UniqueFd input = open_fd(input_path, O_RDWR | O_NONBLOCK);
    Child child = sandbox.spawn(Launch{.args = {"login"}, .stdin_path = input_path});
    constexpr std::string_view prompt = "Paste the code here, then press Enter: ";
    const auto deadline = Clock::now() + output_wait;
    while (!child.err().contains(prompt) && Clock::now() < deadline) {
        std::this_thread::sleep_for(poll_interval);
    }
    REQUIRE(child.err().ends_with(prompt));
    CHECK(child.out().contains("https://openrouter.ai/auth?"));
    CHECK(server.requests().empty());
    constexpr std::string_view code = "mock-code\n";
    REQUIRE(::write(input.get(), code.data(), code.size()) == static_cast<ssize_t>(code.size()));
    CHECK(child.wait() == exit_ok);
    CHECK(child.err().contains("logged in;"));
}

TEST_CASE("login reports blank input and EOF beneath the prompt without sending") {
    for (const std::string& input : {std::string(), std::string("\n"), std::string(" \t\r\n")}) {
        MockServer server;
        Sandbox sandbox(server.port());
        const RunResult login = sandbox.run_with(Launch{.args = {"login"}, .stdin_text = input});
        CHECK(login.code == exit_usage);
        CHECK(login.err.contains("Paste the code here, then press Enter: \nopenrouter-cli:"));
        CHECK(login.err.contains("no code was pasted"));
        CHECK_FALSE(login.err.contains("Completing sign-in"));
        CHECK(server.requests().empty());
        CHECK_FALSE(file_exists(sandbox.api_credential()));
    }
}

TEST_CASE("a plaintext key in .env stops every command except login, logout and auth set") {
    MockServer server;
    Sandbox sandbox(server.port());
    sandbox.store_api_key();
    const std::string plaintext = mock::fake_key('7');

    const std::vector<std::string> env_variants{
        "OPENROUTER_API_KEY=" + plaintext + "\n",
        "OPENROUTER_API_KEY=" + plaintext + "\nOPENROUTER_API_KEY=\n",
        "OPENROUTER_MANAGEMENT_KEY=\"" + plaintext + "\"\n",
    };
    for (const std::string& variant : env_variants) {
        INFO("env variant ", variant.substr(0, variant.find('=')),
             variant.ends_with("=\n") ? " followed by an empty duplicate" : "");
        sandbox.write_env(variant);
        for (const std::vector<std::string>& args :
             std::vector<std::vector<std::string>>{ask("question"),
                                                   {"status"},
                                                   {"models", "list"},
                                                   {"models", "find", "beta"},
                                                   {"spend"},
                                                   {"account", "balance"}}) {
            INFO("command ", args.front());
            const RunResult refused = sandbox.run(args);
            CHECK(refused.code == exit_config);
            CHECK(refused.err.contains("line 4"));
            CHECK_FALSE(refused.all().contains(plaintext));
        }
    }
    CHECK(server.post_count() == 0);

    sandbox.write_env("OPENROUTER_API_KEY=" + plaintext + "\n");
    const RunResult replaced = sandbox.auth_set(inference_key(), {"--replace"});
    CHECK(replaced.code == exit_ok);
    CHECK(replaced.err.contains("line 4"));
    CHECK_FALSE(replaced.all().contains(plaintext));

    sandbox.write_env("");
    REQUIRE(sandbox.run(ask("question")).code == exit_ok);
    CHECK(server.last_authorization() == bearer(inference_key()));
}

TEST_CASE("the management key never reaches an inference endpoint") {
    MockServer server;
    server.set_scenario(Scenario::success_no_cost);
    Sandbox sandbox(server.port());
    sandbox.store_api_key();
    sandbox.store_management_key();

    REQUIRE(sandbox.run(ask("question")).code == exit_ok);
    CHECK(sandbox.run({"status"}).code == exit_ok);
    CHECK(sandbox.run({"account", "balance"}).code == exit_ok);
    CHECK(sandbox.run({"account", "keys"}).code == exit_ok);
    CHECK(sandbox.run({"account", "credits"}).code == exit_ok);
    CHECK(sandbox.run({"account", "activity"}).code == exit_ok);

    const std::vector<std::string_view> inference_paths{"/chat/completions", "/key", "/generation"};
    const std::vector<std::string_view> management_paths{"/keys", "/credits", "/activity",
                                                         "/organization/members"};
    for (const RecordedRequest& request : server.requests()) {
        INFO(request.method, " ", request.path);
        if (std::ranges::contains(inference_paths, std::string_view(request.path))) {
            CHECK(request.authorization == bearer(inference_key()));
        } else if (std::ranges::contains(management_paths, std::string_view(request.path))) {
            CHECK(request.authorization == bearer(management_key()));
        } else {
            CHECK(request.authorization.empty());
        }
    }
    CHECK(server.post_count() == 1);
    CHECK(server.generation_count() > 0);
}

// ----- logout ----------------------------------------------------------------------------------

TEST_CASE("logout removes both keys, and logging out twice is fine") {
    MockServer server;
    Sandbox sandbox(server.port());
    sandbox.store_api_key();
    sandbox.store_management_key();
    const std::size_t requests_before = server.requests().size();

    const RunResult logout = sandbox.run({"logout"});

    CHECK(logout.code == exit_ok);
    CHECK(logout.all().contains("https://openrouter.ai/settings/keys"));
    CHECK_FALSE(file_exists(sandbox.api_credential()));
    CHECK_FALSE(file_exists(sandbox.management_credential()));
    CHECK(server.requests().size() == requests_before);

    const RunResult asked = sandbox.run(ask("question"));
    CHECK(asked.code == exit_config);
    CHECK(asked.err.contains("openrouter-cli login`"));
    CHECK(server.post_count() == 0);

    const RunResult status = sandbox.run({"status"});
    CHECK(status.code == exit_ok);
    CHECK(status.out.contains("logged in: no"));

    const RunResult again = sandbox.run({"logout"});
    CHECK(again.code == exit_ok);
    CHECK(again.all().contains("already logged out"));
}

TEST_CASE("logout --management keeps the API key") {
    MockServer server;
    Sandbox sandbox(server.port());
    sandbox.store_api_key();
    sandbox.store_management_key();

    CHECK(sandbox.run({"logout", "--management"}).code == exit_ok);

    CHECK(file_exists(sandbox.api_credential()));
    CHECK_FALSE(file_exists(sandbox.management_credential()));
    CHECK(sandbox.run(ask("question")).code == exit_ok);
    CHECK(sandbox.run({"account", "credits"}).code == exit_config);
}

TEST_CASE("logout works with a plaintext .env line, without a credential tool and with a "
          "corrupt credential file") {
    MockServer server;
    Sandbox sandbox(server.port());

    sandbox.store_api_key();
    sandbox.write_env("OPENROUTER_API_KEY=" + mock::fake_key('7') + "\n");
    const RunResult with_plaintext = sandbox.run({"logout"});
    CHECK(with_plaintext.code == exit_ok);
    CHECK(with_plaintext.err.contains("line 4"));
    CHECK_FALSE(file_exists(sandbox.api_credential()));
    sandbox.write_env("");

    sandbox.store_api_key();
    const RunResult without_tool =
        sandbox.run_with(Launch{.args = {"logout"}, .creds_tool = false});
    CHECK(without_tool.code == exit_ok);
    CHECK_FALSE(file_exists(sandbox.api_credential()));

    sandbox.store_api_key();
    write_text(sandbox.api_credential(), "not a credential");
    const RunResult corrupt = sandbox.run({"logout"});
    CHECK(corrupt.code == exit_ok);
    CHECK_FALSE(file_exists(sandbox.api_credential()));
}

TEST_CASE("a credential that fails to decrypt is named and exits 7") {
    MockServer server;
    Sandbox sandbox(server.port());
    sandbox.store_api_key();
    sandbox.store_management_key();

    write_text(sandbox.management_credential(), "not a credential");
    const RunResult management = sandbox.run({"account", "credits"});
    CHECK(management.code == exit_config);
    CHECK(management.err.contains("Management key"));
    CHECK(sandbox.run({"status"}).code == exit_config);

    write_text(sandbox.api_credential(), "not a credential");
    const RunResult api = sandbox.run(ask("question"));
    CHECK(api.code == exit_config);
    CHECK(api.err.contains("API key"));
    CHECK(sandbox.run({"status"}).code == exit_config);
    CHECK(server.post_count() == 0);
}

// ----- status ----------------------------------------------------------------------------------

TEST_CASE("status without a stored key says so and contacts nobody") {
    MockServer server;
    Sandbox sandbox(server.port());

    const RunResult status = sandbox.run({"status"});

    CHECK(status.code == exit_ok);
    CHECK(status.out.contains("logged in: no"));
    CHECK(status.all().contains("openrouter-cli login`"));
    CHECK(server.requests().empty());
}

TEST_CASE("status shows the key's identity and how to add a Management key") {
    MockServer server;
    Sandbox sandbox(server.port());
    sandbox.store_api_key();

    const RunResult status = sandbox.run({"status"});

    CHECK(status.code == exit_ok);
    CHECK(status.out.contains("logged in: yes"));
    CHECK(status.out.contains(mock::creator_user_id));
    CHECK(status.out.contains(mock::masked_key_label));
    CHECK(status.out.contains("org_test_1"));
    CHECK(status.out.contains("ws_test_1"));
    CHECK(status.out.contains("management key: not stored"));
    CHECK(status.out.contains("auth set --management"));
    CHECK(status.out.contains("systemd-creds --user"));
    CHECK_FALSE(status.all().contains(inference_key()));
    CHECK(server.requests_to("/organization/members").empty());
}

TEST_CASE("status finds the key's creator among the organization members on a later page") {
    MockServer server;
    server.set_route("/organization/members", [](const RecordedRequest& request) {
        const int offset = offset_param(request);
        if (offset == 0) {
            return data_reply(other_members(0, members_page_size));
        }
        if (offset == members_page_size) {
            json page = other_members(offset, 2);
            page.push_back(mock::member(mock::creator_user_id, "pat@example.com"));
            return data_reply(page);
        }
        return data_reply(json::array());
    });
    Sandbox sandbox(server.port());
    sandbox.store_api_key();
    sandbox.store_management_key();

    const RunResult status = sandbox.run({"status"});

    CHECK(status.code == exit_ok);
    CHECK(status.out.contains("logged in: yes"));
    CHECK(status.out.contains("pat@example.com"));
    CHECK(status.out.contains("Pat"));
    CHECK(status.out.contains("Lee"));
    CHECK(status.out.contains("org:admin"));
    CHECK(status.out.contains("$12.5000"));
    CHECK(server.requests_to("/organization/members").size() >= 2U);
}

TEST_CASE("status reports members it cannot search as not available or lookup failed") {
    struct Case {
        const char* name;
        mock::Responder responder;
        int exit;
        const char* text;
    };
    const std::vector<Case> cases{
        {.name = "404",
         .responder = [](const RecordedRequest&) { return Reply{.status = http_not_found}; },
         .exit = exit_ok,
         .text = "not available"},
        {.name = "403",
         .responder = [](const RecordedRequest&) { return Reply{.status = http_forbidden}; },
         .exit = exit_ok,
         .text = "not available"},
        {.name = "no match",
         .responder =
             [](const RecordedRequest& request) {
                 const int offset = offset_param(request);
                 if (offset == 0) {
                     return data_reply(other_members(0, members_page_size));
                 }
                 return data_reply(offset == members_page_size ? other_members(offset, 3)
                                                               : json::array());
             },
         .exit = exit_ok,
         .text = "not available"},
        {.name = "malformed page",
         .responder =
             [](const RecordedRequest&) {
                 return Reply{.status = http_ok, .body = R"({"data":[{"id":7}]})"};
             },
         .exit = exit_api,
         .text = "lookup failed"},
        {.name = "500",
         .responder =
             [](const RecordedRequest&) {
                 return Reply{.status = http_server_error,
                              .body = R"({"error":{"code":500,"message":"boom"}})"};
             },
         .exit = exit_api,
         .text = "lookup failed"},
    };
    for (const Case& entry : cases) {
        INFO("members ", entry.name);
        MockServer server;
        server.set_route("/organization/members", entry.responder);
        Sandbox sandbox(server.port());
        sandbox.store_api_key();
        sandbox.store_management_key();

        const RunResult status = sandbox.run({"status"});

        CHECK(status.code == entry.exit);
        CHECK(status.all().contains(entry.text));
        CHECK(status.out.contains("logged in: yes"));
        CHECK(status.out.contains("$12.5000"));
    }
}

TEST_CASE("status stops at the members page cap and reports the lookup as failed") {
    MockServer server;
    server.set_route("/organization/members", [](const RecordedRequest& request) {
        return data_reply(other_members(offset_param(request), members_page_size));
    });
    Sandbox sandbox(server.port());
    sandbox.store_api_key();
    sandbox.store_management_key();

    const RunResult status = sandbox.run({"status"});

    CHECK(status.code == exit_api);
    CHECK(status.all().contains("lookup failed"));
    CHECK_FALSE(status.all().contains("not available"));
    CHECK(server.requests_to("/organization/members").size() ==
          static_cast<std::size_t>(members_page_cap));
}

TEST_CASE("status skips the members lookup without a creator id but still shows credits") {
    MockServer server;
    server.set_route("/key", [](const RecordedRequest&) {
        json data = mock::key_data();
        data["creator_user_id"] = nullptr;
        return data_reply(data);
    });
    Sandbox sandbox(server.port());
    sandbox.store_api_key();
    sandbox.store_management_key();

    const RunResult status = sandbox.run({"status"});

    CHECK(status.code == exit_ok);
    CHECK(status.out.contains("logged in: yes"));
    CHECK(status.out.contains("not available"));
    CHECK(status.out.contains("$12.5000"));
    CHECK(server.requests_to("/organization/members").empty());
}

TEST_CASE("status shows null organization, workspace and member names as absent") {
    MockServer server;
    server.set_route("/key", [](const RecordedRequest&) {
        json data = mock::key_data();
        data["organization_id"] = nullptr;
        data["workspace_id"] = nullptr;
        return data_reply(data);
    });
    server.set_route("/organization/members", [](const RecordedRequest& request) {
        if (offset_param(request) != 0) {
            return data_reply(json::array());
        }
        json creator = mock::member(mock::creator_user_id, "solo@example.com");
        creator["first_name"] = nullptr;
        creator["last_name"] = nullptr;
        return data_reply(json::array({creator}));
    });
    Sandbox sandbox(server.port());
    sandbox.store_api_key();
    sandbox.store_management_key();

    const RunResult status = sandbox.run({"status"});

    CHECK(status.code == exit_ok);
    CHECK(status.out.contains("individual account"));
    CHECK(status.out.contains("solo@example.com"));
    CHECK(status.out.contains("n/a"));
}

TEST_CASE("status follows the real HTTP status of /key whatever the body says") {
    struct Case {
        int status;
        const char* body;
    };
    const std::string valid_body = json{{"data", mock::key_data()}}.dump();
    const std::vector<Case> cases{
        {.status = http_unauthorized, .body = ""},
        {.status = http_unauthorized, .body = "not json"},
        {.status = http_unauthorized, .body = valid_body.c_str()},
        {.status = http_forbidden, .body = ""},
        {.status = http_forbidden, .body = R"({"error":{"code":401,"message":"bad key"}})"},
        {.status = http_not_found, .body = valid_body.c_str()},
        {.status = http_server_error, .body = valid_body.c_str()},
    };
    for (const Case& entry : cases) {
        INFO("HTTP ", entry.status, " body ", entry.body);
        MockServer server;
        Sandbox sandbox(server.port());
        sandbox.store_api_key();
        server.set_route("/key", [entry](const RecordedRequest&) {
            return Reply{.status = entry.status, .body = entry.body};
        });

        const RunResult status = sandbox.run({"status"});

        CHECK(status.code == exit_api);
        CHECK_FALSE(status.out.contains("logged in: yes"));
        CHECK(status.all().contains("key rejected") == (entry.status == http_unauthorized));
        if (entry.status == http_unauthorized) {
            CHECK(status.all().contains("openrouter-cli login --replace"));
        }
    }
}

TEST_CASE("status keeps the login state when only the Management key is rejected") {
    MockServer server;
    Sandbox sandbox(server.port());
    sandbox.store_api_key();
    sandbox.store_management_key();
    server.set_route("/credits", [](const RecordedRequest&) {
        return Reply{.status = http_unauthorized,
                     .body = R"({"error":{"code":401,"message":"Invalid management key"}})"};
    });
    server.set_route("/organization/members", [](const RecordedRequest&) {
        return Reply{.status = http_unauthorized,
                     .body = R"({"error":{"code":401,"message":"Invalid management key"}})"};
    });

    const RunResult status = sandbox.run({"status"});

    CHECK(status.code == exit_api);
    CHECK(status.out.contains("logged in: yes"));
    CHECK(status.out.contains("management key: stored but rejected"));
    CHECK(status.all().contains("auth set --management --replace"));
}

// ----- account ---------------------------------------------------------------------------------

TEST_CASE("account balance and credits show the mocked amounts") {
    MockServer server;
    Sandbox sandbox(server.port());
    sandbox.store_api_key();
    sandbox.store_management_key();

    const RunResult balance = sandbox.run({"account", "balance"});
    CHECK(balance.code == exit_ok);
    CHECK(balance.out.contains(mock::masked_key_label));
    CHECK(balance.out.contains("$0.2500"));
    CHECK(balance.out.contains("$1.5000"));

    const RunResult credits = sandbox.run({"account", "credits"});
    CHECK(credits.code == exit_ok);
    CHECK(credits.out.contains("$20.0000"));
    CHECK(credits.out.contains("$7.5000"));
    CHECK(credits.out.contains("$12.5000"));
}

TEST_CASE("account keys reads every page, disabled and expired keys included") {
    MockServer server;
    Sandbox sandbox(server.port());
    sandbox.store_management_key();

    const RunResult keys = sandbox.run({"account", "keys"});

    CHECK(keys.code == exit_ok);
    for (const char* name : {"ci-runner", "laptop", "old-key", "disabled"}) {
        INFO(name);
        CHECK(keys.out.contains(name));
    }
    const std::vector<RecordedRequest> pages = server.requests_to("/keys");
    REQUIRE(pages.size() == 3U);
    CHECK(offset_param(pages.at(0)) == 0);
    CHECK(pages.at(1).param("offset") == "2");
    CHECK(pages.at(2).param("offset") == "3");
    for (const RecordedRequest& page : pages) {
        CHECK(page.param("include_disabled") == "true");
        CHECK(page.param("include_expired") == "true");
        CHECK(page.authorization == bearer(management_key()));
    }
}

TEST_CASE("account keys prints nothing partial when a later page fails") {
    MockServer server;
    server.set_route("/keys", [](const RecordedRequest& request) {
        if (offset_param(request) == 0) {
            return Reply{.status = http_ok,
                         .body = json{{"data", json::array({json{{"hash", "h1"},
                                                                 {"name", "first-page-key"},
                                                                 {"label", "sk-or-v1-a...0000"},
                                                                 {"disabled", false}}})}}
                                     .dump()};
        }
        return Reply{.status = http_forbidden,
                     .body = R"({"error":{"code":403,"message":"Forbidden"}})"};
    });
    Sandbox sandbox(server.port());
    sandbox.store_management_key();

    const RunResult keys = sandbox.run({"account", "keys"});

    CHECK(keys.code == exit_api);
    CHECK_FALSE(keys.out.contains("first-page-key"));
}

TEST_CASE("account activity sums endpoint rows and filters --days by completed UTC days") {
    MockServer server;
    Sandbox sandbox(server.port());
    sandbox.store_management_key();

    const RunResult recent = sandbox.run({"account", "activity", "--days", "2"});
    CHECK(recent.code == exit_ok);
    CHECK(recent.out.contains("test/alpha-1"));
    CHECK(recent.out.contains("1,234"));
    CHECK(recent.out.contains("Total: 42 requests"));
    CHECK_FALSE(recent.out.contains("test/beta-2"));

    const RunResult week = sandbox.run({"account", "activity", "--days", "7"});
    CHECK(week.code == exit_ok);
    CHECK(week.out.contains("test/beta-2"));
    CHECK(week.out.contains("Total: 44 requests"));

    CHECK(sandbox.run({"account", "activity", "--days", "0"}).code == exit_usage);
    CHECK(sandbox.run({"account", "activity", "--days", "31"}).code == exit_usage);
    CHECK(server.requests_to("/activity").size() == 2U);
}

TEST_CASE("legacy override preserves state while costs remain global") {
    MockServer server;
    Sandbox sandbox(server.port(), true);
    sandbox.store_api_key();
    const auto result = sandbox.run(ask("legacy storage"));
    REQUIRE(result.code == exit_ok);
    CHECK(file_exists(sandbox.project() / "state/credentials/api-key.cred"));
    CHECK(file_exists(sandbox.cost_log()));
    CHECK_FALSE(file_exists(sandbox.project() / "state/costs.jsonl"));
}

TEST_CASE("models show uses one catalog snapshot and only save persists aliases") {
    MockServer server;
    Sandbox sandbox(server.port());
    REQUIRE(sandbox.run({"models", "show", "alpha 1"}).code == exit_ok);
    CHECK(server.requests_to("/models").size() == 1);
    CHECK_FALSE(file_exists(sandbox.state_dir() / "models.json"));
    REQUIRE(sandbox.run({"models", "save", "preferred", std::string(alpha)}).code == exit_ok);
    CHECK(read_text(sandbox.state_dir() / "models.json").contains("preferred"));
}

TEST_CASE("a failed explicit model refresh reports the failure despite a cache") {
    MockServer server;
    Sandbox sandbox(server.port());
    REQUIRE(sandbox.run({"models", "find", "alpha"}).code == exit_ok);
    server.set_route("/models", [](const RecordedRequest&) {
        return Reply{.status = http_ok, .body = "invalid-json"};
    });
    const auto result = sandbox.run({"models", "find", "alpha", "--refresh"});
    CHECK(result.code == exit_api);
    CHECK(result.err.contains("not valid JSON"));
}

TEST_CASE("interrupted idle login preserves the parent's stdin file-status flags") {
    MockServer server;
    Sandbox sandbox(server.port());
    std::array<int, 2> ends{-1, -1};
    REQUIRE(::pipe2(ends.data(), O_CLOEXEC) == 0);
    const UniqueFd read_end(ends.front());
    const UniqueFd write_end(ends.back());
    const int flags = ::fcntl(read_end.get(), F_GETFL);
    REQUIRE(flags >= 0);
    Child child = sandbox.spawn(Launch{.args = {"login"}, .stdin_descriptor = read_end.get()});
    const auto deadline = Clock::now() + output_wait;
    while (!child.err().contains("Paste the code here") && Clock::now() < deadline) {
        std::this_thread::sleep_for(poll_interval);
    }
    REQUIRE(child.err().contains("Paste the code here"));
    REQUIRE(::kill(child.pid(), SIGTERM) == 0);
    CHECK(child.wait() == exit_interrupted);
    CHECK(::fcntl(read_end.get(), F_GETFL) == flags);
    CHECK(server.requests().empty());
}

TEST_CASE("failed diagnostic output does not lose a completed answer or its accounting") {
    MockServer server;
    Sandbox sandbox(server.port());
    sandbox.store_api_key();
    const auto result = sandbox.run_with(Launch{.args = ask("question"), .failed_stderr = true});
    CHECK(result.code == exit_ok);
    CHECK(result.out == "Hello world\n");
    CHECK(sandbox.only_answer() == "Hello world");
    CHECK(result_outcome(sandbox) == "complete");
    CHECK(server.post_count() == 1);
}

TEST_CASE("interrupted accounting has a bounded lock wait and preserves exit 8") {
    MockServer server;
    server.set_scenario(Scenario::gated);
    Sandbox sandbox(server.port());
    sandbox.store_api_key();
    Child child = sandbox.spawn(Launch{.args = ask("question")});
    REQUIRE(child.wait_for_output("Hello"));
    const UniqueFd lock = open_fd(sandbox.cost_log().parent_path() / "costs.lock", O_RDWR);
    REQUIRE(::flock(lock.get(), LOCK_EX | LOCK_NB) == 0);
    const auto started = Clock::now();
    REQUIRE(::kill(child.pid(), SIGTERM) == 0);
    CHECK(child.wait() == exit_interrupted);
    constexpr std::chrono::seconds maximum_cleanup{4};
    CHECK(Clock::now() - started < maximum_cleanup);
    CHECK(child.err().contains("saving the cost record failed"));
    CHECK(sandbox.only_answer() == "Hello");
    CHECK(server.post_count() == 1);
}

TEST_CASE("stdin input over the request budget refuses before a paid request") {
    MockServer server;
    Sandbox sandbox(server.port());
    sandbox.store_api_key();
    sandbox.write_env("OPENROUTER_CLI_MAX_INPUT_BYTES=8\n");
    const auto result =
        sandbox.run_with(Launch{.args = {"-m", std::string(alpha), "--role", "plain", "-"},
                                .stdin_text = "more than eight bytes"});
    CHECK(result.code == exit_refused);
    CHECK(server.post_count() == 0);
}

TEST_CASE("status neutralizes terminal controls in provider metadata") {
    MockServer server;
    Sandbox sandbox(server.port());
    sandbox.store_api_key();
    server.set_route("/key", [](const RecordedRequest&) {
        auto data = mock::key_data();
        data["label"] = "Café\033[31m\nspoof";
        return data_reply(data);
    });
    const auto result = sandbox.run({"status"});
    CHECK(result.code == exit_ok);
    CHECK(result.out.contains("Café"));
    CHECK_FALSE(result.out.contains('\033'));
    CHECK_FALSE(result.out.contains("\nspoof"));
}

TEST_CASE("every stored user and assistant history message is scanned before resending") {
    for (const std::size_t index : {std::size_t{0}, std::size_t{1}}) {
        MockServer server;
        Sandbox sandbox(server.port());
        sandbox.store_api_key();
        REQUIRE(
            sandbox.run({"--session", "secret-history", "-m", std::string(alpha), "first"}).code ==
            exit_ok);
        const auto path = sandbox.state_dir() / "sessions/secret-history.json";
        auto session = json::parse(read_text(path));
        session["history"][index]["content"] = mock::fake_key('7');
        write_text(path, session.dump());
        const auto result = sandbox.run({"--session", "secret-history", "second"});
        CHECK(result.code == exit_refused);
        CHECK(result.err.contains("session history"));
        CHECK_FALSE(result.all().contains(mock::fake_key('7')));
        CHECK(server.post_count() == 1);
    }
}

TEST_CASE("both spend modes report corrupt rows without rejecting valid charges") {
    MockServer server;
    Sandbox sandbox(server.port());
    sandbox.store_api_key();
    REQUIRE(sandbox.run(ask("question")).code == exit_ok);
    const auto saved = read_text(sandbox.cost_log());
    write_text(sandbox.cost_log(), "not-json\n" + saved);
    const auto spend = sandbox.run({"spend"});
    CHECK(spend.code == exit_ok);
    CHECK(spend.out.contains("Corrupt log lines (not valid records, not counted): 1\n"));
    CHECK(spend.out.contains("0.0042"));
    const auto resolve = sandbox.run({"spend", "--resolve"});
    CHECK(resolve.code == exit_ok);
    CHECK(resolve.err.contains("ignored 1 corrupt cost-log records"));
    CHECK(resolve.out.contains("no unresolved calls with a generation id"));
    CHECK(read_text(sandbox.cost_log()) == "not-json\n" + saved);
    CHECK(server.generation_count() == 0);
    CHECK(server.post_count() == 1);
}

TEST_CASE("interrupting the pre-send ledger lock preserves exit 8 and sends no POST") {
    MockServer server;
    Sandbox sandbox(server.port());
    sandbox.store_api_key();
    const UniqueFd lock =
        open_fd(sandbox.cost_log().parent_path() / "costs.lock", O_RDWR | O_CREAT);
    REQUIRE(::flock(lock.get(), LOCK_EX | LOCK_NB) == 0);
    Child child = sandbox.spawn(Launch{.args = ask("question")});
    const auto deadline = Clock::now() + output_wait;
    while (sandbox.answer_files().empty() && Clock::now() < deadline) {
        std::this_thread::sleep_for(poll_interval);
    }
    REQUIRE_FALSE(sandbox.answer_files().empty());
    REQUIRE(::kill(child.pid(), SIGTERM) == 0);
    CHECK(child.wait() == exit_interrupted);
    CHECK(sandbox.answer_files().empty());
    CHECK(server.post_count() == 0);
}

TEST_CASE("known prompt overhead refuses before waiting for stdin beyond the aggregate budget") {
    MockServer server;
    Sandbox sandbox(server.port());
    sandbox.store_api_key();
    sandbox.write_env("OPENROUTER_CLI_MAX_INPUT_BYTES=8\n");
    std::array<int, 2> ends{-1, -1};
    REQUIRE(::pipe2(ends.data(), O_CLOEXEC) == 0);
    const UniqueFd read_end(ends.front());
    const UniqueFd write_end(ends.back());
    Child child = sandbox.spawn(Launch{
        .args = {"-m", std::string(alpha), "--role", "plain", "-s", "larger than the limit", "-"},
        .stdin_descriptor = read_end.get()});
    const auto deadline = Clock::now() + output_wait;
    while (!child.err().contains("OPENROUTER_CLI_MAX_INPUT_BYTES") && Clock::now() < deadline) {
        std::this_thread::sleep_for(poll_interval);
    }
    REQUIRE(child.err().contains("OPENROUTER_CLI_MAX_INPUT_BYTES"));
    CHECK(child.wait() == exit_refused);
    CHECK(server.post_count() == 0);
}

TEST_CASE("an error keeps its exit code when stderr cannot be written") {
    MockServer server;
    Sandbox sandbox(server.port());
    const RunResult result = sandbox.run_with(
        Launch{.args = {"models", "find", "zzzz-no-such-model"}, .failed_stderr = true});
    CHECK(result.code == exit_model);
}

TEST_CASE("auth set and logout finish their work when stderr cannot be written") {
    MockServer server;
    Sandbox sandbox(server.port());
    const RunResult stored = sandbox.run_with(Launch{
        .args = {"auth", "set"}, .stdin_text = inference_key() + "\n", .failed_stderr = true});
    CHECK(stored.code == exit_ok);
    CHECK(file_exists(sandbox.api_credential()));

    const RunResult removed = sandbox.run_with(Launch{.args = {"logout"}, .failed_stderr = true});
    CHECK(removed.code == exit_ok);
    CHECK_FALSE(file_exists(sandbox.api_credential()));
}

// Each printed row is about 56 bytes, so 200 rows pass the stdio buffer of a pipe (4096 or 8192
// bytes) long before the last lookup; a throwing write there would abandon the rest.
TEST_CASE("spend --resolve prices every call when stdout is closed") {
    constexpr int calls = 200;
    MockServer server;
    server.set_route("/generation", [](const RecordedRequest&) {
        return Reply{.status = http_ok,
                     .body = R"({"data":{"total_cost":0.0123,"provider_name":"MockProvider",)"
                             R"("model":"test/alpha-1"}})"};
    });
    Sandbox sandbox(server.port());
    sandbox.store_api_key();
    for (int index = 0; index < calls; ++index) {
        log_unpriced_call(sandbox, std::format("{:032x}", index), std::format("gen-{}", index));
    }

    const RunResult result =
        sandbox.run_with(Launch{.args = {"spend", "--resolve"}, .closed_stdout = true});

    CHECK(result.code == exit_ok);
    CHECK(sandbox.records_of("resolved").size() == static_cast<std::size_t>(calls));
    CHECK(server.generation_count() == calls);
    CHECK(server.post_count() == 0);
}
