#pragma once

#include "common.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orcli {

// Server-sent events decoder. Accepts bytes split anywhere (inside UTF-8 sequences, line
// endings or field names), LF or CRLF line endings, multi-line `data` fields and `:` comment
// lines. Returns the joined `data` payload of every event completed by this call.
class SseParser {
public:
    explicit SseParser(std::size_t max_event_bytes = std::size_t{16} << 20U);

    // Exit::api when one event grows past max_event_bytes.
    Result<std::vector<std::string>> feed(std::string_view bytes);

private:
    std::size_t max_event_bytes_;
    std::string line_;
    std::string data_;
    bool has_data_ = false;
    bool pending_cr_ = false;
};

struct StreamOutcome {
    std::string text;
    std::optional<std::string> finish_reason;
    std::optional<std::string> native_finish_reason;
    std::optional<std::string> generation_id;
    std::optional<std::string> served_model;
    std::optional<std::string> provider;
    std::optional<std::string> error_code;
    std::optional<std::string> error_type;
    std::optional<std::string> error_message; // raw provider text; callers sanitize before display
    Usage usage;
    bool done = false;

    // Non-empty text, finish_reason "stop", [DONE] received and no error.
    [[nodiscard]] bool complete() const;
};

// Interprets OpenRouter Chat Completions stream payloads.
class ChatStream {
public:
    // Applies one `data` payload ("[DONE]" or a JSON chunk) and returns the answer text it
    // added. A chunk with a top-level `error` records the error; later chunks are still read so
    // trailing usage is captured. Exit::api only for a payload that is not valid JSON.
    Result<std::string> on_data(std::string_view payload);

    [[nodiscard]] const StreamOutcome& outcome() const { return outcome_; }

private:
    StreamOutcome outcome_;
};

// Reads a non-streamed error body (HTTP error status or a JSON body without SSE framing) into
// the error fields of an outcome. Returns false when the body is not a recognizable error.
bool parse_error_body(std::string_view body, StreamOutcome& outcome);

} // namespace orcli
