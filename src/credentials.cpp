// POSIX APIs use C linkage. Including wait first gives its macros their public-header origin.
extern "C" {
#include <signal.h>
#include <sys/wait.h>
}

#include "credentials.hpp"

#include "common.hpp"
#include "fsutil.hpp"
#include "http.hpp"

#include <openssl/crypto.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/poll.h>
#include <unistd.h>

namespace orcli {

namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::string_view creds_program = "systemd-creds";
constexpr std::string_view credentials_dir_name = "credentials";
constexpr std::string_view locks_dir_name = "locks";
constexpr std::string_view lock_file_name = "credentials.lock";
constexpr std::size_t bytes_per_kib = 1024;
constexpr std::size_t max_output_kib = 64;
constexpr std::size_t max_output_bytes = max_output_kib * bytes_per_kib;
constexpr std::size_t read_chunk_bytes = 4096;
// _POSIX_PIPE_BUF: a pipe write no larger than this never blocks once poll reports POLLOUT.
constexpr std::size_t pipe_write_bytes = 512;
// Bounds how long a signal that lands just before poll() can go unnoticed.
constexpr std::chrono::milliseconds poll_slice{100};
constexpr std::chrono::milliseconds reap_slice{10};
constexpr std::size_t output_slot = 0;
constexpr std::size_t input_slot = 1;
constexpr unsigned int poll_closed_bits = POLLERR | POLLHUP;
constexpr unsigned int poll_readable_bits = POLLIN | POLLHUP | POLLERR;

std::unexpected<Error> sys_fail_code(std::string_view action, int error_number) {
    return fail(Exit::config,
                std::format("cannot {}: {}", action,
                            std::error_code(error_number, std::system_category()).message()));
}

// Reads errno first, before any other call can overwrite it.
std::unexpected<Error> sys_fail(std::string_view action) {
    return sys_fail_code(action, errno);
}

template <class Call> auto retry_eintr(Call call) {
    for (;;) {
        const auto value = call();
        if (value != -1 || errno != EINTR) {
            return value;
        }
    }
}

std::string_view key_noun(KeyKind kind) {
    return kind == KeyKind::management ? "Management key" : "API key";
}

std::string_view key_with_article(KeyKind kind) {
    return kind == KeyKind::management ? "a Management key" : "an API key";
}

std::string_view store_hint(KeyKind kind) {
    return kind == KeyKind::management
               ? "run `openrouter-cli auth set --management`"
               : "run `openrouter-cli login` (or `openrouter-cli auth set`)";
}

class Descriptor {
public:
    explicit Descriptor(int value) noexcept : value_(value) {}
    Descriptor(Descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Descriptor& operator=(Descriptor&&) = delete;
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    ~Descriptor() { reset(); }

    [[nodiscard]] int get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept { return value_ >= 0; }

    // close(2) is not retried on EINTR: on Linux the descriptor is released either way.
    void reset() noexcept {
        if (value_ >= 0) {
            ::close(std::exchange(value_, -1));
        }
    }

private:
    int value_;
};

struct Pipe {
    Descriptor read_end;
    Descriptor write_end;
};

// O_CLOEXEC keeps every pipe end out of the child except the two placed on its stdin and stdout.
Result<Pipe> make_pipe() {
    std::array<int, 2> ends{};
    if (::pipe2(ends.data(), O_CLOEXEC) != 0) {
        return sys_fail("create a pipe for systemd-creds");
    }
    return Pipe{.read_end = Descriptor(ends.at(0)), .write_end = Descriptor(ends.at(1))};
}

// Descriptors the child receives as its stdin and stdout.
struct ChildStdio {
    int input;
    int output;
};

bool add_redirections(posix_spawn_file_actions_t& actions, const ChildStdio& stdio) {
    return ::posix_spawn_file_actions_adddup2(&actions, stdio.input, STDIN_FILENO) == 0 &&
           ::posix_spawn_file_actions_adddup2(&actions, stdio.output, STDOUT_FILENO) == 0 &&
           ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) ==
               0;
}

// The child's stderr goes to /dev/null: systemd-creds messages are never shown or logged.
class SpawnActions {
public:
    explicit SpawnActions(const ChildStdio& stdio)
        : initialized_(::posix_spawn_file_actions_init(&actions_) == 0),
          ok_(initialized_ && add_redirections(actions_, stdio)) {}
    SpawnActions(SpawnActions&&) = delete;
    SpawnActions& operator=(SpawnActions&&) = delete;
    SpawnActions(const SpawnActions&) = delete;
    SpawnActions& operator=(const SpawnActions&) = delete;
    ~SpawnActions() {
        if (initialized_) {
            ::posix_spawn_file_actions_destroy(&actions_);
        }
    }

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] const posix_spawn_file_actions_t* get() const noexcept { return &actions_; }

private:
    posix_spawn_file_actions_t actions_{};
    bool initialized_;
    bool ok_;
};

