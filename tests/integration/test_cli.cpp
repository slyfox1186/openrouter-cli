#include "mock_server.hpp"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
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
using mock::Scenario;
using nlohmann::json;

constexpr const char* binary_path = ORCLI_BINARY;

constexpr int exit_ok = 0;
constexpr int exit_usage = 2;
constexpr int exit_refused = 3;
constexpr int exit_model = 4;
constexpr int exit_api = 5;
constexpr int exit_incomplete = 6;
constexpr int exit_interrupted = 8;
constexpr int exit_exec_failed = 127;
constexpr int signal_exit_base = 128;

constexpr int http_ok = 200;
constexpr int http_not_found = 404;
constexpr int idle_timeout_seconds = 2;
constexpr int total_timeout_seconds = 30;
constexpr long long alpha_max_tokens = 8000;
constexpr mode_t private_file_mode = S_IRUSR | S_IWUSR;

constexpr std::chrono::milliseconds poll_interval{20};
constexpr std::chrono::seconds output_wait{15};
// The persistent-404 case waits out the CLI's 30 s generation lookup window.
constexpr std::chrono::seconds child_time_limit{90};
constexpr std::chrono::seconds idle_hold{5};
constexpr std::chrono::seconds drip_hold{3};
constexpr std::chrono::seconds interrupt_hold{60};

constexpr std::string_view alpha = "test/alpha-1";

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
            ::kill(pid_, SIGKILL);
            int status = 0;
            ::waitpid(pid_, &status, 0);
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
};

