#include "sse.hpp"

#include "common.hpp"

#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orcli {
namespace {

using nlohmann::json;

constexpr std::string_view line_breaks{"\r\n"};
constexpr std::string_view data_field{"data"};
constexpr std::string_view done_marker{"[DONE]"};

// Value of a `data` field line, or nullopt for any other field. Comment lines have an empty
// field name, so they are skipped like unknown fields.
std::optional<std::string_view> data_value(std::string_view line) {
    const std::size_t colon = line.find(':');
    if (line.substr(0, colon) != data_field) {
        return std::nullopt;
    }
    if (colon == std::string_view::npos) {
        return std::string_view{};
    }
    std::string_view value = line.substr(colon + 1);
    if (value.starts_with(' ')) {
        value.remove_prefix(1);
    }
    return value;
}

// Chunk fields are read defensively: a missing key or a value of an unexpected type yields a
// null value instead of throwing, so one odd chunk never aborts a paid stream.
const json& member(const json& object, std::string_view key) {
    static const json absent;
    const auto found = object.find(key);
    return found == object.end() ? absent : *found;
}

std::optional<std::string> string_member(const json& object, std::string_view key) {
    const json& value = member(object, key);
    if (!value.is_string()) {
        return std::nullopt;
    }
    return value.get<std::string>();
}

std::optional<long long> count_member(const json& object, std::string_view key) {
    const json& value = member(object, key);
    if (value.is_number_unsigned()) {
        const auto count = value.get<std::uint64_t>();
        if (!std::in_range<long long>(count)) {
            return std::nullopt;
        }
        return static_cast<long long>(count);
    }
    if (value.is_number_integer()) {
        return value.get<long long>();
    }
    return std::nullopt;
}

std::optional<double> number_member(const json& object, std::string_view key) {
    const json& value = member(object, key);
    if (!value.is_number()) {
        return std::nullopt;
    }
    const double number = value.get<double>();
    return std::isfinite(number) && number >= 0 ? std::optional{number} : std::nullopt;
}

// Only numeric codes are safe to repeat; string codes may contain echoed private input.
std::optional<std::string> code_member(const json& object, std::string_view key) {
    const json& value = member(object, key);
    if (value.is_number_integer()) {
        return value.dump();
    }
    return std::nullopt;
}

// Keeps the previous value when the chunk has none, because later chunks carry null finish
// reasons and omit fields already sent.
void record_string(std::optional<std::string>& target, const json& object, std::string_view key) {
    if (auto value = string_member(object, key); value.has_value()) {
        target = std::move(value);
    }
}

void record_provider(const json& body, StreamOutcome& outcome) {
    const json& available =
        member(member(member(body, "openrouter_metadata"), "endpoints"), "available");
    if (!available.is_array()) {
        return;
    }
    for (const json& endpoint : available) {
        const json& selected = member(endpoint, "selected");
        if (selected.is_boolean() && selected.get<bool>()) {
            record_string(outcome.provider, endpoint, "provider");
            return;
        }
    }
}

void record_error(const json& error, StreamOutcome& outcome) {
    outcome.error_code = code_member(error, "code");
    outcome.error_type = "provider_error";
    outcome.error_message = "provider reported an error";
    if (outcome.error_code == "404" || outcome.error_code == "503") {
        outcome.error_message =
            "no available route; check the model, provider and privacy routing settings";
    }
}

template <class T> void record_known(std::optional<T>& target, std::optional<T> value) {
    if (value.has_value()) {
        target = std::move(value);
    }
}

void record_usage(const json& usage, Usage& totals) {
    const json& prompt_details = member(usage, "prompt_tokens_details");
    const json& completion_details = member(usage, "completion_tokens_details");
    record_known(totals.prompt_tokens, count_member(usage, "prompt_tokens"));
    record_known(totals.cached_tokens, count_member(prompt_details, "cached_tokens"));
    record_known(totals.cache_write_tokens, count_member(prompt_details, "cache_write_tokens"));
    record_known(totals.completion_tokens, count_member(usage, "completion_tokens"));
    record_known(totals.reasoning_tokens, count_member(completion_details, "reasoning_tokens"));
    record_known(totals.cost, number_member(usage, "cost"));
}

void record_chunk(const json& chunk, StreamOutcome& outcome) {
    if (!outcome.generation_id.has_value()) {
        auto chunk_id = string_member(chunk, "id");
        if (chunk_id && valid_generation_id(*chunk_id)) {
            outcome.generation_id = std::move(chunk_id);
        }
    }
    record_string(outcome.served_model, chunk, "model");
    record_provider(chunk, outcome);
    if (const json& error = member(chunk, "error"); error.is_object()) {
        record_error(error, outcome);
    }
    if (const json& usage = member(chunk, "usage"); usage.is_object()) {
        record_usage(usage, outcome.usage);
    }
}

std::string record_choice(const json& chunk, StreamOutcome& outcome) {
    const json& choices = member(chunk, "choices");
    if (!choices.is_array() || choices.empty()) {
        return {};
    }
    const json& choice = choices.front();
    record_string(outcome.finish_reason, choice, "finish_reason");
    record_string(outcome.native_finish_reason, choice, "native_finish_reason");
    if (outcome.finish_reason == "error" && !outcome.error_type.has_value()) {
        outcome.error_type = "provider_error";
        outcome.error_message = "provider reported an error";
    }
    std::optional<std::string> content = string_member(member(choice, "delta"), "content");
    if (!content.has_value() || outcome.error_type.has_value()) {
        return {};
    }
    outcome.text.append(*content);
    return std::move(*content);
}

} // namespace