struct Run {
    std::string_view action; // "encrypt" or "decrypt", for messages
    std::chrono::seconds timeout;
    Clock::time_point deadline;
};

Result<void> still_running(const Run& run) {
    if (interrupt_requested()) {
        return fail(Exit::interrupted,
                    std::format("interrupted while systemd-creds {} was running", run.action));
    }
    if (Clock::now() >= run.deadline) {
        return fail(Exit::config, std::format("systemd-creds {} did not finish within {} s",
                                              run.action, run.timeout.count()));
    }
    return {};
}

// A child that was not waited for is killed and reaped, so no early return leaves a process
// behind.
class Child {
public:
    explicit Child(pid_t pid) noexcept : pid_(pid) {}
    Child(Child&&) = delete;
    Child& operator=(Child&&) = delete;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    ~Child() {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            int status = 0;
            retry_eintr([&] { return ::waitpid(pid_, &status, 0); });
        }
    }

    // The raw wait status once the child has exited.
    Result<int> wait(const Run& run) {
        for (;;) {
            int status = 0;
            const pid_t reaped = retry_eintr([&] { return ::waitpid(pid_, &status, WNOHANG); });
            if (reaped == pid_) {
                pid_ = -1;
                return status;
            }
            if (reaped < 0) {
                auto error = sys_fail("wait for systemd-creds");
                pid_ = -1;
                return error;
            }
            const auto running = still_running(run);
            if (!running) {
                return std::unexpected(running.error());
            }
            std::this_thread::sleep_for(reap_slice);
        }
    }

private:
    pid_t pid_;
};

// Holds child output, which can be a decrypted key. Capacity is reserved up front so reads never
// reallocate and leave an uncleared copy in freed memory; the bytes are cleared on every path.
class OutputBuffer {
public:
    OutputBuffer() { text_.reserve(max_output_bytes + read_chunk_bytes); }
    OutputBuffer(OutputBuffer&&) = delete;
    OutputBuffer& operator=(OutputBuffer&&) = delete;
    OutputBuffer(const OutputBuffer&) = delete;
    OutputBuffer& operator=(const OutputBuffer&) = delete;
    ~OutputBuffer() { cleanse(text_); }

    // One read(2) from the child's stdout; false at end of file.
    Result<bool> read_from(int descriptor, std::string_view action) {
        const std::size_t used = text_.size();
        text_.resize(used + read_chunk_bytes);
        const std::span<char> tail = std::span<char>(text_).subspan(used);
        const ssize_t got = ::read(descriptor, tail.data(), tail.size());
        const int error_number = errno;
        text_.resize(got > 0 ? used + static_cast<std::size_t>(got) : used);
        if (got < 0) {
            if (error_number == EINTR) {
                return true;
            }
            return sys_fail_code("read from systemd-creds", error_number);
        }
        if (text_.size() > max_output_bytes) {
            return fail(Exit::config,
                        std::format("systemd-creds {} produced more than {} KiB of output", action,
                                    max_output_kib));
        }
        return got > 0;
    }

    SecretString take() { return SecretString(std::move(text_)); }

private:
    std::string text_;
};

