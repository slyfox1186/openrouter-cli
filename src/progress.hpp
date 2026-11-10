#ifndef ORCLI_PROGRESS_HPP
#define ORCLI_PROGRESS_HPP

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace orcli {

// What a streamed call can truthfully report while the caller waits. Each method takes the
// event's time and returns the line to print, without the program prefix, or nothing. Time is
// passed in so the schedule can be tested without waiting. The methods are called from
// transfer callbacks, where a thrown exception would abort the call or switch progress off, so
// none of them throws: a line that cannot be formatted is dropped.
class StreamProgress {
public:
    using Clock = std::chrono::steady_clock;

    // Heartbeats come every early_heartbeat until early_heartbeat_window has passed, then every
    // late_heartbeat, so a short wait is informative and a 25-minute call stays quiet.
    static constexpr std::chrono::seconds early_heartbeat{10};
    static constexpr std::chrono::seconds early_heartbeat_window{60};
    static constexpr std::chrono::seconds late_heartbeat{30};

    // reasoning_excluded: the request asked the model to reason and to keep that text out of
    // the stream, which is the usual cause of a long silent start.
    StreamProgress(Clock::time_point started, bool reasoning_excluded) noexcept;

    // Bytes of the event stream, keep-alive comments included. Reports the first arrival.
    [[nodiscard]] std::optional<std::string> on_bytes(std::size_t count,
                                                      Clock::time_point now) noexcept;

    // The provider a chunk named. Reported once, and only before any answer text: a later line
    // would land inside the answer on a terminal, and the cost line names the provider anyway.
    [[nodiscard]] std::optional<std::string> on_provider(std::string_view provider) noexcept;

    // Call before writing non-empty answer text. Reports the first one.
    [[nodiscard]] std::optional<std::string> on_answer_text(Clock::time_point now) noexcept;

    // Call as often as convenient. Reports a heartbeat when one is due; none while answer text
    // has arrived since the previous heartbeat, because the text itself is the progress.
    [[nodiscard]] std::optional<std::string> on_tick(Clock::time_point now) noexcept;

private:
    Clock::time_point started_;
    Clock::time_point next_tick_;
    std::optional<Clock::time_point> last_text_;
    std::size_t received_ = 0;
    bool reasoning_excluded_ = false;
    bool provider_reported_ = false;
    bool text_since_tick_ = false;
};

// Blank lines that keep a call's status lines apart from its answer. The answer goes to stdout
// and the status lines to stderr; when both reach the same terminal, pipe or file they would
// otherwise run together. Each method returns what to write to stderr first, so stdout and the
// saved answer stay exactly what the model wrote. With separate destinations every result is
// empty.
class AnswerSpacing {
public:
    explicit AnswerSpacing(bool shared_destination) noexcept;

    // Call before writing a status line. After answer text this ends an unfinished line and
    // leaves one blank line.
    [[nodiscard]] std::string_view before_status() noexcept;

    // Call before writing non-empty answer text. After a status line this leaves one blank line.
    [[nodiscard]] std::string_view before_answer(std::string_view text) noexcept;

    // The caller ended the answer's last line itself, as it does when the answer is complete.
    void line_completed() noexcept;

private:
    enum class Last : unsigned char { nothing, status, answer };

    bool shared_ = false;
    bool mid_line_ = false;
    Last last_ = Last::nothing;
};

} // namespace orcli

#endif // ORCLI_PROGRESS_HPP
