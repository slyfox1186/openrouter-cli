#pragma once

#include "common.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orcli {

// Parsing and display of OpenRouter account endpoints (shapes from
// https://openrouter.ai/openapi.json). Parsers fail with Exit::api naming the endpoint, never
// echoing response text. Absent or null optional fields stay std::nullopt and display as "n/a";
// amounts are USD.

// USD with 4 decimals, more for sub-cent amounts so a small charge never prints as $0.0000.
std::string format_usd(double amount);

// GET /key with the inference key: the calling key's own label, usage and limit.
struct KeyStatus {
    std::string label;
    std::optional<double> usage;
    std::optional<double> usage_daily;
    std::optional<double> usage_weekly;
    std::optional<double> usage_monthly;
    std::optional<double> limit;
    std::optional<double> limit_remaining;
    std::optional<std::string> limit_reset;
    std::optional<std::string> expires_at;
    bool is_free_tier = false;
    std::optional<std::string> creator_user_id;
    std::optional<std::string> organization_id; // null when an individual account owns the key
    std::optional<std::string> workspace_id;
    // Required in the response: an absent value must not pass as "not a management key".
    bool is_management_key = false;
};
Result<KeyStatus> parse_key_status(std::string_view json_text);
std::string format_key_status(const KeyStatus& status);

// One row of GET /keys (management key). `hash` identifies a key; no secret is ever returned.
struct KeyEntry {
    std::string hash;
    std::string label;
    std::string name;
    bool disabled = false;
    std::optional<std::string> created_at;
    std::optional<std::string> expires_at;
    std::optional<std::string> last_used_at;
    std::optional<double> usage;
    std::optional<double> usage_daily;
    std::optional<double> usage_weekly;
    std::optional<double> usage_monthly;
    std::optional<double> limit;
    std::optional<double> limit_remaining;
    std::optional<std::string> limit_reset;
};
// One page: the `data` array. An empty vector marks the end of paging.
Result<std::vector<KeyEntry>> parse_keys_page(std::string_view json_text);
// Table with a first line stating the scope, e.g. "API keys in this management key's workspace
// (an organization-scoped key lists its default workspace), disabled and expired included".
std::string format_keys(const std::vector<KeyEntry>& keys);

// One row of GET /organization/members (management key).
struct MemberEntry {
    std::string id;
    std::string email;
    std::optional<std::string> first_name;
    std::optional<std::string> last_name;
    std::string role; // e.g. "org:admin"; new roles are passed through unchanged
};
// One page: the `data` array (total_count is ignored). An empty vector marks the end of paging.
Result<std::vector<MemberEntry>> parse_members_page(std::string_view json_text);

// GET /credits (management key).
struct Credits {
    double total_credits = 0;
    double total_usage = 0;
};
Result<Credits> parse_credits(std::string_view json_text);
std::string format_credits(const Credits& credits); // purchased, used, remaining

// GET /activity (management key): rows per UTC date, model and endpoint for the last 30
// completed UTC days.
struct ActivityRow {
    std::string date; // YYYY-MM-DD
    std::string model;
    std::string provider;
    long long requests = 0;
    long long prompt_tokens = 0;
    long long completion_tokens = 0;
    long long reasoning_tokens = 0; // shown beside completion tokens, never added to them
    double usage = 0;
};
Result<std::vector<ActivityRow>> parse_activity(std::string_view json_text);

// Keeps rows whose date is one of the `days` completed UTC days before `today_utc`
// (YYYY-MM-DD), sums rows sharing date, model and provider, and sorts by date (newest first),
// then usage (largest first). days must be 1 to 30.
std::vector<ActivityRow> summarize_activity(const std::vector<ActivityRow>& rows, int days,
                                            std::string_view today_utc);
// Table plus a total line; states the date range shown.
std::string format_activity(const std::vector<ActivityRow>& rows, int days,
                            std::string_view today_utc);

} // namespace orcli