// Writes the next piece of input once the child's stdin can take it. A child that closed its
// stdin early is not an error here: its exit status reports what went wrong.
Result<void> feed(Descriptor& to_child, short revents, std::string_view& pending) {
    if (!to_child.valid() || revents == 0) {
        return {};
    }
    const auto events = static_cast<unsigned int>(std::bit_cast<unsigned short>(revents));
    if ((events & poll_closed_bits) != 0) {
        to_child.reset();
        return {};
    }
    const ssize_t written =
        ::write(to_child.get(), pending.data(), std::min(pending.size(), pipe_write_bytes));
    if (written < 0) {
        if (errno == EINTR) {
            return {};
        }
        if (errno == EPIPE) {
            to_child.reset();
            return {};
        }
        return sys_fail("write to systemd-creds");
    }
    pending.remove_prefix(static_cast<std::size_t>(written));
    if (pending.empty()) {
        to_child.reset();
    }
    return {};
}

// Feeds input to the child and collects its stdout until end of file, within the run's deadline.
Result<void> communicate(const Run& run, Descriptor& to_child, const Descriptor& from_child,
                         std::string_view input, OutputBuffer& output) {
    std::string_view pending = input;
    if (pending.empty()) {
        to_child.reset();
    }
    for (;;) {
        const auto running = still_running(run);
        if (!running) {
            return running;
        }
        const auto remaining =
            std::chrono::ceil<std::chrono::milliseconds>(run.deadline - Clock::now());
        const auto slice = std::clamp(remaining, std::chrono::milliseconds{0}, poll_slice);
        std::array<pollfd, 2> slots{};
        slots.at(output_slot) = pollfd{.fd = from_child.get(), .events = POLLIN, .revents = 0};
        // poll(2) skips a negative descriptor, which is how a finished stdin drops out.
        slots.at(input_slot) = pollfd{.fd = to_child.get(), .events = POLLOUT, .revents = 0};
        if (::poll(slots.data(), slots.size(), static_cast<int>(slice.count())) < 0) {
            if (errno == EINTR) {
                continue;
            }
            return sys_fail("wait for systemd-creds");
        }
        const auto fed = feed(to_child, slots.at(input_slot).revents, pending);
        if (!fed) {
            return fed;
        }
        const auto output_events =
            static_cast<unsigned int>(std::bit_cast<unsigned short>(slots.at(output_slot).revents));
        if ((output_events & poll_readable_bits) != 0) {
            const auto more = output.read_from(from_child.get(), run.action);
            if (!more) {
                return std::unexpected(more.error());
            }
            if (!*more) {
                return {};
            }
        }
    }
}

// Runs the credential tool without a shell: input on its stdin, output from its stdout.
Result<SecretString> run_tool(const CredsTool& tool, std::string_view action,
                              std::span<const std::string_view> args, std::string_view input) {
    // The stdin pipe is created and redirected first: if this process runs with a standard
    // stream closed, its ends take the lowest descriptors, so the stdout redirection cannot
    // overwrite them in the child.
    auto to_child = make_pipe();
    if (!to_child) {
        return std::unexpected(to_child.error());
    }
    auto from_child = make_pipe();
    if (!from_child) {
        return std::unexpected(from_child.error());
    }
    const SpawnActions actions(
        ChildStdio{.input = to_child->read_end.get(), .output = from_child->write_end.get()});
    if (!actions.ok()) {
        return fail(Exit::config, "cannot prepare to start systemd-creds");
    }

    std::vector<std::string> arguments{tool.program.string()};
    for (const std::string_view arg : args) {
        arguments.emplace_back(arg);
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int spawned =
        ::posix_spawn(&pid, tool.program.c_str(), actions.get(), nullptr, argv.data(), environ);
    if (spawned != 0) {
        return fail(Exit::config,
                    std::format("cannot start {}: {}", tool.program.string(),
                                std::error_code(spawned, std::system_category()).message()));
    }
    Child child(pid);
    // The parent's copies of the child's ends must close, or end of file never arrives.
    to_child->read_end.reset();
    from_child->write_end.reset();

    const Run run{
        .action = action,
        .timeout = tool.timeout,
        .deadline = Clock::now() + tool.timeout,
    };
    OutputBuffer output;
    const auto communicated =
        communicate(run, to_child->write_end, from_child->read_end, input, output);
    if (!communicated) {
        return std::unexpected(communicated.error());
    }
    to_child->write_end.reset();
    const auto status = child.wait(run);
    if (!status) {
        return std::unexpected(status.error());
    }
    const int code = *status;
    if (!WIFEXITED(code)) {
        return fail(Exit::config, std::format("systemd-creds {} was stopped by signal {}", action,
                                              WTERMSIG(code)));
    }
    if (WEXITSTATUS(code) != 0) {
        return fail(Exit::config,
                    std::format("systemd-creds {} failed (exit {})", action, WEXITSTATUS(code)));
    }
    return output.take();
}

Result<std::string> read_ciphertext(const fs::path& file) {
    return read_regular_file(file, max_output_bytes);
}

} // namespace

