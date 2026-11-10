#include "account.hpp"

#include "common.hpp"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace {

using orcli::ActivityRow;
using orcli::Exit;

TEST_CASE("activity dates are validated before records enter summaries") {
    for (const std::string_view date : {"2026-02-30", "2026-11-09junk", "bad"}) {
        const nlohmann::json row{{"date", date},          {"model", "a/b"},
                                 {"provider_name", "p"},  {"requests", 1},
                                 {"prompt_tokens", 0},    {"completion_tokens", 0},
                                 {"reasoning_tokens", 0}, {"usage", 0}};
        CHECK_FALSE(orcli::parse_activity(nlohmann::json{{"data", {row}}}.dump()).has_value());
    }
}

TEST_CASE("tiny nonzero charges never display as zero") {
    constexpr double tiny = 1e-12;
    CHECK(orcli::format_usd(tiny) != "$0.00000000");
    CHECK(orcli::format_usd(tiny).find("1e-12") != std::string::npos);
}

// Shapes from the OpenRouter OpenAPI document (GET /key, /keys, /credits, /activity).
constexpr std::string_view key_json = R"json({"data": {
  "label": "sk-or-v1-au7...890", "usage": 25.5, "usage_daily": 1.25, "usage_weekly": 5.5,
  "usage_monthly": 20, "limit": 100, "limit_remaining": 74.5, "limit_reset": "monthly",
  "expires_at": "2027-12-31T23:59:59Z", "is_free_tier": false, "is_management_key": false,
  "creator_user_id": "user_2dHF", "organization_id": "org_9", "workspace_id": "w1",
  "rate_limit": {"interval": "1h", "note": "deprecated", "requests": 1000}}})json";

constexpr std::string_view key_null_json = R"json({"data": {
  "label": "sk-or-v1-abc...def", "usage": 0, "usage_daily": 0, "usage_weekly": 0,
  "usage_monthly": 0, "limit": null, "limit_remaining": null, "limit_reset": null,
  "expires_at": null, "is_free_tier": true, "is_management_key": false,
  "creator_user_id": null, "organization_id": null, "workspace_id": null}})json";

constexpr std::string_view keys_json = R"json({"data": [
  {"hash": "f01d52606dc8f0a8", "label": "sk-or-v1-0e6...1c96", "name": "Production",
   "disabled": false, "created_at": "2025-08-24T10:30:00Z", "expires_at": null,
   "last_used_at": "2026-09-15T15:45:00Z", "usage": 25.5, "usage_daily": 0.5,
   "usage_weekly": 2, "usage_monthly": 10, "limit": 100, "limit_remaining": 74.5,
   "limit_reset": "monthly", "updated_at": null, "workspace_id": "w1"},
  {"hash": "aa11bb22cc33dd44", "label": "sk-or-v1-111...2222", "name": "Old laptop",
   "disabled": true, "created_at": "2024-01-02T00:00:00Z", "expires_at": "2025-01-01T00:00:00Z",
   "last_used_at": null, "usage": 3, "usage_daily": 0, "usage_weekly": 0, "usage_monthly": 0,
   "limit": null, "limit_remaining": null, "limit_reset": null}
]})json";

constexpr std::string_view credits_json = R"json({"data": {"total_credits": 100.5,
  "total_usage": 25.75}})json";

constexpr std::string_view activity_json = R"json({"data": [
  {"date": "2026-11-08", "model": "openai/gpt-6-sol", "model_permaslug": "openai/gpt-6-sol-1",
   "endpoint_id": "e1", "provider_name": "OpenAI", "usage": 0.015, "byok_usage_inference": 0,
   "requests": 5, "prompt_tokens": 50, "completion_tokens": 125, "reasoning_tokens": 25,
   "cached_tokens": 10}
]})json";

constexpr std::string_view members_json = R"json({"data": [
  {"id": "user_2dHF", "email": "jane.doe@example.com", "first_name": "Jane",
   "last_name": "Doe", "role": "org:admin"},
  {"id": "user_3abc", "email": "sam@example.com", "first_name": null, "last_name": null,
   "role": "org:member"}
], "total_count": 2})json";

constexpr double key_usage = 25.5;
constexpr double key_limit = 100;
constexpr double key_limit_remaining = 74.5;
constexpr double credits_total = 100.5;
constexpr double credits_used = 25.75;
constexpr long long activity_requests = 5;
constexpr long long activity_completion = 125;
constexpr long long activity_reasoning = 25;
constexpr std::string_view today = "2026-11-09";
constexpr int window_days = 3;

