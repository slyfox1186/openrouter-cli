#include "progress.hpp"

#include "display.hpp"

#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace orcli {

namespace {

constexpr long long millis_per_second = 1000;
constexpr long long millis_per_tenth = 100;
// Below this a tenth of a second is worth showing; above it whole seconds read better.
constexpr long long tenths_shown_below_millis = 10 * millis_per_second;

long long whole_seconds(StreamProgress::Clock::duration elapsed) {
    return std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
}

std::string seconds_text(StreamProgress::Clock::duration elapsed) {
    const long long millis = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    if (millis < tenths_shown_below_millis) {
        return std::format("{}.{}s", millis / millis_per_second,
                           (millis % millis_per_second) / millis_per_tenth);
    }
    return std::format("{}s", millis / millis_per_second);
}

} // namespace

StreamProgress::StreamProgress(Clock::time_point started, bool reasoning_excluded) noexcept
    : started_(started), next_tick_(started + early_heartbeat),
      reasoning_excluded_(reasoning_excluded) {}

std::optional<std::string> StreamProgress::on_bytes(std::size_t count,
                                                    Clock::time_point now) noexcept {
    const bool first = received_ == 0 && count > 0;
    received_ += count;
    if (!first) {
        return std::nullopt;
    }
    try {
        return std::format("response stream started after {}{}", seconds_text(now - started_),
                           reasoning_excluded_
                               ? "; reasoning text is excluded, so the answer can take a while"
                               : "");
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> StreamProgress::on_provider(std::string_view provider) noexcept {
    if (provider_reported_ || last_text_.has_value() || provider.empty()) {
        return std::nullopt;
    }
    provider_reported_ = true;
    try {
        return std::format("provider reported: {}", display_metadata(provider));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> StreamProgress::on_answer_text(Clock::time_point now) noexcept {
    const bool first = !last_text_.has_value();
    last_text_ = now;
    text_since_tick_ = true;
    if (!first) {
        return std::nullopt;
    }
    try {
        return std::format("answer started after {}", seconds_text(now - started_));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> StreamProgress::on_tick(Clock::time_point now) noexcept {
    if (now < next_tick_) {
        return std::nullopt;
    }
    const Clock::duration elapsed = now - started_;
    next_tick_ = now + (elapsed < early_heartbeat_window ? early_heartbeat : late_heartbeat);
    const bool text_arrived = text_since_tick_;
    text_since_tick_ = false;
    try {
        if (received_ == 0) {
            return std::format("{}s, waiting for the response stream", whole_seconds(elapsed));
        }
        if (!last_text_.has_value()) {
            return std::format("{}s, no answer text yet, {} bytes of keep-alive and metadata "
                               "received",
                               whole_seconds(elapsed), received_);
        }
        if (text_arrived) {
            return std::nullopt;
        }
        return std::format("{}s, no new answer text for {}s", whole_seconds(elapsed),
                           whole_seconds(now - *last_text_));
    } catch (...) {
        return std::nullopt;
    }
}

AnswerSpacing::AnswerSpacing(bool shared_destination) noexcept : shared_(shared_destination) {}

std::string_view AnswerSpacing::before_status() noexcept {
    const bool after_answer = last_ == Last::answer;
    last_ = Last::status;
    if (!shared_ || !after_answer) {
        return {};
    }
    return mid_line_ ? "\n\n" : "\n";
}

std::string_view AnswerSpacing::before_answer(std::string_view text) noexcept {
    const bool after_status = last_ == Last::status;
    last_ = Last::answer;
    mid_line_ = !text.ends_with('\n');
    return shared_ && after_status ? "\n" : std::string_view{};
}

void AnswerSpacing::line_completed() noexcept {
    mid_line_ = false;
}

} // namespace orcli
