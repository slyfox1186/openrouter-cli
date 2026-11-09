#include "costlog.hpp"

#include "common.hpp"
#include "fsutil.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <random>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orcli {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr int record_version = 1;
constexpr std::size_t call_id_chunks = 4;
constexpr int compact_dump = -1;
constexpr std::size_t iso_date_length = 10;
constexpr std::size_t thousands_group = 3;
constexpr int money_decimals = 4;
constexpr int max_money_decimals = 8;
constexpr double smallest_four_decimal_amount = 0.0001;

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
    return found->get<double>();
}

std::optional<long long> integer_at(const json& object, std::string_view key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number_integer()) {
        return std::nullopt;
    }
    return found->get<long long>();
}

template <class T> json nullable(const std::optional<T>& value) {
    if (value.has_value()) {
        return json(*value);
    }
    return json(nullptr);
}

json record_header(std::string_view type, const std::string& call_id, const std::string& time) {
    json record = json::object();
    record["v"] = record_version;
    record["type"] = type;
    record["call_id"] = call_id;
    record["time"] = time;
    return record;
}

void put_tokens(json& record, const Usage& usage) {
    record["prompt_tokens"] = nullable(usage.prompt_tokens);
    record["cached_tokens"] = nullable(usage.cached_tokens);
    record["cache_write_tokens"] = nullable(usage.cache_write_tokens);
    record["completion_tokens"] = nullable(usage.completion_tokens);
    record["reasoning_tokens"] = nullable(usage.reasoning_tokens);
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

// Sub-cent calls are common with cheap models, so a tiny charge must not print as $0.0000.
std::string money(double amount) {
    if (amount <= 0.0 || amount >= smallest_four_decimal_amount) {
        return std::format("${:.{}f}", amount, money_decimals);
    }
    std::string text;
    for (int decimals = money_decimals + 1; decimals <= max_money_decimals; ++decimals) {
        text = std::format("${:.{}f}", amount, decimals);
        if (text.find_first_of("123456789") != std::string::npos) {
            break;
        }
    }
    return text;
}

std::string money_text(const std::optional<double>& amount) {
    return amount.has_value() ? money(*amount) : std::string("unknown");
}

// Everything known about one call_id, gathered from its records in log order.
struct CallRecords {
    std::string call_id;
    std::string earliest_time;
    std::optional<std::string> pending_time;
    std::optional<std::string> model_id;
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

std::optional<json> parse_record(std::string_view line) {
    auto record = json::parse(line, nullptr, false);
    if (!record.is_object() || !integer_at(record, "v").has_value() ||
        !string_at(record, "call_id").has_value()) {
        return std::nullopt;
    }
    const auto type = string_at(record, "type");
    if (!type.has_value() || !known_type(*type)) {
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
    std::random_device device;
    std::uniform_int_distribution<std::uint32_t> distribution;
    std::string hex;
    for (std::size_t index = 0; index < call_id_chunks; ++index) {
        hex += std::format("{:08x}", distribution(device));
    }
    return hex;
}

std::string utc_now_iso() {
    return std::format("{:%FT%TZ}",
                       std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

json pending_record(const PendingInfo& info) {
    json record = record_header("pending", info.call_id, info.time);
    record["cwd"] = info.cwd;
    record["requested_model"] = info.requested_model;
    record["model_id"] = info.model_id;
    record["privacy"] = info.privacy;
    record["effort"] = nullable(info.effort);
    record["session"] = nullable(info.session);
    record["answer_path"] = info.answer_path;
    return record;
}

json generation_record(const std::string& call_id, const std::string& generation_id) {
    json record = record_header("generation", call_id, utc_now_iso());
    record["generation_id"] = generation_id;
    return record;
}

json result_record(const ResultInfo& info) {
    json record = record_header("result", info.call_id, utc_now_iso());
    record["outcome"] = info.outcome;
    record["served_model"] = nullable(info.served_model);
    record["provider"] = nullable(info.provider);
    put_tokens(record, info.usage);
    record["cost"] = nullable(info.usage.cost);
    record["finish_reason"] = nullable(info.finish_reason);
    record["error_type"] = nullable(info.error_type);
    record["latency_ms"] = info.latency_ms;
    return record;
}

json resolved_record(const std::string& call_id, const GenerationStats& stats) {
    json record = record_header("resolved", call_id, utc_now_iso());
    record["total_cost"] = nullable(stats.total_cost);
    record["provider"] = nullable(stats.provider);
    record["model"] = nullable(stats.model);
    record["finish_reason"] = nullable(stats.finish_reason);
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

Result<void> CostLog::append(const json& record) const {
    const auto made = ensure_private_dir(file_.parent_path());
    if (!made) {
        return std::unexpected(made.error());
    }
    // Paths and model names may hold invalid UTF-8; replacing bad bytes keeps dump from throwing.
    return append_record(file_, lock_file_,
                         record.dump(compact_dump, ' ', false, json::error_handler_t::replace) +
                             "\n");
}

SpendReport fold_log(std::string_view contents, std::string_view since_iso) {
    SpendReport report;
    std::vector<CallRecords> calls;
    std::map<std::string, std::size_t> index_by_id;
    for (const auto [index, piece] : std::views::enumerate(std::views::split(contents, '\n'))) {
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
        if (!since_iso.empty() && std::string_view(summary.time) < since_iso) {
            continue;
        }
        add_call(report, std::move(summary));
    }
    return report;
}

std::string format_spend(const SpendReport& report) {
    std::string text =
        std::format("Known spend: {} across {} calls ({} unresolved)\n", money(report.known_total),
                    report.calls.size(), report.unresolved.size());
    if (!report.cost_by_day.empty()) {
        text += "\nBy day:\n";
        for (const auto& [day, cost] : report.cost_by_day) {
            text += std::format("  {}  {}\n", day, money(cost));
        }
    }
    if (!report.cost_by_model.empty()) {
        text += "\nBy model:\n";
        for (const auto& [model, cost] : report.cost_by_model) {
            text += std::format("  {}  {}\n", model, money(cost));
        }
    }
    if (!report.unresolved.empty()) {
        text += "\nUnresolved calls (no result yet, or cost unknown):\n";
        for (const CallSummary& call : report.unresolved) {
            text +=
                std::format("  {}  {}  {}  {}\n", call.call_id, call.time, call.model, call.state);
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
        money_text(usage.cost), result.served_model.value_or(std::string(model_id)),
        result.provider.value_or("unknown provider"), count_text(usage.prompt_tokens),
        count_text(usage.cached_tokens), count_text(usage.completion_tokens),
        count_text(usage.reasoning_tokens), answer_path);
}

} // namespace orcli