ActivityRow activity(std::string_view date, std::string_view provider, long long requests,
                     double usage) {
    return ActivityRow{.date = std::string(date),
                       .model = "openai/gpt-6-sol",
                       .provider = std::string(provider),
                       .requests = requests,
                       .prompt_tokens = requests,
                       .completion_tokens = requests,
                       .reasoning_tokens = requests,
                       .usage = usage};
}

// The day before `today`, the last day of the window, and the days just outside it.
std::vector<ActivityRow> window_rows() {
    constexpr double small = 0.25;
    constexpr double large = 0.75;
    return {activity("2026-11-09", "OpenAI", 1, small), activity("2026-11-08", "OpenAI", 2, small),
            activity("2026-11-08", "Azure", 1, large),  activity("2026-11-08", "OpenAI", 3, small),
            activity("2026-11-06", "OpenAI", 4, small), activity("2026-11-05", "OpenAI", 1, small),
            activity("not-a-date", "OpenAI", 1, small)};
}

void check_api_error(const orcli::Error& error, std::string_view path) {
    CHECK(error.code == Exit::api);
    CHECK(error.message == std::string("unexpected response from GET ") + std::string(path));
}

} // namespace

TEST_CASE("parse_key_status reads the key's usage and limit") {
    const orcli::KeyStatus status = orcli::parse_key_status(key_json).value();
    CHECK(status.label == "sk-or-v1-au7...890");
    CHECK(status.usage == key_usage);
    CHECK(status.limit == key_limit);
    CHECK(status.limit_remaining == key_limit_remaining);
    CHECK(status.limit_reset == "monthly");
    CHECK_FALSE(status.is_free_tier);
}

TEST_CASE("parse_key_status keeps null fields unset and shows them as n/a") {
    const orcli::KeyStatus status = orcli::parse_key_status(key_null_json).value();
    CHECK_FALSE(status.limit.has_value());
    CHECK_FALSE(status.limit_remaining.has_value());
    CHECK_FALSE(status.expires_at.has_value());
    CHECK(status.is_free_tier);
    const std::string text = orcli::format_key_status(status);
    CHECK(text.contains("Limit:              n/a\n"));
    CHECK(text.contains("Expires:            n/a\n"));
}

TEST_CASE("format_key_status prints amounts in USD") {
    const std::string text = orcli::format_key_status(orcli::parse_key_status(key_json).value());
    CHECK(text.contains("$25.5000"));
    CHECK(text.contains("$1.2500"));
    CHECK(text.contains("$74.5000"));
    CHECK(text.contains("2027-12-31T23:59:59Z"));
}

TEST_CASE("parse_key_status rejects malformed JSON and wrong types without echoing them") {
    for (const std::string_view body :
         {"secret prompt text", R"({"data": []})",
          R"({"data": {"label": 5, "is_management_key": false}})",
          R"({"data": {"label": "k", "is_management_key": false, "usage": "25.5"}})",
          R"({"data": {"label": "k", "is_management_key": false, "is_free_tier": "no"}})",
          R"({"label": "k"})"}) {
        const auto parsed = orcli::parse_key_status(body);
        REQUIRE_FALSE(parsed.has_value());
        check_api_error(parsed.error(), "/key");
    }
}

TEST_CASE("parse_key_status reads the key's identity") {
    const orcli::KeyStatus status = orcli::parse_key_status(key_json).value();
    CHECK(status.creator_user_id == "user_2dHF");
    CHECK(status.organization_id == "org_9");
    CHECK(status.workspace_id == "w1");
    CHECK_FALSE(status.is_management_key);
}

TEST_CASE("parse_key_status keeps null identity fields unset") {
    const orcli::KeyStatus status = orcli::parse_key_status(key_null_json).value();
    CHECK_FALSE(status.creator_user_id.has_value());
    CHECK_FALSE(status.organization_id.has_value());
    CHECK_FALSE(status.workspace_id.has_value());
}

TEST_CASE("parse_key_status reads a management key") {
    const auto status = orcli::parse_key_status(
        R"({"data": {"label": "k", "is_management_key": true, "creator_user_id": "u"}})");
    REQUIRE(status.has_value());
    CHECK(status.value().is_management_key);
}

TEST_CASE("parse_key_status requires a boolean is_management_key") {
    for (const std::string_view body :
         {R"({"data": {"label": "k"}})", R"({"data": {"label": "k", "is_management_key": null}})",
          R"({"data": {"label": "k", "is_management_key": "false"}})",
          R"({"data": {"label": "k", "is_management_key": 0}})"}) {
        const auto parsed = orcli::parse_key_status(body);
        REQUIRE_FALSE(parsed.has_value());
        check_api_error(parsed.error(), "/key");
    }
}

