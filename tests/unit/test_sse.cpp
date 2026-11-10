#include "sse.hpp"

#include "common.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using orcli::ChatStream;
using orcli::Exit;
using orcli::SseParser;
using orcli::StreamOutcome;

namespace {

constexpr long long prompt_tokens = 1200;
constexpr long long cached_tokens = 800;
constexpr long long cache_write_tokens = 300;
constexpr long long completion_tokens = 450;
constexpr long long reasoning_tokens = 200;
constexpr double reported_cost = 0.00042;
constexpr std::size_t small_limit = 16;
constexpr std::size_t max_error_type_length = 64;

// Router comment, content split across chunks (with a two-byte UTF-8 character), duplicate
// finish chunk, usage chunk carrying router metadata, then [DONE]. Values match the constants.
constexpr std::string_view realistic_stream =
    ": OPENROUTER PROCESSING\n\n"
    R"(data: {"id":"gen-1","model":"example/model-a","choices":[{"index":0,"delta":{"role":"assistant","content":"Hel"},"finish_reason":null,"native_finish_reason":null}]})"
    "\n\n"
    R"(data: {"id":"gen-1","model":"example/model-a","choices":[{"index":0,"delta":{"content":"lo caf)"
    "\xC3\xA9"
    R"("},"finish_reason":null,"native_finish_reason":null}]})"
    "\n\n"
    ": keepalive\n\n"
    R"(data: {"id":"gen-1","model":"example/model-a","choices":[{"index":0,"delta":{"content":""},"finish_reason":"stop","native_finish_reason":"end_turn"}]})"
    "\n\n"
    R"(data: {"id":"gen-1","model":"example/model-a","choices":[{"index":0,"delta":{},"finish_reason":"stop","native_finish_reason":"end_turn"}]})"
    "\n\n"
    R"(data: {"id":"gen-1","model":"example/model-a","choices":[],"usage":{"prompt_tokens":1200,"prompt_tokens_details":{"cached_tokens":800,"cache_write_tokens":300},"completion_tokens":450,"completion_tokens_details":{"reasoning_tokens":200},"cost":0.00042},"openrouter_metadata":{"endpoints":{"available":[{"provider":"Other","selected":false},{"provider":"Example","selected":true}]}}})"
    "\n\n"
    "data: [DONE]\n\n";

std::vector<std::string> feed_ok(SseParser& parser, std::string_view bytes) {
    auto result = parser.feed(bytes);
    REQUIRE(result.has_value());
    return std::move(*result);
}

std::vector<std::string> feed_all(SseParser& parser,
                                  std::initializer_list<std::string_view> pieces) {
    std::vector<std::string> events;
    for (const std::string_view piece : pieces) {
        for (std::string& event : feed_ok(parser, piece)) {
            events.push_back(std::move(event));
        }
    }
    return events;
}

std::vector<std::string> parse_events(std::string_view stream) {
    SseParser parser;
    return feed_ok(parser, stream);
}

std::string apply_ok(ChatStream& chat, std::string_view payload) {
    auto result = chat.on_data(payload);
    REQUIRE(result.has_value());
    return std::move(*result);
}

StreamOutcome run_stream(std::string_view stream, std::size_t chunk_size) {
    SseParser parser;
    ChatStream chat;
    for (std::size_t offset = 0; offset < stream.size(); offset += chunk_size) {
        for (const std::string& payload : feed_ok(parser, stream.substr(offset, chunk_size))) {
            apply_ok(chat, payload);
        }
    }
    return chat.outcome();
}

StreamOutcome apply_all(std::initializer_list<std::string_view> payloads) {
    ChatStream chat;
    for (const std::string_view payload : payloads) {
        apply_ok(chat, payload);
    }
    return chat.outcome();
}

void check_realistic(const StreamOutcome& outcome) {
    CHECK(outcome.text == "Hello caf\xC3\xA9");
    CHECK(outcome.finish_reason == "stop");
    CHECK(outcome.native_finish_reason == "end_turn");
    CHECK(outcome.generation_id == "gen-1");
    CHECK(outcome.served_model == "example/model-a");
    CHECK(outcome.provider == "Example");
    CHECK(outcome.usage.prompt_tokens == prompt_tokens);
    CHECK(outcome.usage.cached_tokens == cached_tokens);
    CHECK(outcome.usage.cache_write_tokens == cache_write_tokens);
    CHECK(outcome.usage.completion_tokens == completion_tokens);
    CHECK(outcome.usage.reasoning_tokens == reasoning_tokens);
    CHECK(outcome.usage.cost == reported_cost);
    CHECK(outcome.done);
    CHECK_FALSE(outcome.error_code.has_value());
    CHECK(orcli::stream_complete(outcome));
}

