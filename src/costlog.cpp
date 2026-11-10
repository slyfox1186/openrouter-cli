#include "costlog.hpp"

#include "account.hpp"
#include "common.hpp"
#include "fsutil.hpp"
#include "secrets.hpp"

#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orcli {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr int record_version = 1;
constexpr std::size_t call_id_bytes = 16;
constexpr int compact_dump = -1;
constexpr std::size_t iso_date_length = 10;
constexpr std::size_t thousands_group = 3;

std::optional<std::string> string_at(const json& object, std::string_view key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) {
        return std::nullopt;
    }
    return found->get<std::string>();
}

std::optional<double> number_at(const json& object, std::string_view key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number()) {
        return std::nullopt;
    }
    const double value = found->get<double>();
    return std::isfinite(value) && value >= 0 ? std::optional<double>{value} : std::nullopt;
}

std::optional<long long> integer_at(const json& object, std::string_view key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number_integer()) {
        return std::nullopt;
    }
    if (found->is_number_unsigned() &&
        found->get<unsigned long long>() >
            static_cast<unsigned long long>(std::numeric_limits<long long>::max())) {
        return std::nullopt;
    }
    const auto value = found->get<long long>();
    return value >= 0 ? std::optional<long long>{value} : std::nullopt;
}

template <class T> json nullable(const std::optional<T>& value) {
    if (value.has_value()) {
        return *value;
    }
    return nullptr;
}

struct RecordMetadata {
    std::string_view type;
    std::string_view call_id;
    std::string_view time;
};

json record_header(const RecordMetadata& metadata) {
    json record = json::object();
    record.emplace("v", record_version);
    record.emplace("type", metadata.type);
    record.emplace("call_id", metadata.call_id);
    record.emplace("time", metadata.time);
    return record;
}

void put_tokens(json& record, const Usage& usage) {
    record.emplace("prompt_tokens", nullable(usage.prompt_tokens));
    record.emplace("cached_tokens", nullable(usage.cached_tokens));
    record.emplace("cache_write_tokens", nullable(usage.cache_write_tokens));
    record.emplace("completion_tokens", nullable(usage.completion_tokens));
    record.emplace("reasoning_tokens", nullable(usage.reasoning_tokens));
}

std::string with_thousands(long long value) {
    std::string digits = std::to_string(value);
    const std::size_t first_digit = digits.starts_with('-') ? 1 : 0;
    for (std::size_t end = digits.size(); end > first_digit + thousands_group;
         end -= thousands_group) {
        digits.insert(end - thousands_group, 1, ',');
    }
    return digits;
}

std::string count_text(const std::optional<long long>& count) {
    return count.has_value() ? with_thousands(*count) : std::string("unknown");
}

std::string money_text(const std::optional<double>& amount) {
    return amount.has_value() ? format_usd(*amount) : std::string("unknown");
}

// Everything known about one call_id, gathered from its records in log order.
struct CallRecords {
    std::string call_id;
    std::string earliest_time;
    std::optional<std::string> pending_time;
    std::optional<std::string> model_id;
    std::optional<std::string> generation_id;
    bool has_result = false;
    std::optional<std::string> outcome;
    std::optional<std::string> served_model;
    std::optional<std::string> result_provider;
    std::optional<double> result_cost;
    std::optional<std::string> resolved_provider;
    std::optional<double> resolved_cost;
};

bool known_type(std::string_view type) {
    return type == "pending" || type == "generation" || type == "result" || type == "resolved";
}

bool nonempty_string(const json& record, std::string_view key) {
    const auto value = string_at(record, key);
    return value && !value->empty();
}

bool optional_string(const json& record, std::string_view key) {
    const auto value = record.find(key);
    return value == record.end() || value->is_null() || value->is_string();
}

bool optional_cost(const json& record, std::string_view key) {
    const auto value = record.find(key);
    return value == record.end() || value->is_null() || number_at(record, key).has_value();
}