TEST_CASE("parse_key_status rejects wrong-typed identity fields") {
    for (const std::string_view body :
         {R"({"data": {"label": "k", "is_management_key": false, "creator_user_id": 7}})",
          R"({"data": {"label": "k", "is_management_key": false, "organization_id": []}})",
          R"({"data": {"label": "k", "is_management_key": false, "workspace_id": {}}})"}) {
        const auto parsed = orcli::parse_key_status(body);
        REQUIRE_FALSE(parsed.has_value());
        check_api_error(parsed.error(), "/key");
    }
}

TEST_CASE("parse_members_page reads members and null names") {
    const std::vector<orcli::MemberEntry> members = orcli::parse_members_page(members_json).value();
    REQUIRE(members.size() == 2);
    CHECK(members.front().id == "user_2dHF");
    CHECK(members.front().email == "jane.doe@example.com");
    CHECK(members.front().first_name == "Jane");
    CHECK(members.front().role == "org:admin");
    CHECK_FALSE(members.back().first_name.has_value());
    CHECK_FALSE(members.back().last_name.has_value());
}

TEST_CASE("parse_members_page returns an empty vector for the last page") {
    const auto page = orcli::parse_members_page(R"({"data": [], "total_count": 0})");
    REQUIRE(page.has_value());
    CHECK(page.value().empty());
}

TEST_CASE("parse_members_page rejects malformed pages and wrong types") {
    for (const std::string_view body :
         {"not json", R"({"total_count": 1})", R"({"data": {}})", R"({"data": ["user"]})",
          R"({"data": [{"id": 5, "email": "e", "role": "org:member"}]})",
          R"({"data": [{"id": "u", "email": "e", "first_name": 1, "role": "org:member"}]})",
          R"({"data": [{"id": "u", "email": "e", "role": null}]})"}) {
        const auto parsed = orcli::parse_members_page(body);
        REQUIRE_FALSE(parsed.has_value());
        check_api_error(parsed.error(), "/organization/members");
    }
}

TEST_CASE("parse_keys_page reads enabled, disabled and expired keys") {
    const std::vector<orcli::KeyEntry> keys = orcli::parse_keys_page(keys_json).value();
    REQUIRE(keys.size() == 2);
    CHECK(keys.front().name == "Production");
    CHECK_FALSE(keys.front().disabled);
    CHECK_FALSE(keys.front().expires_at.has_value());
    CHECK(keys.back().disabled);
    CHECK(keys.back().expires_at == "2025-01-01T00:00:00Z");
    CHECK_FALSE(keys.back().limit.has_value());
}

TEST_CASE("parse_keys_page returns an empty vector for the last page") {
    const auto page = orcli::parse_keys_page(R"({"data": []})");
    REQUIRE(page.has_value());
    CHECK(page.value().empty());
}

TEST_CASE("parse_keys_page rejects malformed pages") {
    for (const std::string_view body :
         {"{", R"({"data": {}})", R"({"data": [5]})",
          R"({"data": [{"hash": "h", "label": "l", "name": "n", "disabled": "no"}]})",
          R"({"data": [{"label": "l", "name": "n", "disabled": false}]})",
          R"({"data": [{"hash": "h", "label": "l", "name": "n", "disabled": false,
                        "usage": "3"}]})"}) {
        const auto parsed = orcli::parse_keys_page(body);
        REQUIRE_FALSE(parsed.has_value());
        check_api_error(parsed.error(), "/keys");
    }
}

TEST_CASE("format_keys states the scope and lists every key") {
    const std::string text = orcli::format_keys(orcli::parse_keys_page(keys_json).value());
    CHECK(text.starts_with("API keys in this management key's workspace (an "
                           "organization-scoped key lists its default workspace), disabled "
                           "and expired included:\n"));
    CHECK(text.contains("Old laptop"));
    CHECK(text.contains("disabled"));
    CHECK(text.contains("2025-01-01"));
    CHECK_FALSE(text.contains("f01d52606dc8f0a8"));
    CHECK_FALSE(text.contains(" \n"));
}

TEST_CASE("format_keys reports an empty workspace") {
    CHECK(orcli::format_keys({}).ends_with("No API keys.\n"));
}

TEST_CASE("parse_credits and format_credits show purchased, used and remaining") {
    const orcli::Credits credits = orcli::parse_credits(credits_json).value();
    CHECK(credits.total_credits == credits_total);
    CHECK(credits.total_usage == credits_used);
    const std::string text = orcli::format_credits(credits);
    CHECK(text.contains("Purchased:          $100.5000\n"));
    CHECK(text.contains("Used:               $25.7500\n"));
    CHECK(text.contains("Remaining:          $74.7500\n"));
}