// Takes the error type already escaped for a JSON string literal.
std::optional<std::string> error_type_of(std::string_view escaped_type) {
    std::string body{R"({"error":{"code":400,"metadata":{"error_type":")"};
    body.append(escaped_type);
    body.append(R"("}}})");
    StreamOutcome outcome;
    REQUIRE(orcli::parse_error_body(body, outcome));
    return outcome.error_type;
}

} // namespace

TEST_CASE("every chunk size of a realistic stream gives the same outcome") {
    check_realistic(run_stream(realistic_stream, realistic_stream.size()));
    for (std::size_t chunk_size = 1; chunk_size < realistic_stream.size(); ++chunk_size) {
        INFO("chunk size ", chunk_size);
        check_realistic(run_stream(realistic_stream, chunk_size));
    }
}

TEST_CASE("CRLF and lone CR end lines") {
    const std::vector<std::string> expected{"a", "b"};
    CHECK(parse_events("data: a\r\n\r\ndata: b\r\n\r\n") == expected);
    CHECK(parse_events("data: a\r\rdata: b\r\r") == expected);
    CHECK(parse_events("data: a\n\rdata: b\r\n\n") == expected);
}

TEST_CASE("CR at the end of a chunk and LF at the start of the next are one line break") {
    SseParser parser;
    const std::vector<std::string> expected{"a\nb"};
    CHECK(feed_all(parser, {"data: a\r", "\ndata: b\r", "\n\r", "\n"}) == expected);
    CHECK(feed_all(parser, {"data: c\r", "", "\n", "\r", "\n"}) == std::vector<std::string>{"c"});
}

TEST_CASE("multi-line data is joined with LF") {
    const std::vector<std::string> expected{"one\ntwo\n"};
    CHECK(parse_events("data: one\ndata: two\ndata\n\n") == expected);
}

TEST_CASE("data values lose exactly one leading space") {
    const std::vector<std::string> expected{"x", "x", " x", "", "a:b"};
    CHECK(parse_events("data:x\n\ndata: x\n\ndata:  x\n\ndata\n\ndata: a:b\n\n") == expected);
}

TEST_CASE("comment lines are ignored") {
    CHECK(parse_events(": keepalive\n\n:\n\n").empty());
    const std::vector<std::string> expected{"a"};
    CHECK(parse_events(": one\ndata: a\n: two\n\n") == expected);
}

TEST_CASE("fields other than data are ignored") {
    CHECK(parse_events("event: message\nid: 7\nretry: 10\n\n").empty());
    const std::vector<std::string> expected{"a"};
    CHECK(parse_events("event: message\nid: 7\nretry: 10\ndatum: z\nfoo\ndata: a\n\n") == expected);
}

TEST_CASE("an event without its blank line is not dispatched") {
    SseParser parser;
    CHECK(feed_ok(parser, "data: a\n").empty());
    const std::vector<std::string> expected{"a"};
    CHECK(feed_ok(parser, "\n") == expected);
}

TEST_CASE("an event larger than the limit fails") {
    SUBCASE("one long line, even before its line break") {
        SseParser parser{small_limit};
        const auto result = parser.feed("data: 0123456789abcdef");
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Exit::api);
    }
    SUBCASE("data accumulated over several lines") {
        SseParser parser{small_limit};
        CHECK(feed_ok(parser, "data: 0123456\n").empty());
        const auto result = parser.feed("data: 0123456\n");
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Exit::api);
    }
    SUBCASE("events within the limit pass") {
        SseParser parser{small_limit};
        const std::vector<std::string> expected{"0123456789", "0123456789"};
        CHECK(feed_ok(parser, "data: 0123456789\n\ndata: 0123456789\n\n") == expected);
    }
}

TEST_CASE("[DONE] marks the stream done and adds no text") {
    ChatStream chat;
    CHECK(apply_ok(chat, "[DONE]").empty());
    CHECK(chat.outcome().done);
    CHECK(chat.outcome().text.empty());
}