bool valid_timestamp(const std::optional<std::string>& text) {
    constexpr std::size_t timestamp_length = 20;
    if (!text || text->size() != timestamp_length) {
        return false;
    }
    std::istringstream input{*text};
    std::chrono::sys_seconds timestamp{};
    input >> std::chrono::parse("%FT%TZ", timestamp);
    return !input.fail() && input.peek() == std::istringstream::traits_type::eof();
}

bool valid_record_fields(const json& record, std::string_view type) {
    if (!valid_timestamp(string_at(record, "time"))) {
        return false;
    }
    for (const auto key : {"served_model", "provider", "model"}) {
        if (!optional_string(record, key)) {
            return false;
        }
    }
    if (type == "pending") {
        return nonempty_string(record, "model_id");
    }
    if (type == "generation") {
        const auto id = string_at(record, "generation_id");
        return id && valid_generation_id(*id);
    }
    if (type == "resolved") {
        return optional_cost(record, "total_cost");
    }
    const auto outcome = string_at(record, "outcome");
    constexpr std::array<std::string_view, 4> outcomes{"complete", "incomplete", "error",
                                                       "interrupted"};
    return outcome && std::ranges::contains(outcomes, *outcome) && optional_cost(record, "cost");
}

std::optional<json> parse_record(std::string_view line) {
    auto record = json::parse(line, nullptr, false);
    if (!record.is_object() || integer_at(record, "v") != record_version ||
        !nonempty_string(record, "call_id")) {
        return std::nullopt;
    }
    const auto type = string_at(record, "type");
    if (!type || !known_type(*type) || !valid_record_fields(record, *type)) {
        return std::nullopt;
    }
    return record;
}

void absorb_pending(CallRecords& call, const json& record) {
    if (call.pending_time.has_value()) {
        return;
    }
    call.pending_time = string_at(record, "time");
    call.model_id = string_at(record, "model_id");
}

void absorb_generation(CallRecords& call, const json& record) {
    if (call.generation_id.has_value()) {
        return;
    }
    if (auto generation_id = string_at(record, "generation_id");
        generation_id.has_value() && !generation_id->empty()) {
        call.generation_id = std::move(generation_id);
    }
}

void absorb_result(CallRecords& call, const json& record) {
    if (call.has_result) {
        return;
    }
    call.has_result = true;
    call.outcome = string_at(record, "outcome");
    call.served_model = string_at(record, "served_model");
    call.result_provider = string_at(record, "provider");
    call.result_cost = number_at(record, "cost");
}

// A charge may be resolved more than once (retried lookups); only the first counts.
void absorb_resolved(CallRecords& call, const json& record) {
    if (call.resolved_cost.has_value()) {
        return;
    }
    call.resolved_cost = number_at(record, "total_cost");
    if (!call.resolved_provider.has_value()) {
        call.resolved_provider = string_at(record, "provider");
    }
}

void absorb(CallRecords& call, const json& record) {
    if (const auto time = string_at(record, "time");
        time.has_value() && (call.earliest_time.empty() || *time < call.earliest_time)) {
        call.earliest_time = *time;
    }
    const auto type = string_at(record, "type").value_or("");
    if (type == "pending") {
        absorb_pending(call, record);
    } else if (type == "generation") {
        absorb_generation(call, record);
    } else if (type == "result") {
        absorb_result(call, record);
    } else if (type == "resolved") {
        absorb_resolved(call, record);
    }
}

CallSummary summarize(const CallRecords& call) {
    CallSummary summary{
        .call_id = call.call_id,
        .time = call.pending_time.value_or(call.earliest_time),
        .model = call.model_id.value_or(call.served_model.value_or("unknown")),
        .provider =
            call.result_provider.has_value() ? call.result_provider : call.resolved_provider,
        .generation_id = call.generation_id,
        // The stream's cost and the looked-up charge describe the same call; never add both.
        .cost = call.result_cost.has_value() ? call.result_cost : call.resolved_cost,
        .state = call.has_result ? call.outcome.value_or("unknown") : std::string("pending"),
    };
    return summary;
}