SseParser::SseParser(std::size_t max_event_bytes) : max_event_bytes_(max_event_bytes) {}

Result<std::vector<std::string>> SseParser::feed(std::string_view bytes) {
    if (bytes.contains('\0')) {
        return fail(Exit::api, "invalid control byte in server-sent event");
    }
    std::vector<std::string> events;
    while (!bytes.empty()) {
        // A CR ending the previous piece and an LF starting this one form one CRLF line break.
        if (std::exchange(pending_cr_, false) && bytes.front() == '\n') {
            bytes.remove_prefix(1);
            continue;
        }
        const std::size_t end = bytes.find_first_of(line_breaks);
        line_.append(bytes.substr(0, end));
        if (line_.size() + data_.size() > max_event_bytes_) {
            return fail(Exit::api, "server-sent event exceeds the size limit");
        }
        if (end == std::string_view::npos) {
            break;
        }
        pending_cr_ = bytes.at(end) == '\r';
        bytes.remove_prefix(end + 1);
        if (line_.empty()) {
            if (has_data_) {
                events.push_back(std::exchange(data_, std::string{}));
                has_data_ = false;
            }
        } else if (const auto value = data_value(line_); value.has_value()) {
            if (has_data_) {
                data_.push_back('\n');
            }
            data_.append(*value);
            has_data_ = true;
        }
        line_.clear();
    }
    return events;
}

bool stream_complete(const StreamOutcome& outcome) {
    return !outcome.text.empty() && outcome.finish_reason == "stop" && outcome.done &&
           !outcome.error_code.has_value() && !outcome.error_type.has_value() &&
           !outcome.error_message.has_value();
}

Result<std::string> ChatStream::on_data(std::string_view payload) {
    if (payload == done_marker) {
        outcome_.done = true;
        return std::string{};
    }
    // An empty data event carries nothing; failing on it would abort a call that is being billed.
    if (payload.empty()) {
        return std::string{};
    }
    try {
        const json chunk = json::parse(payload, nullptr, false);
        if (chunk.is_discarded() || !chunk.is_object()) {
            return fail(Exit::api, "invalid JSON object in stream");
        }
        record_chunk(chunk, outcome_);
        return record_choice(chunk, outcome_);
    } catch (const json::exception&) {
        return fail(Exit::api, "unreadable JSON value in stream");
    }
}

bool parse_error_body(std::string_view body, StreamOutcome& outcome) {
    try {
        const json parsed = json::parse(body, nullptr, false);
        const json& error = member(parsed, "error");
        if (!error.is_object()) {
            return false;
        }
        record_error(error, outcome);
        record_provider(parsed, outcome);
        return true;
    } catch (const json::exception&) {
        return false;
    }
}

} // namespace orcli