// Swapping hands the buffer over without copying the bytes, and leaves the source holding this
// object's previous, empty or already cleared, buffer.
SecretString::SecretString(std::string value) {
    value_.swap(value);
    cleanse(value);
}

SecretString::SecretString(SecretString&& other) noexcept {
    value_.swap(other.value_);
}

SecretString& SecretString::operator=(SecretString&& other) noexcept {
    if (this != &other) {
        cleanse(value_);
        value_.swap(other.value_);
    }
    return *this;
}

SecretString::~SecretString() {
    cleanse(value_);
}

SecretString SecretString::clone() const {
    return SecretString(value_);
}

// Growing to the full capacity first also clears bytes past size() that an earlier, longer value
// or a moved-out small-string buffer left behind; resize within capacity never reallocates.
void cleanse(std::string& text) {
    text.resize(text.capacity());
    OPENSSL_cleanse(text.data(), text.size());
    text.clear();
}

Result<CredsTool> find_creds_tool() {
    const char* const search_path = std::getenv("PATH");
    if (search_path != nullptr) {
        for (const auto piece : std::string_view{search_path} | std::views::split(':')) {
            // Relative entries (an empty one means the current directory) would let whatever
            // directory the command runs from supply the program.
            const fs::path dir{std::string_view{piece.begin(), piece.end()}};
            if (!dir.is_absolute()) {
                continue;
            }
            const fs::path candidate = dir / creds_program;
            std::error_code error;
            if (fs::is_regular_file(candidate, error) && ::access(candidate.c_str(), X_OK) == 0) {
                return CredsTool{.program = candidate};
            }
        }
    }
    return fail(Exit::config,
                "systemd-creds was not found on PATH; openrouter-cli stores keys only encrypted, "
                "with systemd-creds --user (systemd 256 or newer)");
}

std::string_view credential_name(KeyKind kind) {
    return kind == KeyKind::management ? "openrouter-cli-management-key" : "openrouter-cli-api-key";
}

fs::path credential_file(const fs::path& state_dir, KeyKind kind) {
    return state_dir / credentials_dir_name /
           (kind == KeyKind::management ? "management-key.cred" : "api-key.cred");
}

Result<std::string> encrypt_key(const CredsTool& tool, KeyKind kind, const SecretString& key) {
    const std::string name_option = std::format("--name={}", credential_name(kind));
    const auto args = std::to_array<std::string_view>(
        {"--user", "--no-ask-password", name_option, "encrypt", "-", "-"});
    const auto ciphertext = run_tool(tool, "encrypt", args, key.view());
    if (!ciphertext) {
        return std::unexpected(ciphertext.error());
    }
    return std::string(ciphertext->view());
}

Result<SecretString> decrypt_key(const CredsTool& tool, KeyKind kind, std::string_view ciphertext) {
    const std::string name_option = std::format("--name={}", credential_name(kind));
    const auto args = std::to_array<std::string_view>(
        {"--user", "--no-ask-password", "--newline=no", name_option, "decrypt", "-", "-"});
    return run_tool(tool, "decrypt", args, ciphertext);
}

Result<bool> key_stored(const fs::path& state_dir, KeyKind kind) {
    const fs::path file = credential_file(state_dir, kind);
    std::error_code error;
    const fs::file_status status = fs::symlink_status(file, error);
    // Checked before the error code, which also reports the ENOENT behind not_found.
    if (status.type() == fs::file_type::not_found) {
        return false;
    }
    if (error) {
        return fail(Exit::config,
                    std::format("cannot inspect {}: {}", file.string(), error.message()));
    }
    if (status.type() != fs::file_type::regular) {
        return fail(Exit::config, std::format("{} is not a regular file", file.string()));
    }
    return true;
}