void add_call(SpendReport& report, CallSummary call) {
    if (call.cost.has_value()) {
        report.known_total += *call.cost;
        report.cost_by_day[call.time.substr(0, iso_date_length)] += *call.cost;
        report.cost_by_model[call.model] += *call.cost;
    }
    if (call.state == "pending" || !call.cost.has_value()) {
        report.unresolved.push_back(call);
    }
    report.calls.push_back(std::move(call));
}

} // namespace

std::string new_call_id() {
    return random_hex(call_id_bytes);
}

std::string utc_now_iso() {
    return std::format("{:%FT%TZ}",
                       std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

json pending_record(const PendingInfo& info) {
    json record = record_header({.type = "pending", .call_id = info.call_id, .time = info.time});
    record.emplace("cwd", info.cwd);
    record.emplace("requested_model", info.requested_model);
    record.emplace("model_id", info.model_id);
    record.emplace("privacy", info.privacy);
    record.emplace("effort", nullable(info.effort));
    record.emplace("session", nullable(info.session));
    record.emplace("answer_path", info.answer_path);
    return record;
}

json generation_record(const GenerationInfo& info) {
    json record =
        record_header({.type = "generation", .call_id = info.call_id, .time = utc_now_iso()});
    record.emplace("generation_id", info.generation_id);
    return record;
}

json result_record(const ResultInfo& info) {
    json record = record_header({.type = "result", .call_id = info.call_id, .time = utc_now_iso()});
    record.emplace("outcome", info.outcome);
    record.emplace("served_model", nullable(info.served_model));
    record.emplace("provider", nullable(info.provider));
    put_tokens(record, info.usage);
    record.emplace("cost", nullable(info.usage.cost));
    record.emplace("finish_reason", nullable(info.finish_reason));
    record.emplace("error_type", nullable(info.error_type));
    record.emplace("latency_ms", info.latency_ms);
    return record;
}

json resolved_record(const std::string& call_id, const GenerationStats& stats) {
    json record = record_header({.type = "resolved", .call_id = call_id, .time = utc_now_iso()});
    record.emplace("total_cost", nullable(stats.total_cost));
    record.emplace("provider", nullable(stats.provider));
    record.emplace("model", nullable(stats.model));
    record.emplace("finish_reason", nullable(stats.finish_reason));
    put_tokens(record, stats.usage);
    return record;
}

Result<GenerationStats> parse_generation(std::string_view json_text) {
    const auto root = json::parse(json_text, nullptr, false);
    if (!root.is_object()) {
        return fail(Exit::api, "the generation lookup returned a body that is not a JSON object");
    }
    const auto data = root.find("data");
    if (data == root.end() || !data->is_object()) {
        return fail(Exit::api, "the generation lookup response has no data object");
    }
    GenerationStats stats;
    stats.total_cost = number_at(*data, "total_cost");
    stats.provider = string_at(*data, "provider_name");
    stats.model = string_at(*data, "model");
    stats.finish_reason = string_at(*data, "finish_reason");
    stats.usage.prompt_tokens = integer_at(*data, "native_tokens_prompt");
    stats.usage.completion_tokens = integer_at(*data, "native_tokens_completion");
    stats.usage.cached_tokens = integer_at(*data, "native_tokens_cached");
    stats.usage.reasoning_tokens = integer_at(*data, "native_tokens_reasoning");
    return stats;
}

CostLog::CostLog(fs::path file, fs::path lock_file)
    : file_(std::move(file)), lock_file_(std::move(lock_file)) {}

Result<void> CostLog::append(const json& record, LockDeadline deadline) const {
    const auto made = ensure_private_dir(file_.parent_path());
    if (!made) {
        return std::unexpected(made.error());
    }
    // Paths and model names may hold invalid UTF-8; replacing bad bytes keeps dump from throwing.
    return append_record(
        {.file = file_, .lock_file = lock_file_},
        record.dump(compact_dump, ' ', false, json::error_handler_t::replace) + "\n", deadline);
}

Result<std::string> CostLog::read_snapshot() const {
    std::error_code error;
    const auto parent = file_.parent_path().empty() ? fs::path(".") : file_.parent_path();
    const auto status = fs::symlink_status(parent, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        return fail(Exit::config, "cannot inspect the cost log directory");
    }
    if (status.type() == fs::file_type::not_found) {
        return std::string{};
    }
    const auto lock = FileLock::acquire(lock_file_, true);
    if (!lock) {
        return std::unexpected(lock.error());
    }
    const auto checked = check_private_file(file_);
    if (!checked) {
        return std::unexpected(checked.error());
    }
    return read_regular_file(file_, std::numeric_limits<std::size_t>::max(), true);
}

SpendReport fold_log(const SpendQuery& query) {
    SpendReport report;
    std::vector<CallRecords> calls;
    std::map<std::string, std::size_t> index_by_id;
    for (const auto [index, piece] :
         std::views::enumerate(std::views::split(query.contents, '\n'))) {
        const std::string_view line(piece.begin(), piece.end());
        if (line.empty()) {
            continue;
        }
        const auto record = parse_record(line);
        if (!record.has_value()) {
            report.corrupt_lines.push_back(static_cast<std::size_t>(index) + 1);
            continue;
        }
        const std::string call_id = string_at(*record, "call_id").value_or("");
        const auto [position, inserted] = index_by_id.try_emplace(call_id, calls.size());
        if (inserted) {
            CallRecords fresh;
            fresh.call_id = call_id;
            calls.push_back(std::move(fresh));
        }
        absorb(calls.at(position->second), *record);
    }
    for (const CallRecords& call : calls) {
        CallSummary summary = summarize(call);
        if (!query.since_iso.empty() && std::string_view(summary.time) < query.since_iso) {
            continue;
        }
        add_call(report, std::move(summary));
    }
    return report;
}

std::string format_spend(const SpendReport& report) {
    std::string text =
        std::format("Known spend: {} across {} calls ({} unresolved)\n",
                    format_usd(report.known_total), report.calls.size(), report.unresolved.size());
    if (!report.cost_by_day.empty()) {
        text += "\nBy day:\n";
        for (const auto& [day, cost] : report.cost_by_day) {
            text += std::format("  {}  {}\n", display_metadata(day), format_usd(cost));
        }
    }
    if (!report.cost_by_model.empty()) {
        text += "\nBy model:\n";
        for (const auto& [model, cost] : report.cost_by_model) {
            text += std::format("  {}  {}\n", display_metadata(model), format_usd(cost));
        }
    }
    if (!report.unresolved.empty()) {
        text += "\nUnresolved calls (no result yet, or cost unknown):\n";
        for (const CallSummary& call : report.unresolved) {
            text += std::format("  {}  {}  {}  {}\n", display_metadata(call.call_id),
                                display_metadata(call.time), display_metadata(call.model),
                                display_metadata(call.state));
        }
    }
    if (!report.corrupt_lines.empty()) {
        text += std::format("\nCorrupt log lines (not valid records, not counted): {:n}\n",
                            report.corrupt_lines);
    }
    return text;
}

std::string format_cost_line(const ResultInfo& result, std::string_view model_id,
                             std::string_view answer_path) {
    const Usage& usage = result.usage;
    return std::format(
        "cost {} | {} via {} | in {} (cached {}) | out {} (reasoning {}) | answer {}",
        money_text(usage.cost),
        display_metadata(result.served_model.value_or(std::string(model_id))),
        display_metadata(result.provider.value_or("unknown provider")),
        count_text(usage.prompt_tokens), count_text(usage.cached_tokens),
        count_text(usage.completion_tokens), count_text(usage.reasoning_tokens),
        display_metadata(answer_path));
}

} // namespace orcli
