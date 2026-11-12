#ifndef ORCLI_GROUP_HPP
#define ORCLI_GROUP_HPP

#include "common.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace orcli {

// "a/b, c/d" -> {"a/b", "c/d"}: items are trimmed of spaces and tabs; an empty item is
// Exit::usage. Commas cannot be escaped, since model ids and provider tags never contain one.
Result<std::vector<std::string>> parse_model_list(std::string_view text);

// 32-bit FNV-1a as eight lowercase hex digits.
std::string fnv1a_hex(std::string_view text);

// The session a group member keeps under the group conversation `name`:
// NAME.<normalized model id, shortened>.<fnv1a of the full id>, at most 64 characters.
// Exit::usage when name leaves no room for the model part.
Result<std::string> member_session_name(std::string_view name, std::string_view model_id);

// How one member's question ended, from the parent's point of view.
enum class MemberState : std::uint8_t {
    complete,      // exit 0
    incomplete,    // exit 6: the partial answer is kept
    failed,        // any other CLI exit code
    interrupted,   // exit 8
    crashed,       // killed by a signal, or exit 127 (the program could not run)
    never_started, // the parent could not start it
};

struct MemberOutcome {
    MemberState state = MemberState::never_started;
    int code = 0; // the child's exit code, the signal number when crashed, or the spawn error's
};

MemberOutcome outcome_of_exit(int exit_code);

// 8 when the parent was cancelled or any member was interrupted; 1 when any member crashed;
// 0 when every member completed; otherwise the code of the first member, in group order,
// that did not complete.
int group_exit_code(const std::vector<MemberOutcome>& outcomes, bool cancelled);

// ----- running children -------------------------------------------------------------------

struct ChildSpec {
    std::vector<std::string> argv; // argv[0] is the program path
};

struct ChildResult {
    bool started = false;
    Error spawn_error{.code = Exit::ok, .message = {}}; // set when started is false
    int exit_code = -1;                                 // when the child exited normally
    int term_signal = 0;                                // when a signal ended it
    std::string out;                                    // everything it wrote to stdout
};

struct ChildEvents {
    // One complete stderr line (without its newline) from child `index`, as it arrives. A final
    // fragment without a newline is delivered at EOF.
    std::function<void(std::size_t index, std::string_view line)> stderr_line;
    // Child `index` has exited and both of its output streams are at EOF.
    std::function<void(std::size_t index, const ChildResult& result)> finished;
};

// Longer than the 2 s a question child may spend recording an interrupted call's cost.
inline constexpr std::chrono::seconds default_stop_grace{10};

struct SuperviseOptions {
    // Sent to every child's stdin, which is then closed.
    std::string_view input;
    // How long children get to finish after SIGINT before SIGKILL.
    std::chrono::milliseconds grace{default_stop_grace};
};

// Starts every child at once with stdin, stdout and stderr on private pipes and multiplexes all
// of them until each child is reaped. If a child cannot be started, no later child is; the ones
// already running are left to finish, since their requests may already be billed. When
// interrupt_requested() becomes true, SIGINT is sent to each live child (never to the process
// group), and any child still running after the grace period is killed. Every started child is
// reaped exactly once on every path.
std::vector<ChildResult> run_children(const std::vector<ChildSpec>& children,
                                      const SuperviseOptions& options, const ChildEvents& events,
                                      bool& cancelled);

} // namespace orcli

#endif // ORCLI_GROUP_HPP