TEST_CASE("content accumulates across chunks") {
    ChatStream chat;
    CHECK(apply_ok(chat, R"({"choices":[{"delta":{"content":"ab"}}]})") == "ab");
    CHECK(apply_ok(chat, R"({"choices":[{"delta":{"content":null}}]})").empty());
    CHECK(apply_ok(chat, R"({"choices":[{"delta":{"reasoning":"hidden"}}]})").empty());
    CHECK(apply_ok(chat, R"({"choices":[{"delta":{"content":"cd"}}]})") == "cd");
    CHECK(chat.outcome().text == "abcd");
}

TEST_CASE("duplicate finish chunks and later null finish reasons keep the last value") {
    const StreamOutcome outcome = apply_all({
        R"({"choices":[{"delta":{"content":"a"},"finish_reason":"stop","native_finish_reason":"end_turn"}]})",
        R"({"choices":[{"delta":{},"finish_reason":"stop","native_finish_reason":"end_turn"}]})",
        R"({"choices":[{"delta":{},"finish_reason":null,"native_finish_reason":null}]})",
        "[DONE]",
    });
    CHECK(outcome.finish_reason == "stop");
    CHECK(outcome.native_finish_reason == "end_turn");
    CHECK(orcli::stream_complete(outcome));
}

TEST_CASE("usage chunk is parsed and cost null stays unknown") {
    SUBCASE("cost zero is known") {
        const StreamOutcome outcome = apply_all({R"({"choices":[],"usage":{"cost":0}})"});
        CHECK(outcome.usage.cost.has_value());
        CHECK(outcome.usage.cost == 0.0);
    }
    SUBCASE("cost null is unknown") {
        const StreamOutcome outcome =
            apply_all({R"({"choices":[],"usage":{"prompt_tokens":1200,"cost":null}})"});
        CHECK_FALSE(outcome.usage.cost.has_value());
        CHECK(outcome.usage.prompt_tokens == prompt_tokens);
    }
    SUBCASE("cost missing is unknown") {
        const StreamOutcome outcome = apply_all({R"({"choices":[],"usage":{}})"});
        CHECK_FALSE(outcome.usage.cost.has_value());
        CHECK_FALSE(outcome.usage.prompt_tokens.has_value());
    }
}

TEST_CASE("fields of unexpected types are ignored") {
    const StreamOutcome outcome = apply_all({
        R"({"id":5,"model":null,"error":"x","choices":{"delta":"x"},"usage":{"prompt_tokens":"1200","cost":"0.1","prompt_tokens_details":[1]},"openrouter_metadata":{"endpoints":{"available":{"provider":"x"}}}})",
        R"({"choices":[{"delta":{"content":5},"finish_reason":7}]})",
        R"({"choices":[{"delta":{"content":"a"}}],"usage":{"prompt_tokens":1.5,"completion_tokens":-1}})",
    });
    CHECK(outcome.text == "a");
    CHECK_FALSE(outcome.generation_id.has_value());
    CHECK_FALSE(outcome.served_model.has_value());
    CHECK_FALSE(outcome.provider.has_value());
    CHECK_FALSE(outcome.error_code.has_value());
    CHECK_FALSE(outcome.error_message.has_value());
    CHECK_FALSE(outcome.finish_reason.has_value());
    CHECK_FALSE(outcome.usage.prompt_tokens.has_value());
    CHECK_FALSE(outcome.usage.cost.has_value());
    CHECK(outcome.usage.completion_tokens == -1);
}

TEST_CASE("the first generation id is kept") {
    const StreamOutcome outcome =
        apply_all({R"({"id":"gen-1","choices":[]})", R"({"id":"gen-2","choices":[]})"});
    CHECK(outcome.generation_id == "gen-1");
}

TEST_CASE("error chunk then usage chunk then EOF records both and is not complete") {
    const std::string_view stream =
        R"(data: {"id":"gen-1","choices":[{"delta":{"content":"partial"},"finish_reason":null}]})"
        "\n\n"
        R"(data: {"id":"gen-1","error":{"code":502,"message":"Upstream failed","metadata":{"error_type":"provider_error"}},"choices":[{"delta":{"content":""},"finish_reason":"error"}]})"
        "\n\n"
        R"(data: {"id":"gen-1","choices":[],"usage":{"prompt_tokens":1200,"completion_tokens":450,"cost":0.00042}})"
        "\n\n";
    const StreamOutcome outcome = run_stream(stream, stream.size());
    CHECK(outcome.text == "partial");
    CHECK(outcome.error_code == "502");
    CHECK(outcome.error_message == "provider reported an error");
    CHECK(outcome.error_type == "provider_error");
    CHECK(outcome.finish_reason == "error");
    CHECK(outcome.usage.prompt_tokens == prompt_tokens);
    CHECK(outcome.usage.completion_tokens == completion_tokens);
    CHECK(outcome.usage.cost == reported_cost);
    CHECK_FALSE(outcome.done);
    CHECK_FALSE(orcli::stream_complete(outcome));
}