Result<SecretString> load_key(const CredsTool& tool, const fs::path& state_dir, KeyKind kind) {
    const auto stored = key_stored(state_dir, kind);
    if (!stored) {
        return std::unexpected(stored.error());
    }
    if (!*stored) {
        return fail(Exit::config,
                    std::format("no {} is stored; {}", key_noun(kind), store_hint(kind)));
    }
    const fs::path file = credential_file(state_dir, kind);
    const auto checked = check_private_file(file);
    if (!checked) {
        return std::unexpected(checked.error());
    }
    const auto ciphertext = read_ciphertext(file);
    if (!ciphertext) {
        return std::unexpected(ciphertext.error());
    }
    auto key = decrypt_key(tool, kind, *ciphertext);
    if (!key && key.error().code != Exit::interrupted) {
        return fail(Exit::config,
                    std::format("could not decrypt the stored {} ({}): {}; run `openrouter-cli "
                                "logout` and log in again",
                                key_noun(kind), file.string(), key.error().message));
    }
    return key;
}

Result<StoreOutcome> store_key(const CredsTool& tool, const fs::path& state_dir, KeyKind kind,
                               const SecretString& key, bool replace, const VerifyKey& verify) {
    const fs::path locks_dir = state_dir / locks_dir_name;
    for (const fs::path& dir : {state_dir, state_dir / credentials_dir_name, locks_dir}) {
        const auto made = ensure_private_dir(dir);
        if (!made) {
            return std::unexpected(made.error());
        }
    }
    const auto lock = FileLock::acquire(locks_dir / lock_file_name, true);
    if (!lock) {
        return std::unexpected(lock.error());
    }
    const auto stored = key_stored(state_dir, kind);
    if (!stored) {
        return std::unexpected(stored.error());
    }
    if (*stored && !replace) {
        return fail(Exit::refused, std::format("{} is already stored; pass --replace to change it",
                                               key_with_article(kind)));
    }
    const auto ciphertext = encrypt_key(tool, kind, key);
    if (!ciphertext) {
        return std::unexpected(ciphertext.error());
    }
    const auto round_trip = decrypt_key(tool, kind, *ciphertext);
    if (!round_trip) {
        return std::unexpected(round_trip.error());
    }
    if (round_trip->view() != key.view()) {
        return fail(Exit::config, "encryption round-trip check failed; the key was not stored");
    }
    const auto verified = verify(key);
    if (!verified) {
        return std::unexpected(verified.error());
    }
    const auto replaced = atomic_replace(credential_file(state_dir, kind), *ciphertext);
    if (!replaced) {
        return std::unexpected(replaced.error());
    }
    return StoreOutcome{.durable = replaced->durable};
}

Result<std::vector<KeyKind>> remove_keys(const fs::path& state_dir, bool include_inference) {
    const fs::path credentials_dir = state_dir / credentials_dir_name;
    std::error_code error;
    const fs::file_status status = fs::symlink_status(credentials_dir, error);
    if (status.type() == fs::file_type::not_found) {
        return std::vector<KeyKind>{};
    }
    if (error) {
        return fail(Exit::config, std::format("cannot inspect {}: {}", credentials_dir.string(),
                                              error.message()));
    }
    if (status.type() != fs::file_type::directory) {
        return fail(Exit::config, std::format("{} is not a directory", credentials_dir.string()));
    }
    const fs::path locks_dir = state_dir / locks_dir_name;
    const auto made = ensure_private_dir(locks_dir);
    if (!made) {
        return std::unexpected(made.error());
    }
    const auto lock = FileLock::acquire(locks_dir / lock_file_name, true);
    if (!lock) {
        return std::unexpected(lock.error());
    }
    std::vector<KeyKind> kinds{KeyKind::management};
    if (include_inference) {
        kinds.push_back(KeyKind::inference);
    }
    std::vector<KeyKind> removed;
    for (const KeyKind kind : kinds) {
        const fs::path file = credential_file(state_dir, kind);
        if (fs::remove(file, error)) {
            removed.push_back(kind);
            continue;
        }
        if (error) {
            // The caller reports only this error, so it must say what is already gone.
            const std::string already =
                removed.empty() ? std::string{}
                                : std::format("; the {} was removed", key_noun(removed.front()));
            return fail(Exit::config, std::format("cannot remove {}: {}{}", file.string(),
                                                  error.message(), already));
        }
    }
    return removed;
}

} // namespace orcli
