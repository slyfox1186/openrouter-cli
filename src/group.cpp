// POSIX APIs use C linkage. Including wait first gives its macros their public-header origin.
extern "C" {
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
}

#include "group.hpp"

#include "common.hpp"
#include "http.hpp"
#include "models.hpp"
#include "posix.hpp"
#include "session.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/poll.h>
#include <unistd.h>

namespace orcli {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t max_session_name = 64;
constexpr std::size_t digest_length = 8;
constexpr std::uint32_t fnv_offset = 2166136261U;
constexpr std::uint32_t fnv_prime = 16777619U;
constexpr int exit_ok = 0;
constexpr int exit_incomplete = 6;
constexpr int exit_interrupted = 8;
constexpr int exit_not_runnable = 127;
constexpr int exit_internal = 1;
constexpr std::size_t io_chunk = 65536;
constexpr int poll_interval_ms = 100;

bool is_blank(char raw) {
    return raw == ' ' || raw == '\t';
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && is_blank(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_blank(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// One child as the supervisor tracks it.
struct Running {
    pid_t pid = -1;
    bool reaped = false;
    bool reported = false;
    posix::Descriptor input;
    posix::Descriptor output;
    posix::Descriptor errors;
    std::size_t written = 0;
    std::string pending_line;
};

void set_nonblocking(const posix::Descriptor& descriptor) {
    const int flags = ::fcntl(descriptor.get(), F_GETFL);
    if (flags >= 0) {
        ::fcntl(descriptor.get(), F_SETFL,
                static_cast<int>(static_cast<unsigned int>(flags) |
                                 static_cast<unsigned int>(O_NONBLOCK)));
    }
}

struct MemberPipes {
    posix::Pipe input;
    posix::Pipe output;
    posix::Pipe errors;
};

Result<MemberPipes> member_pipes() {
    auto input = posix::make_pipe("a group member");
    if (!input) {
        return std::unexpected(input.error());
    }
    auto output = posix::make_pipe("a group member");
    if (!output) {
        return std::unexpected(output.error());
    }
    auto errors = posix::make_pipe("a group member");
    if (!errors) {
        return std::unexpected(errors.error());
    }
    return MemberPipes{
        .input = std::move(*input),
        .output = std::move(*output),
        .errors = std::move(*errors),
    };
}

std::string program_of(const ChildSpec& spec) {
    return spec.argv.empty() ? std::string("a group member") : spec.argv.front();
}

// The child starts with SIGINT, SIGTERM and SIGHUP blocked; openrouter-cli unblocks them once
// its handlers are installed, so an interrupt that arrives in between takes the normal path.
Result<pid_t> spawn_child(const ChildSpec& spec, Running& child) {
    auto pipes = member_pipes();
    if (!pipes) {
        return std::unexpected(pipes.error());
    }
    posix_spawn_file_actions_t actions{};
    if (::posix_spawn_file_actions_init(&actions) != 0) {
        return posix::sys_fail("prepare a group member");
    }
    posix_spawnattr_t attributes{};
    if (::posix_spawnattr_init(&attributes) != 0) {
        ::posix_spawn_file_actions_destroy(&actions);
        return posix::sys_fail("prepare a group member");
    }
    const auto blocked = posix::stop_signals();
    const bool configured =
        ::posix_spawn_file_actions_adddup2(&actions, pipes->input.read_end.get(), STDIN_FILENO) ==
            0 &&
        ::posix_spawn_file_actions_adddup2(&actions, pipes->output.write_end.get(),
                                           STDOUT_FILENO) == 0 &&
        ::posix_spawn_file_actions_adddup2(&actions, pipes->errors.write_end.get(),
                                           STDERR_FILENO) == 0 &&
        ::posix_spawnattr_setsigmask(&attributes, &blocked) == 0 &&
        ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK) == 0;

    std::vector<std::string> arguments = spec.argv;
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int spawned =
        configured && !arguments.empty()
            ? ::posix_spawn(&pid, argv.front(), &actions, &attributes, argv.data(), environ)
            : EINVAL;
    ::posix_spawn_file_actions_destroy(&actions);
    ::posix_spawnattr_destroy(&attributes);
    if (spawned != 0) {
        return posix::sys_fail_code(std::format("start {}", program_of(spec)), spawned);
    }
    child.input = std::move(pipes->input.write_end);
    child.output = std::move(pipes->output.read_end);
    child.errors = std::move(pipes->errors.read_end);
    for (const posix::Descriptor* end : {&child.input, &child.output, &child.errors}) {
        set_nonblocking(*end);
    }
    return pid;
}

void deliver_lines(std::size_t index, Running& child, const ChildEvents& events, bool at_end) {
    std::size_t start = 0;
    for (std::size_t newline = child.pending_line.find('\n'); newline != std::string::npos;
         newline = child.pending_line.find('\n', start)) {
        if (events.stderr_line) {
            events.stderr_line(index,
                               std::string_view(child.pending_line).substr(start, newline - start));
        }
        start = newline + 1;
    }
    child.pending_line.erase(0, start);
    if (at_end && !child.pending_line.empty()) {
        if (events.stderr_line) {
            events.stderr_line(index, child.pending_line);
        }
        child.pending_line.clear();
    }
}

// Reads what is available; closes the descriptor at EOF or on a read error.
void drain(posix::Descriptor& descriptor, std::string& sink) {
    std::array<char, io_chunk> buffer{};
    for (;;) {
        const ssize_t got = posix::retry_eintr(
            [&] { return ::read(descriptor.get(), buffer.data(), buffer.size()); });
        if (got > 0) {
            sink.append(buffer.data(), static_cast<std::size_t>(got));
            continue;
        }
        if (got < 0 && errno == EAGAIN) {
            return;
        }
        descriptor.reset();
        return;
    }
}

void feed(Running& child, std::string_view input) {
    while (child.written < input.size()) {
        const std::string_view rest = input.substr(child.written);
        const ssize_t sent = posix::retry_eintr([&] {
            return ::write(child.input.get(), rest.data(), std::min(io_chunk, rest.size()));
        });
        if (sent > 0) {
            child.written += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EAGAIN) {
            return;
        }
        // EPIPE or another error: the child stopped reading, which its own exit will explain.
        break;
    }
    child.input.reset();
}

enum class Stream : std::uint8_t { input, output, errors };

// Runs the children of one group question; see run_children.
class Supervisor {
public:
    Supervisor(std::vector<ChildSpec> children, const SuperviseOptions& options, ChildEvents events)
        : specs_(std::move(children)), options_(options), events_(std::move(events)),
          results_(specs_.size()), running_(specs_.size()) {}

    std::vector<ChildResult> run(bool& cancelled) {
        start_all();
        for (;;) {
            try {
                reap();
                if (report_finished()) {
                    cancelled = cancelled_;
                    return std::move(results_);
                }
                check_stop();
                poll_once();
            } catch (const std::exception&) {
                // Output can no longer be relayed safely; only stopping and reaping remain.
                stop_all(true);
            }
        }
    }

private:
    void start_all() {
        for (std::size_t index = 0; index < specs_.size(); ++index) {
            auto pid = spawn_child(specs_.at(index), running_.at(index));
            if (!pid) {
                // Later members are not started; earlier ones may already be billed, so they
                // finish.
                results_.at(index).spawn_error = pid.error();
                return;
            }
            running_.at(index).pid = *pid;
            results_.at(index).started = true;
            if (options_.input.empty()) {
                running_.at(index).input.reset();
            }
        }
    }

    void signal_live(int signal_number) const {
        for (const Running& child : running_) {
            if (child.pid > 0 && !child.reaped) {
                ::kill(child.pid, signal_number);
            }
        }
    }

    void stop_all(bool abandon_io) {
        signal_live(SIGINT);
        if (!kill_at_.has_value()) {
            kill_at_ = Clock::now() + options_.grace;
        }
        if (abandon_io) {
            for (Running& child : running_) {
                child.input.reset();
                child.output.reset();
                child.errors.reset();
            }
        }
    }

    void reap() {
        for (std::size_t index = 0; index < running_.size(); ++index) {
            Running& child = running_.at(index);
            if (child.pid <= 0 || child.reaped) {
                continue;
            }
            int status = 0;
            const pid_t done =
                posix::retry_eintr([&] { return ::waitpid(child.pid, &status, WNOHANG); });
            if (done != child.pid) {
                continue;
            }
            child.reaped = true;
            if (WIFEXITED(status)) {
                results_.at(index).exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                results_.at(index).term_signal = WTERMSIG(status);
            }
        }
    }

    // Reports each child that has exited with both outputs at EOF; true once all have been.
    bool report_finished() {
        bool all_reported = true;
        for (std::size_t index = 0; index < running_.size(); ++index) {
            Running& child = running_.at(index);
            if (!results_.at(index).started || child.reported) {
                continue;
            }
            if (!child.reaped || child.output.valid() || child.errors.valid()) {
                all_reported = false;
                continue;
            }
            deliver_lines(index, child, events_, true);
            child.reported = true;
            if (events_.finished) {
                events_.finished(index, results_.at(index));
            }
        }
        return all_reported;
    }

    // A parent interrupt is forwarded to each child itself, so one sent to this process alone
    // still reaches them; children still running after the grace period are killed.
    void check_stop() {
        if (interrupt_requested() && !cancelled_) {
            cancelled_ = true;
            stop_all(false);
        }
        if (kill_at_.has_value() && Clock::now() >= *kill_at_) {
            signal_live(SIGKILL);
        }
    }

    void poll_once() {
        std::vector<pollfd> watched;
        std::vector<std::pair<std::size_t, Stream>> owners;
        const auto watch = [&](std::size_t index, const posix::Descriptor& end, short wanted,
                               Stream stream) {
            if (end.valid()) {
                watched.push_back({.fd = end.get(), .events = wanted, .revents = 0});
                owners.emplace_back(index, stream);
            }
        };
        for (std::size_t index = 0; index < running_.size(); ++index) {
            const Running& child = running_.at(index);
            watch(index, child.input, POLLOUT, Stream::input);
            watch(index, child.output, POLLIN, Stream::output);
            watch(index, child.errors, POLLIN, Stream::errors);
        }
        const int ready = ::poll(watched.data(), watched.size(), poll_interval_ms);
        if (ready < 0 && errno != EINTR) {
            stop_all(true);
            return;
        }
        for (std::size_t slot = 0; ready > 0 && slot < watched.size(); ++slot) {
            if (watched.at(slot).revents != 0) {
                service(owners.at(slot).first, owners.at(slot).second);
            }
        }
    }

    void service(std::size_t index, Stream stream) {
        Running& child = running_.at(index);
        switch (stream) {
        case Stream::input:
            feed(child, options_.input);
            return;
        case Stream::output:
            drain(child.output, results_.at(index).out);
            return;
        case Stream::errors:
            drain(child.errors, child.pending_line);
            deliver_lines(index, child, events_, false);
            return;
        }
    }

    std::vector<ChildSpec> specs_;
    SuperviseOptions options_;
    ChildEvents events_;
    std::vector<ChildResult> results_;
    std::vector<Running> running_;
    std::optional<Clock::time_point> kill_at_;
    bool cancelled_ = false;
};

} // namespace

Result<std::vector<std::string>> parse_model_list(std::string_view text) {
    std::vector<std::string> items;
    for (std::size_t start = 0;;) {
        const std::size_t comma = text.find(',', start);
        const std::string_view item = trim(text.substr(
            start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
        if (item.empty()) {
            return fail(Exit::usage, "the model list has an empty item; separate models with "
                                     "single commas, e.g. \"AUTHOR/A,AUTHOR/B\"");
        }
        items.emplace_back(item);
        if (comma == std::string_view::npos) {
            return items;
        }
        start = comma + 1;
    }
}

std::string fnv1a_hex(std::string_view text) {
    std::uint32_t hash = fnv_offset;
    for (const char raw : text) {
        hash ^= static_cast<unsigned char>(raw);
        hash *= fnv_prime;
    }
    return std::format("{:08x}", hash);
}

Result<std::string> member_session_name(std::string_view name, std::string_view model_id) {
    if (!valid_session_name(name)) {
        return fail(Exit::usage, "invalid session name; use 1 to 64 letters, digits, '.', '_' or "
                                 "'-'");
    }
    // NAME + "." + at least one character + "." + digest.
    const std::size_t fixed = name.size() + 2 + digest_length;
    if (fixed >= max_session_name) {
        return fail(Exit::usage,
                    std::format("a group session name can have at most {} characters, so each "
                                "member's session fits the 64-character limit",
                                max_session_name - digest_length - 3));
    }
    std::string prefix = normalize_alias(model_id);
    prefix.resize(std::min(prefix.size(), max_session_name - fixed));
    while (!prefix.empty() && (prefix.back() == '-' || prefix.back() == '.')) {
        prefix.pop_back();
    }
    if (prefix.empty()) {
        prefix = "m";
    }
    return std::format("{}.{}.{}", name, prefix, fnv1a_hex(model_id));
}

MemberOutcome outcome_of_exit(int exit_code) {
    switch (exit_code) {
    case exit_ok:
        return {.state = MemberState::complete, .code = exit_code};
    case exit_incomplete:
        return {.state = MemberState::incomplete, .code = exit_code};
    case exit_interrupted:
        return {.state = MemberState::interrupted, .code = exit_code};
    case exit_not_runnable:
        return {.state = MemberState::crashed, .code = exit_code};
    default:
        return {.state = MemberState::failed, .code = exit_code};
    }
}

int group_exit_code(const std::vector<MemberOutcome>& outcomes, bool cancelled) {
    const auto any = [&outcomes](MemberState state) {
        return std::ranges::any_of(
            outcomes, [state](const MemberOutcome& one) { return one.state == state; });
    };
    if (cancelled || any(MemberState::interrupted)) {
        return exit_interrupted;
    }
    if (any(MemberState::crashed)) {
        return exit_internal;
    }
    for (const MemberOutcome& outcome : outcomes) {
        if (outcome.state != MemberState::complete) {
            return outcome.code;
        }
    }
    return exit_ok;
}

std::vector<ChildResult> run_children(const std::vector<ChildSpec>& children,
                                      const SuperviseOptions& options, const ChildEvents& events,
                                      bool& cancelled) {
    cancelled = false;
    return Supervisor(children, options, events).run(cancelled);
}

} // namespace orcli
