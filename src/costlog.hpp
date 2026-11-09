#pragma once

#include "common.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orcli {

// 32 lowercase hex characters from std::random_device; shared by the answer file name and
// every cost record of one call.
std::string new_call_id();

// Current time as ISO 8601 UTC with seconds, e.g. "2026-11-09T14:15:02Z".
std::string utc_now_iso();

struct PendingInfo {
    std::string call_id;
    std::string time;
    std::string cwd;
    std::string requested_model;
    std::string model_id;
    std::string privacy; // "zdr" or "no-training"
    std::optional<std::string> effort;
    std::optional<std::string> session;
    std::string answer_path;
};

struct ResultInfo {
    std::string call_id;
    std::string outcome; // "complete", "incomplete", "error" or "interrupted"
    std::optional<std::string> served_model;
    std::optional<std::string> provider;
    Usage usage;
    std::optional<std::string> finish_reason;
    std::optional<std::string> error_type;
    long long latency_ms = 0;
};

struct GenerationStats {
    std::optional<double> total_cost;
    std::optional<std::string> provider;
    std::optional<std::string> model;
    std::optional<std::string> finish_reason;
    Usage usage; // native_tokens_* mapped onto Usage; cost left empty
};

// Record builders. Every record carries "v": 1, "type" and "call_id"; absent values are JSON
// null so unknown is never confused with zero.
nlohmann::json pending_record(const PendingInfo& info);
nlohmann::json generation_record(const std::string& call_id, const std::string& generation_id);
nlohmann::json result_record(const ResultInfo& info);
nlohmann::json resolved_record(const std::string& call_id, const GenerationStats& stats);

// Maps a GET /api/v1/generation response body (data.total_cost, data.provider_name,
// data.model, data.native_tokens_prompt/completion/cached/reasoning, data.finish_reason).
Result<GenerationStats> parse_generation(std::string_view json_text);

// One JSON line per append, through fsutil append_record.
class CostLog {
public:
    CostLog(std::filesystem::path file, std::filesystem::path lock_file);
    Result<void> append(const nlohmann::json& record) const;
    [[nodiscard]] const std::filesystem::path& file() const { return file_; }

private:
    std::filesystem::path file_;
    std::filesystem::path lock_file_;
};

struct CallSummary {
    std::string call_id;
    std::string time;
    std::string model;
    std::optional<std::string> provider;
    // From the `generation` record; lets `spend --resolve` look up an unknown cost later.
    std::optional<std::string> generation_id;
    std::optional<double> cost; // result cost if numeric, else resolved cost, else unknown
    std::string state;          // "complete", "incomplete", "error", "interrupted" or "pending"
};

struct SpendReport {
    std::vector<CallSummary> calls; // in log order, filtered by since
    std::map<std::string, double> cost_by_day;
    std::map<std::string, double> cost_by_model;
    double known_total = 0.0;
    std::vector<CallSummary> unresolved;    // pending with no result, or cost unknown
    std::vector<std::size_t> corrupt_lines; // 1-based line numbers that are not valid records
};

// Folds records by call_id into one call and one cost. since_iso (inclusive, compared on the
// pending record's time) may be empty for all records.
SpendReport fold_log(std::string_view contents, std::string_view since_iso);
std::string format_spend(const SpendReport& report);

// The final stderr line, e.g.
// "cost $0.0412 | google/gemini-4-argon via Google Vertex | in 18,204 (cached 0) |
//  out 3,911 (reasoning 2,870) | answer /path/to/answer.md" (one line). Unknown values print
// as "unknown".
std::string format_cost_line(const ResultInfo& result, std::string_view model_id,
                             std::string_view answer_path);

} // namespace orcli
