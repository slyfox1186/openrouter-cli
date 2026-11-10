#include "progress.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

namespace {

using orcli::AnswerSpacing;
using orcli::StreamProgress;
using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr std::size_t keepalive_bytes = 26;

// A fixed origin, so every time in a test is an offset from the start of the call.
StreamProgress::Clock::time_point at(milliseconds offset) {
    return StreamProgress::Clock::time_point{} + offset;
}

StreamProgress started(bool reasoning_excluded) {
    return StreamProgress(at(milliseconds{0}), reasoning_excluded);
}

} // namespace

TEST_CASE("the first stream bytes are reported once, with the reasoning note when it applies") {
    StreamProgress reasoning = started(true);
    CHECK(reasoning.on_bytes(0, at(milliseconds{100})) == std::nullopt);
    CHECK(reasoning.on_bytes(keepalive_bytes, at(milliseconds{800})) ==
          "response stream started after 0.8s; reasoning text is excluded, so the answer can "
          "take a while");
    CHECK(reasoning.on_bytes(keepalive_bytes, at(milliseconds{900})) == std::nullopt);

    StreamProgress plain = started(false);
    CHECK(plain.on_bytes(keepalive_bytes, at(seconds{12})) == "response stream started after 12s");
}

TEST_CASE("the provider is reported once and only before answer text") {
    StreamProgress early = started(false);
    CHECK(early.on_provider("") == std::nullopt);
    CHECK(early.on_provider("MockProvider") == "provider reported: MockProvider");
    CHECK(early.on_provider("MockProvider") == std::nullopt);

    StreamProgress late = started(false);
    CHECK(late.on_answer_text(at(seconds{2})) == "answer started after 2.0s");
    CHECK(late.on_provider("MockProvider") == std::nullopt);

    StreamProgress hostile = started(false);
    CHECK(hostile.on_provider("Mock\x1b[31m\nProvider") == "provider reported: Mock [31m Provider");
}

TEST_CASE("the start of the answer is reported once") {
    StreamProgress progress = started(false);
    CHECK(progress.on_answer_text(at(seconds{41})) == "answer started after 41s");
    CHECK(progress.on_answer_text(at(seconds{42})) == std::nullopt);
}

TEST_CASE("heartbeats come every 10 seconds for a minute and every 30 after") {
    StreamProgress progress = started(false);
    CHECK(progress.on_tick(at(milliseconds{9999})) == std::nullopt);
    CHECK(progress.on_tick(at(seconds{10})) == "10s, waiting for the response stream");
    CHECK(progress.on_tick(at(seconds{10})) == std::nullopt);
    CHECK(progress.on_tick(at(seconds{19})) == std::nullopt);
    for (const int second : {20, 30, 40, 50, 60}) {
        INFO(second);
        CHECK(progress.on_tick(at(seconds{second})).has_value());
    }
    CHECK(progress.on_tick(at(seconds{70})) == std::nullopt);
    CHECK(progress.on_tick(at(seconds{89})) == std::nullopt);
    CHECK(progress.on_tick(at(seconds{90})) == "90s, waiting for the response stream");
    CHECK(progress.on_tick(at(seconds{119})) == std::nullopt);
    CHECK(progress.on_tick(at(seconds{120})).has_value());
}

TEST_CASE("a heartbeat says how far the call has got") {
    StreamProgress progress = started(true);
    CHECK(progress.on_bytes(keepalive_bytes, at(seconds{1})).has_value());
    CHECK(progress.on_bytes(keepalive_bytes, at(seconds{5})) == std::nullopt);
    CHECK(progress.on_tick(at(seconds{10})) ==
          "10s, no answer text yet, 52 bytes of keep-alive and metadata received");

    // Text that keeps arriving is its own progress, so the heartbeat stays out of the answer.
    CHECK(progress.on_answer_text(at(seconds{15})).has_value());
    CHECK(progress.on_tick(at(seconds{20})) == std::nullopt);
    CHECK(progress.on_answer_text(at(seconds{29})) == std::nullopt);
    CHECK(progress.on_tick(at(seconds{30})) == std::nullopt);

    CHECK(progress.on_tick(at(seconds{40})) == "40s, no new answer text for 11s");
    CHECK(progress.on_tick(at(seconds{50})) == "50s, no new answer text for 21s");
    CHECK(progress.on_answer_text(at(seconds{55})) == std::nullopt);
    CHECK(progress.on_tick(at(seconds{60})) == std::nullopt);
}

TEST_CASE("a shared destination gets one blank line on each side of the answer") {
    AnswerSpacing spacing(true);
    CHECK(spacing.before_status().empty());
    CHECK(spacing.before_status().empty());
    CHECK(spacing.before_answer("Hello") == "\n");
    CHECK(spacing.before_answer(" world").empty());
    spacing.line_completed();
    CHECK(spacing.before_status() == "\n");
    CHECK(spacing.before_status().empty());
}

TEST_CASE("a status line inside the answer ends an unfinished line and is set apart") {
    AnswerSpacing spacing(true);
    CHECK(spacing.before_status().empty());
    CHECK(spacing.before_answer("First half") == "\n");
    CHECK(spacing.before_status() == "\n\n");
    CHECK(spacing.before_answer(" and a full line\n") == "\n");
    CHECK(spacing.before_status() == "\n");
}

TEST_CASE("a call with no answer text gets no blank lines") {
    AnswerSpacing spacing(true);
    CHECK(spacing.before_status().empty());
    spacing.line_completed();
    CHECK(spacing.before_status().empty());
}

TEST_CASE("separate destinations get no blank lines") {
    AnswerSpacing spacing(false);
    CHECK(spacing.before_status().empty());
    CHECK(spacing.before_answer("Hello").empty());
    CHECK(spacing.before_status().empty());
    CHECK(spacing.before_answer(" world\n").empty());
    spacing.line_completed();
    CHECK(spacing.before_status().empty());
}