TEST_CASE("an error object blocks completion even with a string code and no message") {
    const StreamOutcome outcome = apply_all({
        R"({"choices":[{"delta":{"content":"a"},"finish_reason":"stop"}]})",
        R"({"error":{"code":"server_error"},"choices":[]})",
        "[DONE]",
    });
    CHECK_FALSE(outcome.error_code.has_value());
    CHECK(outcome.error_message == "provider reported an error");
    CHECK(outcome.error_type == "provider_error");
    CHECK_FALSE(orcli::stream_complete(outcome));
}

TEST_CASE("stop without [DONE] is not complete") {
    const StreamOutcome outcome =
        apply_all({R"({"choices":[{"delta":{"content":"a"},"finish_reason":"stop"}]})"});
    CHECK(outcome.finish_reason == "stop");
    CHECK_FALSE(orcli::stream_complete(outcome));
}

TEST_CASE("empty text with stop and [DONE] is not complete") {
    const StreamOutcome outcome = apply_all({
        R"({"choices":[{"delta":{"content":""},"finish_reason":"stop"}]})",
        "[DONE]",
    });
    CHECK(outcome.done);
    CHECK_FALSE(orcli::stream_complete(outcome));
}

TEST_CASE("length finish with [DONE] is not complete") {
    const StreamOutcome outcome = apply_all({
        R"({"choices":[{"delta":{"content":"a"},"finish_reason":"length"}]})",
        "[DONE]",
    });
    CHECK_FALSE(orcli::stream_complete(outcome));
}

TEST_CASE("provider comes from the selected openrouter_metadata endpoint") {
    const StreamOutcome outcome = apply_all({
        R"({"choices":[],"openrouter_metadata":{"endpoints":{"available":[{"provider":"First","selected":false},{"provider":"Second","selected":true}]}}})",
        R"({"choices":[],"openrouter_metadata":{"endpoints":{"available":[{"provider":"Third"}]}}})",
    });
    CHECK(outcome.provider == "Second");
}

TEST_CASE("invalid JSON payload fails without echoing it") {
    ChatStream chat;
    for (const std::string_view payload :
         {std::string_view{R"({"secret-text)"}, std::string_view{R"(secret-text})"}}) {
        const auto result = chat.on_data(payload);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Exit::api);
        CHECK(result.error().message.find("secret-text") == std::string::npos);
    }
}

TEST_CASE("parse_error_body reads an error body") {
    StreamOutcome outcome;
    CHECK(orcli::parse_error_body(
        R"({"error":{"code":429,"message":"Slow down","metadata":{"error_type":"rate_limit_exceeded"}},"openrouter_metadata":{"endpoints":{"available":[{"provider":"Example","selected":true}]}}})",
        outcome));
    CHECK(outcome.error_code == "429");
    CHECK(outcome.error_message == "provider reported an error");
    CHECK(outcome.error_type == "provider_error");
    CHECK(outcome.provider == "Example");
    CHECK_FALSE(orcli::stream_complete(outcome));
}

TEST_CASE("provider error types are replaced with a local classification") {
    const std::string longest(max_error_type_length, 'a');
    const std::string too_long(max_error_type_length + 1, 'a');
    CHECK(error_type_of("rate_limit_exceeded") == "provider_error");
    CHECK(error_type_of("v1.provider-error_2") == "provider_error");
    CHECK(error_type_of(longest) == "provider_error");
    CHECK(error_type_of(too_long) == "provider_error");
    CHECK(error_type_of("") == "provider_error");
    CHECK(error_type_of("sk-or-v1-0123abcd leaked") == "provider_error");
    CHECK(error_type_of(R"(sk-or-v1-0123abcd\"leaked)") == "provider_error");
    CHECK(error_type_of(R"(\u001b[31mred\u001b[0m)") == "provider_error");
    CHECK(error_type_of(R"(line\nbreak)") == "provider_error");
    CHECK(error_type_of(R"(\u00e9t\u00e9)") == "provider_error");
}