TEST_CASE("parse_credits rejects missing, null and wrong-typed totals") {
    for (const std::string_view body : {"null", R"({"data": {"total_credits": 1}})",
                                        R"({"data": {"total_credits": null, "total_usage": 1}})",
                                        R"({"data": {"total_credits": "1", "total_usage": 1}})"}) {
        const auto parsed = orcli::parse_credits(body);
        REQUIRE_FALSE(parsed.has_value());
        check_api_error(parsed.error(), "/credits");
    }
}

TEST_CASE("parse_activity reads rows and keeps reasoning tokens separate") {
    const std::vector<ActivityRow> rows = orcli::parse_activity(activity_json).value();
    REQUIRE(rows.size() == 1);
    const ActivityRow& row = rows.front();
    CHECK(row.date == "2026-11-08");
    CHECK(row.provider == "OpenAI");
    CHECK(row.requests == activity_requests);
    CHECK(row.completion_tokens == activity_completion);
    CHECK(row.reasoning_tokens == activity_reasoning);
}

TEST_CASE("parse_activity accepts empty data and rejects malformed numbers") {
    CHECK(orcli::parse_activity(R"({"data": []})").value().empty());
    for (const std::string_view body :
         {"[]", R"({"data": [{"date": "2026-11-08", "model": "m", "provider_name": "p",
                    "requests": 1.5, "prompt_tokens": 1, "completion_tokens": 1,
                    "reasoning_tokens": 0, "usage": 0}]})",
          R"({"data": [{"date": "2026-11-08", "model": "m", "provider_name": "p",
                    "requests": 1, "prompt_tokens": "50", "completion_tokens": 1,
                    "reasoning_tokens": 0, "usage": 0}]})",
          R"({"data": [{"date": "2026-11-08", "model": "m", "provider_name": "p",
                    "requests": 1, "prompt_tokens": 1, "completion_tokens": 1,
                    "reasoning_tokens": 99999999999999999999, "usage": 0}]})",
          R"({"data": [{"date": "2026-11-08", "model": "m", "provider_name": "p",
                    "requests": 1, "prompt_tokens": 1, "completion_tokens": 1,
                    "usage": 0}]})"}) {
        const auto parsed = orcli::parse_activity(body);
        REQUIRE_FALSE(parsed.has_value());
        check_api_error(parsed.error(), "/activity");
    }
}

TEST_CASE("summarize_activity keeps completed days in the window only") {
    const std::vector<ActivityRow> summary =
        orcli::summarize_activity(window_rows(), window_days, today);
    std::vector<std::string> dates;
    for (const ActivityRow& row : summary) {
        dates.push_back(row.date);
    }
    const std::vector<std::string> expected{"2026-11-08", "2026-11-08", "2026-11-06"};
    CHECK(dates == expected);
}

TEST_CASE("summarize_activity sums rows sharing date, model and provider, costliest first") {
    constexpr long long summed_requests = 5;
    constexpr double summed_usage = 0.5;
    const std::vector<ActivityRow> summary =
        orcli::summarize_activity(window_rows(), window_days, today);
    REQUIRE(summary.size() == 3);
    CHECK(summary.front().provider == "Azure");
    const ActivityRow& openai = summary.at(1);
    CHECK(openai.provider == "OpenAI");
    CHECK(openai.requests == summed_requests);
    CHECK(openai.reasoning_tokens == summed_requests);
    CHECK(openai.usage == doctest::Approx(summed_usage));
}

TEST_CASE("summarize_activity with one day keeps only yesterday") {
    const std::vector<ActivityRow> summary = orcli::summarize_activity(window_rows(), 1, today);
    REQUIRE(summary.size() == 2);
    CHECK(summary.front().date == "2026-11-08");
    CHECK(summary.back().date == "2026-11-08");
}

TEST_CASE("format_activity states the range and totals the window") {
    const std::string text = orcli::format_activity(window_rows(), window_days, today);
    CHECK(text.starts_with("OpenRouter activity from 2026-11-06 to 2026-11-08 UTC (3 "
                           "completed days; today, 2026-11-09, is not included)\n"));
    CHECK(text.contains("REASONING"));
    CHECK(text.contains("Azure"));
    CHECK(text.ends_with("Total: 10 requests; 10 prompt, 10 completion and 10 reasoning "
                         "tokens; $1.5000\n"));
}

TEST_CASE("format_activity reports an empty window with a zero total") {
    const std::string text = orcli::format_activity({}, window_days, today);
    CHECK(text.contains("No activity in this range.\n"));
    CHECK(text.ends_with("Total: 0 requests; 0 prompt, 0 completion and 0 reasoning tokens; "
                         "$0.0000\n"));
}