struct Launch {
    std::vector<std::string> args;
    std::optional<std::string> stdin_text;
    bool closed_stdout = false; // stdout is a pipe whose read end is already closed
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

// Runs between fork and execve in a multithreaded parent, so only async-signal-safe calls.
[[noreturn]] void exec_child(int in, int out, int err, const char* workdir, char* const* argv,
                             char* const* envp) noexcept {
    if (::dup2(in, STDIN_FILENO) < 0 || ::dup2(out, STDOUT_FILENO) < 0 ||
        ::dup2(err, STDERR_FILENO) < 0 || ::chdir(workdir) != 0) {
        ::_exit(exit_exec_failed);
    }
    ::close_range(static_cast<unsigned int>(STDERR_FILENO + 1),
                  std::numeric_limits<unsigned int>::max(), 0);
    // The mock server ignores SIGPIPE in this process; the CLI must set that up for itself.
    struct sigaction default_action{};
    default_action.sa_handler = SIG_DFL;
    sigemptyset(&default_action.sa_mask);
    ::sigaction(SIGPIPE, &default_action, nullptr);
    sigset_t no_signals;
    sigemptyset(&no_signals);
    ::pthread_sigmask(SIG_SETMASK, &no_signals, nullptr);
    ::execve(binary_path, argv, envp);
    ::_exit(exit_exec_failed);
}

// A private temporary HOME and project directory wired to one mock server.
class Sandbox {
public:
    explicit Sandbox(int port) : root_(make_root()) {
        for (const fs::path& dir : {home(), project(), work()}) {
            make_private_dir(dir);
        }
        write_text(project() / ".env",
                   std::format("OPENROUTER_API_KEY=test-key\n"
                               "OPENROUTER_CLI_BASE_URL=http://127.0.0.1:{}/api/v1\n"
                               "OPENROUTER_CLI_IDLE_TIMEOUT_SECONDS={}\n"
                               "OPENROUTER_CLI_TOTAL_TIMEOUT_SECONDS={}\n",
                               port, idle_timeout_seconds, total_timeout_seconds));
    }
    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;
    Sandbox(Sandbox&&) = delete;
    Sandbox& operator=(Sandbox&&) = delete;
    ~Sandbox() {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    [[nodiscard]] fs::path home() const { return root_ / "home"; }
    [[nodiscard]] fs::path project() const { return root_ / "project"; }
    [[nodiscard]] fs::path work() const { return root_ / "work"; }
    [[nodiscard]] fs::path state_dir() const { return project() / "state"; }
    [[nodiscard]] fs::path cost_log() const {
        return home() / ".local" / "state" / "openrouter-cli" / "costs.jsonl";
    }

    Child spawn(const Launch& launch) {
        ++runs_;
        const fs::path in_path = root_ / std::format("run-{}.in", runs_);
        const fs::path out_path = root_ / std::format("run-{}.out", runs_);
        const fs::path err_path = root_ / std::format("run-{}.err", runs_);

        if (launch.stdin_text.has_value()) {
            write_text(in_path, *launch.stdin_text);
        }
        const UniqueFd in =
            open_fd(launch.stdin_text.has_value() ? in_path : fs::path("/dev/null"), O_RDONLY);
        UniqueFd out;
        if (launch.closed_stdout) {
            std::array<int, 2> ends{-1, -1};
            REQUIRE(::pipe2(ends.data(), O_CLOEXEC) == 0);
            const UniqueFd read_end(ends[0]);
            out = UniqueFd(ends[1]);
        } else {
            out = open_fd(out_path, O_WRONLY | O_CREAT | O_TRUNC);
        }
        const UniqueFd err = open_fd(err_path, O_WRONLY | O_CREAT | O_TRUNC);

        std::vector<std::string> arg_strings{binary_path};
        std::ranges::copy(launch.args, std::back_inserter(arg_strings));
        std::vector<std::string> env_strings{
            "HOME=" + home().string(),
            "OPENROUTER_CLI_HOME=" + project().string(),
            "PATH=/usr/bin:/bin",
        };
        const std::vector<char*> argv = pointers_to(arg_strings);
        const std::vector<char*> envp = pointers_to(env_strings);
        const std::string workdir = work().string();

        const pid_t pid = ::fork();
        REQUIRE(pid >= 0);
        if (pid == 0) {
            exec_child(in.get(), out.get(), err.get(), workdir.c_str(), argv.data(), envp.data());
        }
        return {pid, out_path, err_path};
    }

    RunResult run_with(const Launch& launch) {
        Child child = spawn(launch);
        const int code = child.wait();
        return {code, child.out(), child.err()};
    }

    RunResult run(std::vector<std::string> args) {
        return run_with(Launch{.args = std::move(args)});
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

} // namespace

TEST_CASE("a streamed answer is printed, saved and logged with its cost") {
    MockServer server;
    Sandbox sandbox(server.port());

    const RunResult result = sandbox.run(ask("Review this plan."));

    CHECK(result.code == exit_ok);
    CHECK(result.out == "Hello world\n");
    const std::string cost_line = line_starting(result.err, "openrouter-cli: cost $0.0042");
    CHECK_FALSE(cost_line.empty());
    CHECK(cost_line.contains("via MockProvider"));
    CHECK(server.post_count() == 1);
    CHECK(server.generation_count() == 0);
    CHECK(server.last_authorization() == "Bearer test-key");
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
    CHECK(body.at("plugins").dump() ==
          R"([{"enabled":false,"id":"web"},{"enabled":false,"id":"context-compression"}])");
    for (const char* absent : {"temperature", "top_p", "session_id", "cache_control"}) {
        INFO("field ", absent);
        CHECK_FALSE(body.contains(absent));
    }
}

TEST_CASE("a plain model name is resolved once and saved as an alias") {
    MockServer server;
    Sandbox sandbox(server.port());

    const RunResult first = sandbox.run({"-m", "alpha 1", "first question"});
    CHECK(first.code == exit_ok);
    CHECK(first.err.contains("saved alias alpha-1 -> test/alpha-1"));
    const std::string registry = read_text(sandbox.state_dir() / "models.json");
    CHECK(registry.contains("alpha-1"));
    CHECK(registry.contains("test/alpha-1"));
    CHECK(text_at(server.last_post(), "model") == alpha);

    const RunResult second = sandbox.run({"-m", "alpha-1", "second question"});
    CHECK(second.code == exit_ok);
    CHECK(text_at(server.last_post(), "model") == alpha);
    CHECK(server.post_count() == 2);
}

TEST_CASE("an ambiguous model name lists the candidates and sends nothing") {
    MockServer server;
    Sandbox sandbox(server.port());

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

    const RunResult result = sandbox.run(ask("question"));

    CHECK(result.code == exit_ok);
    CHECK(result.out == "Hello world\n");
    CHECK_FALSE(line_starting(result.err, "openrouter-cli: cost $0.0123").empty());
    CHECK(server.generation_count() == 3);
    CHECK(server.last_generation_id() == "gen-test-1");
    CHECK(sandbox.records_of("resolved").size() == 1U);
    CHECK(server.post_count() == 1);
}

TEST_CASE("a generation record that never appears leaves the cost unknown") {
    MockServer server;
    server.set_scenario(Scenario::success_no_cost);
    server.set_generation_statuses({}, http_not_found);
    Sandbox sandbox(server.port());

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

    const RunResult result = sandbox.run(ask("question"));

    CHECK(result.code == exit_api);
    CHECK(result.err.contains("402"));
    CHECK(result.err.contains("payment_required"));
    CHECK_FALSE(result.err.contains(mock::fake_api_key()));
    CHECK(result.err.contains("[redacted]"));
    CHECK(result.out.empty());
    CHECK(server.post_count() == 1);
    CHECK(result_outcome(sandbox) == "error");
}

TEST_CASE("an error body served with status 200 is an API error") {
    MockServer server;
    server.set_scenario(Scenario::error_in_200);
    Sandbox sandbox(server.port());

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
    for (fs::path dir = sandbox.cost_log(); dir != sandbox.home(); dir = dir.parent_path()) {
        make_private_dir(dir);
    }

    const RunResult result = sandbox.run(ask("question"));

    CHECK(result.code == exit_refused);
    CHECK(result.err.contains("nothing was sent"));
    CHECK(server.post_count() == 0);
}

TEST_CASE("a session carries completed turns only and pins its model and system text") {
    MockServer server;
    Sandbox sandbox(server.port());

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

    const RunResult anthropic_session =
        sandbox.run({"--session", "s2", "-m", "anthropic/claude-test", "q1"});
    CHECK(anthropic_session.code == exit_ok);
    CHECK(server.last_post().contains("session_id"));
    const RunResult anthropic_plain = sandbox.run({"-m", "anthropic/claude-test", "q1"});
    CHECK(anthropic_plain.code == exit_ok);
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

    const RunResult result = sandbox.run_with(
        Launch{.args = ask("-"), .stdin_text = std::string("question from stdin")});

    CHECK(result.code == exit_ok);
    CHECK(last_user_text(server.last_post()) == "question from stdin");
    CHECK(server.post_count() == 1);
}

TEST_CASE("models, spend and balance answer without asking a model") {
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
    CHECK(shown.out.contains("10"));

    const RunResult empty_list = sandbox.run({"models", "list"});
    CHECK(empty_list.code == exit_ok);
    CHECK(empty_list.out.contains("no saved aliases"));

    const RunResult saved = sandbox.run({"models", "save", "mine", "test/beta-2"});
    CHECK(saved.code == exit_ok);
    const RunResult listed = sandbox.run({"models", "list"});
    CHECK(listed.code == exit_ok);
    CHECK(listed.out.contains("mine -> test/beta-2"));

    const RunResult balance = sandbox.run({"balance"});
    CHECK(balance.code == exit_ok);
    CHECK(balance.out.contains("usage today      $0.2500"));
    CHECK(balance.out.contains("usage total      $1.5000"));
    CHECK(balance.out.contains("key limit        n/a"));

    CHECK(server.post_count() == 0);

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

    const auto started = Clock::now();
    const RunResult result = sandbox.run(ask("question"));
    const auto elapsed = Clock::now() - started;

    CHECK(result.code == exit_api);
    CHECK(result.err.contains("idle timeout"));
    CHECK(elapsed < idle_hold);
    CHECK(sandbox.only_answer() == "Hello");
    CHECK(server.post_count() == 1);
    CHECK(result_outcome(sandbox) == "error");
}

TEST_CASE("keepalive comments keep a quiet call alive past the idle limit") {
    MockServer server;
    server.set_scenario(Scenario::drip);
    server.set_hold(drip_hold);
    Sandbox sandbox(server.port());

    const RunResult result = sandbox.run(ask("question"));

    CHECK(result.code == exit_ok);
    CHECK(result.out == "Hello world\n");
    CHECK(server.post_count() == 1);
}

TEST_CASE("SIGTERM mid-stream keeps the partial answer, and the busy session is refused") {
    MockServer server;
    server.set_scenario(Scenario::slow);
    server.set_hold(interrupt_hold);
    Sandbox sandbox(server.port());

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
    CHECK(server.post_count() == 1);
}

TEST_CASE("command line grammar") {
    MockServer server;
    Sandbox sandbox(server.port());

    CHECK(sandbox.run({"models"}).code == exit_usage);
    CHECK(sandbox.run({"-m", "x", "spend"}).code == exit_usage);
    CHECK(sandbox.run({}).code == exit_usage);
    CHECK(server.post_count() == 0);

    const RunResult literal = sandbox.run({"-m", std::string(alpha), "--", "models"});
    CHECK(literal.code == exit_ok);
    CHECK(last_user_text(server.last_post()) == "models");
    CHECK(server.post_count() == 1);
}

TEST_CASE("a closed stdout still leaves a complete answer file") {
    MockServer server;
    Sandbox sandbox(server.port());

    const RunResult result =
        sandbox.run_with(Launch{.args = ask("question"), .closed_stdout = true});

    CHECK(result.code == exit_ok);
    CHECK(sandbox.only_answer() == "Hello world");
    CHECK(result_outcome(sandbox) == "complete");
    CHECK(server.post_count() == 1);
}