TEST_CASE("error types shaped like known keys are rejected") {
    const std::string openrouter_key = "sk-or-v1-" + std::string(max_error_type_length, 'a');
    CHECK(error_type_of(openrouter_key) == "provider_error");
    CHECK(error_type_of("sk-ant-api03-0123456789abcdefghij") == "provider_error");
    CHECK(error_type_of("AKIA0123456789ABCDEF") == "provider_error");
}

TEST_CASE("a mid-stream error type is sanitized") {
    const StreamOutcome outcome = apply_all({
        R"({"error":{"code":500,"message":"x","metadata":{"error_type":"bad\u0007type"}},"choices":[]})",
    });
    CHECK(outcome.error_type == "provider_error");
}

TEST_CASE("parse_error_body rejects bodies that are not error JSON") {
    StreamOutcome outcome;
    CHECK_FALSE(orcli::parse_error_body("<html>Bad Gateway</html>", outcome));
    CHECK_FALSE(orcli::parse_error_body("", outcome));
    CHECK_FALSE(orcli::parse_error_body(R"({"choices":[]})", outcome));
    CHECK_FALSE(orcli::parse_error_body(R"({"error":"text"})", outcome));
    CHECK_FALSE(outcome.error_code.has_value());
    CHECK_FALSE(outcome.error_message.has_value());
}

TEST_CASE("an empty data event is ignored instead of failing the stream") {
    orcli::ChatStream stream;
    const auto result = stream.on_data("");
    REQUIRE(result.has_value());
    CHECK(result->empty());
    CHECK(stream.outcome().text.empty());
    CHECK_FALSE(stream.outcome().error_message.has_value());
}

TEST_CASE("later partial usage preserves known accounting") {
    const StreamOutcome outcome = apply_all({
        R"({"usage":{"prompt_tokens":12,"cost":0.002}})",
        R"({"usage":{"completion_tokens":3,"cost":null}})",
        R"({"usage":{"prompt_tokens":18446744073709551615}})",
    });
    CHECK(outcome.usage.prompt_tokens == 12);
    CHECK(outcome.usage.completion_tokens == 3);
    CHECK(outcome.usage.cost == doctest::Approx(0.002));
}

TEST_CASE("an invalid chunk identifier cannot hide a later recovery identifier") {
    const StreamOutcome outcome = apply_all({
        R"({"id":"bad id","choices":[]})",
        R"({"id":"gen-valid_123","choices":[]})",
        R"({"id":"gen-other","choices":[]})",
    });
    CHECK(outcome.generation_id == "gen-valid_123");
}

TEST_CASE("provider errors stop content but retain trailing accounting") {
    const StreamOutcome outcome = apply_all({
        R"({"choices":[{"delta":{"content":"before"}}]})",
        R"({"error":{"code":503,"message":"private prompt","metadata":{"error_type":"private_identifier"}},"choices":[{"delta":{"content":"discard"}}]})",
        R"({"choices":[{"delta":{"content":"also discard"}}],"usage":{"cost":0.01}})",
    });
    CHECK(outcome.text == "before");
    CHECK(outcome.usage.cost == doctest::Approx(0.01));
    CHECK(outcome.error_type == "provider_error");
    REQUIRE(outcome.error_message.has_value());
    CHECK(outcome.error_message->find("private") == std::string::npos);
    CHECK(outcome.error_message->find("routing") != std::string::npos);
}

TEST_CASE("framing rejects control bytes before an event is complete") {
    SseParser parser;
    const auto result = parser.feed(std::string_view{"data: abc\0def", 13});
    CHECK_FALSE(result.has_value());
}

TEST_CASE("valid JSON that is not an object is not a chat event") {
    ChatStream chat;
    CHECK_FALSE(chat.on_data("[]").has_value());
    CHECK_FALSE(chat.on_data("42").has_value());
}

TEST_CASE("an error finish is terminal for answer content even without an error object") {
    const StreamOutcome outcome = apply_all({
        R"({"choices":[{"delta":{"content":"before"}}]})",
        R"({"choices":[{"delta":{},"finish_reason":"error"}]})",
        R"({"choices":[{"delta":{"content":"discard"},"finish_reason":"stop"}],"usage":{"cost":0}})",
        "[DONE]",
    });
    CHECK(outcome.text == "before");
    CHECK(outcome.usage.cost == 0);
    CHECK_FALSE(orcli::stream_complete(outcome));
}
