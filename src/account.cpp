#include "account.hpp"

#include "common.hpp"
#include "display.hpp"

#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace orcli {
namespace {

using nlohmann::json;

constexpr std::string_view key_path = "/key";
constexpr std::string_view keys_path = "/keys";
constexpr std::string_view credits_path = "/credits";
constexpr std::string_view activity_path = "/activity";
constexpr std::string_view members_path = "/organization/members";

constexpr std::string_view not_available = "n/a";
constexpr int min_activity_days = 1;
constexpr int max_activity_days = 30;
constexpr int money_decimals = 4;
constexpr int max_money_decimals = 8;
constexpr double smallest_four_decimal_amount = 0.0001;
// Holds any finite double in fixed notation (at most 309 integer digits) with sign, point and
// max_money_decimals decimals.
constexpr std::size_t money_buffer_bytes = 384;
constexpr std::size_t digits_per_group = 3;
constexpr std::size_t date_length = 10; // YYYY-MM-DD
constexpr std::size_t column_gap = 2;
constexpr std::size_t label_width = 20;

// Response text is never quoted: an error body can echo what was sent.
std::unexpected<Error> malformed(std::string_view path) {
    return fail(Exit::api, std::format("unexpected response from GET {}", path));
}

// The response's `data` member, owned so callers can keep references into it.
std::optional<json> response_data(std::string_view json_text) {
    json document = json::parse(json_text, nullptr, false);
    if (!document.is_object()) {
        return std::nullopt;
    }
    const auto found = document.find("data");
    if (found == document.end()) {
        return std::nullopt;
    }
    return std::move(*found);
}

// Reads typed fields from one JSON object. Absent and null fields are std::nullopt; a present
// field of the wrong type, or a missing required field, marks the object invalid so the caller
// rejects the whole response instead of showing a guessed value.
class FieldReader {
public:
    explicit FieldReader(const json& object) : object_(&object) {}

    [[nodiscard]] bool valid() const { return valid_; }

    std::optional<std::string> optional_text(std::string_view key) {
        const json* value = lookup(key);
        if (value == nullptr) {
            return std::nullopt;
        }
        if (!value->is_string()) {
            valid_ = false;
            return std::nullopt;
        }
        return value->get<std::string>();
    }

    std::optional<double> optional_number(std::string_view key) {
        const json* value = lookup(key);
        if (value == nullptr) {
            return std::nullopt;
        }
        if (!value->is_number()) {
            valid_ = false;
            return std::nullopt;
        }
        return value->get<double>();
    }

    std::optional<long long> optional_count(std::string_view key) {
        const json* value = lookup(key);
        if (value == nullptr) {
            return std::nullopt;
        }
        if (value->is_number_unsigned() && std::in_range<long long>(value->get<std::uint64_t>())) {
            return static_cast<long long>(value->get<std::uint64_t>());
        }
        if (value->is_number_integer() && !value->is_number_unsigned()) {
            return value->get<long long>();
        }
        valid_ = false;
        return std::nullopt;
    }

    std::optional<bool> optional_flag(std::string_view key) {
        const json* value = lookup(key);
        if (value == nullptr) {
            return std::nullopt;
        }
        if (!value->is_boolean()) {
            valid_ = false;
            return std::nullopt;
        }
        return value->get<bool>();
    }

    std::string text(std::string_view key) { return required(optional_text(key)); }
    double number(std::string_view key) { return required(optional_number(key)); }
    long long count(std::string_view key) { return required(optional_count(key)); }
    bool flag(std::string_view key) { return required(optional_flag(key)); }

private:
    [[nodiscard]] const json* lookup(std::string_view key) const {
        const auto found = object_->find(key);
        if (found == object_->end() || found->is_null()) {
            return nullptr;
        }
        return &*found;
    }

    template <class T> T required(std::optional<T> value) {
        if (!value.has_value()) {
            valid_ = false;
            return T{};
        }
        return std::move(*value);
    }

    const json* object_;
    bool valid_ = true;
};

// ---------------------------------------------------------------------------------------------
// Display helpers.
// ---------------------------------------------------------------------------------------------

std::string with_commas(long long value) {
    const std::string digits = std::to_string(value);
    const std::size_t sign = digits.starts_with('-') ? std::size_t{1} : std::size_t{0};
    std::string grouped;
    for (std::size_t index = 0; index < digits.size(); ++index) {
        if (index > sign && (digits.size() - index) % digits_per_group == 0) {
            grouped.push_back(',');
        }
        grouped.push_back(digits.at(index));
    }
    return grouped;
}

// std::format with a nested precision ("{:.{}f}") is rejected by Clang with libstdc++ 15
// (llvm/llvm-project#151371, a Clang bug), so the digits come from std::to_chars, which
// formats like printf's %.*f in the "C" locale.
std::string dollars(double amount, int decimals) {
    std::array<char, money_buffer_bytes> digits{};
    const auto [end, error] = std::to_chars(digits.data(), std::to_address(digits.end()), amount,
                                            std::chars_format::fixed, decimals);
    if (error != std::errc{}) {
        return std::format("${}", amount);
    }
    return "$" + std::string(digits.data(), end);
}

// Sub-cent amounts are common with cheap models, so a tiny charge must not print as $0.0000.
std::string unsigned_money(double amount) {
    if (amount <= 0.0 || amount >= smallest_four_decimal_amount) {
        return dollars(amount, money_decimals);
    }
    std::string text;
    for (int decimals = money_decimals + 1; decimals <= max_money_decimals; ++decimals) {
        text = dollars(amount, decimals);
        if (text.find_first_of("123456789") != std::string::npos) {
            break;
        }
    }
    if (text.find_first_of("123456789") == std::string::npos) {
        return std::format("${:g}", amount);
    }
    return text;
}

std::string money_text(const std::optional<double>& amount) {
    return amount.has_value() ? format_usd(*amount) : std::string(not_available);
}

std::string text_or_na(const std::optional<std::string>& text) {
    return text.value_or(std::string(not_available));
}

std::string date_part(const std::optional<std::string>& stamp) {
    if (!stamp.has_value()) {
        return std::string(not_available);
    }
    return stamp->substr(0, date_length);
}

using Table = std::vector<std::vector<std::string>>;

// Left-aligned columns sized to their widest cell; the first row is the header.
std::string render_table(const Table& rows) {
    std::vector<std::size_t> widths;
    for (const auto& row : rows) {
        widths.resize(std::max(widths.size(), row.size()));
        for (auto&& [cell, width] : std::views::zip(row, widths)) {
            width = std::max(width, cell.size());
        }
    }
    std::string table;
    for (const auto& row : rows) {
        std::string line;
        for (const auto& [cell, width] : std::views::zip(row, widths)) {
            // Padding by hand: a nested width ("{:<{}}") hits the same Clang bug as dollars().
            const std::size_t start = line.size();
            line += display_metadata(cell);
            line.resize(start + std::max(cell.size(), width + column_gap), ' ');
        }
        while (line.ends_with(' ')) {
            line.pop_back();
        }
        table += line;
        table.push_back('\n');
    }
    return table;
}

struct Field {
    std::string_view label;
    std::string value;
};

std::string render_fields(const std::vector<Field>& fields) {
    std::string text;
    for (const Field& field : fields) {
        std::string label = std::format("{}:", field.label);
        label.resize(std::max(label.size(), label_width), ' ');
        text += label;
        text += display_metadata(field.value);
        text += '\n';
    }
    return text;
}

// ---------------------------------------------------------------------------------------------
// Parsing of list rows.
// ---------------------------------------------------------------------------------------------

std::optional<KeyEntry> parse_key_entry(const json& item) {
    if (!item.is_object()) {
        return std::nullopt;
    }
    FieldReader fields(item);
    KeyEntry key;
    key.hash = fields.text("hash");
    key.label = fields.text("label");
    key.name = fields.text("name");
    key.disabled = fields.flag("disabled");
    key.created_at = fields.optional_text("created_at");
    key.expires_at = fields.optional_text("expires_at");
    key.last_used_at = fields.optional_text("last_used_at");
    key.usage = fields.optional_number("usage");
    key.usage_daily = fields.optional_number("usage_daily");
    key.usage_weekly = fields.optional_number("usage_weekly");
    key.usage_monthly = fields.optional_number("usage_monthly");
    key.limit = fields.optional_number("limit");
    key.limit_remaining = fields.optional_number("limit_remaining");
    key.limit_reset = fields.optional_text("limit_reset");
    if (!fields.valid()) {
        return std::nullopt;
    }
    return key;
}

std::optional<MemberEntry> parse_member(const json& item) {
    if (!item.is_object()) {
        return std::nullopt;
    }
    FieldReader fields(item);
    MemberEntry member{
        .id = fields.text("id"),
        .email = fields.text("email"),
        .first_name = fields.optional_text("first_name"),
        .last_name = fields.optional_text("last_name"),
        .role = fields.text("role"),
    };
    if (!fields.valid()) {
        return std::nullopt;
    }
    return member;
}

std::optional<ActivityRow> parse_activity_row(const json& item) {
    if (!item.is_object()) {
        return std::nullopt;
    }
    FieldReader fields(item);
    ActivityRow row{
        .date = fields.text("date"),
        .model = fields.text("model"),
        .provider = fields.text("provider_name"),
        .requests = fields.count("requests"),
        .prompt_tokens = fields.count("prompt_tokens"),
        .completion_tokens = fields.count("completion_tokens"),
        .reasoning_tokens = fields.count("reasoning_tokens"),
        .usage = fields.number("usage"),
    };
    if (!fields.valid()) {
        return std::nullopt;
    }
    return row;
}

// ---------------------------------------------------------------------------------------------
// Activity window.
// ---------------------------------------------------------------------------------------------

// Rejects trailing text so "2026-11-08junk" is not read as a date.
std::optional<std::chrono::sys_days> parse_date(std::string_view text) {
    std::istringstream input{std::string(text)};
    std::chrono::sys_days day{};
    input >> std::chrono::parse("%F", day);
    if (input.fail() || input.peek() != std::istringstream::traits_type::eof()) {
        return std::nullopt;
    }
    return day;
}

// The CLI validates --days; clamping keeps a bad value from widening or emptying the window
// silently in other callers.
int clamp_days(int days) {
    return std::clamp(days, min_activity_days, max_activity_days);
}

void add_into(ActivityRow& total, const ActivityRow& row) {
    total.requests = orcli::saturating_add(total.requests, row.requests);
    total.prompt_tokens = orcli::saturating_add(total.prompt_tokens, row.prompt_tokens);
    total.completion_tokens = orcli::saturating_add(total.completion_tokens, row.completion_tokens);
    total.reasoning_tokens = orcli::saturating_add(total.reasoning_tokens, row.reasoning_tokens);
    total.usage += row.usage;
}

bool newest_then_costliest(const ActivityRow& left, const ActivityRow& right) {
    if (left.date != right.date) {
        return left.date > right.date;
    }
    if (left.usage != right.usage) {
        return left.usage > right.usage;
    }
    return std::tie(left.model, left.provider) < std::tie(right.model, right.provider);
}

std::vector<std::string> activity_cells(const ActivityRow& row) {
    return {
        row.date,
        row.model,
        row.provider,
        with_commas(row.requests),
        with_commas(row.prompt_tokens),
        with_commas(row.completion_tokens),
        with_commas(row.reasoning_tokens),
        format_usd(row.usage),
    };
}

std::string activity_range(int days, std::string_view today_utc) {
    const std::optional<std::chrono::sys_days> today = parse_date(today_utc);
    if (!today.has_value()) {
        return std::format("OpenRouter activity for the {} completed UTC days before {}", days,
                           today_utc);
    }
    const std::chrono::sys_days first = *today - std::chrono::days{days};
    const std::chrono::sys_days last = *today - std::chrono::days{1};
    return std::format("OpenRouter activity from {:%F} to {:%F} UTC ({} completed days; today, "
                       "{:%F}, is not included)",
                       first, last, days, *today);
}

} // namespace

std::string format_usd(double amount) {
    return amount < 0.0 ? "-" + unsigned_money(-amount) : unsigned_money(amount);
}

Result<KeyStatus> parse_key_status(std::string_view json_text) {
    const std::optional<json> data = response_data(json_text);
    if (!data.has_value() || !data->is_object()) {
        return malformed(key_path);
    }
    FieldReader fields(*data);
    KeyStatus status;
    status.label = fields.text("label");
    status.usage = fields.optional_number("usage");
    status.usage_daily = fields.optional_number("usage_daily");
    status.usage_weekly = fields.optional_number("usage_weekly");
    status.usage_monthly = fields.optional_number("usage_monthly");
    status.limit = fields.optional_number("limit");
    status.limit_remaining = fields.optional_number("limit_remaining");
    status.limit_reset = fields.optional_text("limit_reset");
    status.expires_at = fields.optional_text("expires_at");
    status.is_free_tier = fields.optional_flag("is_free_tier").value_or(false);
    status.creator_user_id = fields.optional_text("creator_user_id");
    status.organization_id = fields.optional_text("organization_id");
    status.workspace_id = fields.optional_text("workspace_id");
    status.is_management_key = fields.flag("is_management_key");
    if (!fields.valid()) {
        return malformed(key_path);
    }
    return status;
}

std::string format_key_status(const KeyStatus& status) {
    const std::vector<Field> fields{
        {.label = "Key", .value = status.label},
        {.label = "Free tier", .value = status.is_free_tier ? "yes" : "no"},
        {.label = "Used today (UTC)", .value = money_text(status.usage_daily)},
        {.label = "Used this week", .value = money_text(status.usage_weekly)},
        {.label = "Used this month", .value = money_text(status.usage_monthly)},
        {.label = "Used in total", .value = money_text(status.usage)},
        {.label = "Limit", .value = money_text(status.limit)},
        {.label = "Limit remaining", .value = money_text(status.limit_remaining)},
        {.label = "Limit reset", .value = text_or_na(status.limit_reset)},
        {.label = "Expires", .value = text_or_na(status.expires_at)},
    };
    return render_fields(fields) +
           "Weeks run Monday to Sunday and months by calendar month, both in UTC.\n";
}

Result<std::vector<KeyEntry>> parse_keys_page(std::string_view json_text) {
    const std::optional<json> data = response_data(json_text);
    if (!data.has_value() || !data->is_array()) {
        return malformed(keys_path);
    }
    std::vector<KeyEntry> keys;
    keys.reserve(data->size());
    for (const json& item : *data) {
        auto key = parse_key_entry(item);
        if (!key.has_value()) {
            return malformed(keys_path);
        }
        keys.push_back(std::move(*key));
    }
    return keys;
}

std::string format_keys(const std::vector<KeyEntry>& keys) {
    const std::string text =
        "API keys in this management key's workspace (an organization-scoped key "
        "lists its default workspace), disabled and expired included:\n";
    if (keys.empty()) {
        return text + "No API keys.\n";
    }
    Table rows{
        {
            "NAME",
            "LABEL",
            "STATUS",
            "CREATED",
            "EXPIRES",
            "TODAY",
            "WEEK",
            "MONTH",
            "TOTAL",
            "LIMIT",
            "REMAINING",
        },
    };
    for (const KeyEntry& key : keys) {
        rows.push_back({
            key.name,
            key.label,
            key.disabled ? "disabled" : "enabled",
            date_part(key.created_at),
            date_part(key.expires_at),
            money_text(key.usage_daily),
            money_text(key.usage_weekly),
            money_text(key.usage_monthly),
            money_text(key.usage),
            money_text(key.limit),
            money_text(key.limit_remaining),
        });
    }
    return text + render_table(rows);
}

Result<std::vector<MemberEntry>> parse_members_page(std::string_view json_text) {
    const std::optional<json> data = response_data(json_text);
    if (!data.has_value() || !data->is_array()) {
        return malformed(members_path);
    }
    std::vector<MemberEntry> members;
    members.reserve(data->size());
    for (const json& item : *data) {
        auto member = parse_member(item);
        if (!member.has_value()) {
            return malformed(members_path);
        }
        members.push_back(std::move(*member));
    }
    return members;
}

Result<Credits> parse_credits(std::string_view json_text) {
    const std::optional<json> data = response_data(json_text);
    if (!data.has_value() || !data->is_object()) {
        return malformed(credits_path);
    }
    FieldReader fields(*data);
    const Credits credits{
        .total_credits = fields.number("total_credits"),
        .total_usage = fields.number("total_usage"),
    };
    if (!fields.valid()) {
        return malformed(credits_path);
    }
    return credits;
}

std::string format_credits(const Credits& credits) {
    const std::vector<Field> fields{
        {.label = "Purchased", .value = format_usd(credits.total_credits)},
        {.label = "Used", .value = format_usd(credits.total_usage)},
        {.label = "Remaining", .value = format_usd(credits.total_credits - credits.total_usage)},
    };
    return render_fields(fields);
}

Result<std::vector<ActivityRow>> parse_activity(std::string_view json_text) {
    const std::optional<json> data = response_data(json_text);
    if (!data.has_value() || !data->is_array()) {
        return malformed(activity_path);
    }
    std::vector<ActivityRow> rows;
    rows.reserve(data->size());
    for (const json& item : *data) {
        auto row = parse_activity_row(item);
        if (!row.has_value() || row->date.size() != date_length ||
            !parse_date(row->date).has_value()) {
            return malformed(activity_path);
        }
        rows.push_back(std::move(*row));
    }
    return rows;
}

std::vector<ActivityRow> summarize_activity(const std::vector<ActivityRow>& rows, int days,
                                            std::string_view today_utc) {
    const std::optional<std::chrono::sys_days> today = parse_date(today_utc);
    if (!today.has_value()) {
        return {};
    }
    const std::chrono::sys_days first = *today - std::chrono::days{clamp_days(days)};
    // Activity is reported per endpoint, so one model and provider can have several rows a day.
    std::map<std::tuple<std::string, std::string, std::string>, ActivityRow> merged;
    for (const ActivityRow& row : rows) {
        const std::optional<std::chrono::sys_days> day = parse_date(row.date);
        if (!day.has_value() || *day < first || *day >= *today) {
            continue;
        }
        const auto [slot, inserted] =
            merged.try_emplace(std::tuple{row.date, row.model, row.provider}, row);
        if (!inserted) {
            add_into(slot->second, row);
        }
    }
    auto summary = merged | std::views::values | std::ranges::to<std::vector>();
    std::ranges::sort(summary, newest_then_costliest);
    return summary;
}

std::string format_activity(const std::vector<ActivityRow>& rows, int days,
                            std::string_view today_utc) {
    const int shown_days = clamp_days(days);
    const std::vector<ActivityRow> summary = summarize_activity(rows, shown_days, today_utc);
    std::string text = activity_range(shown_days, today_utc) + "\n";
    ActivityRow total;
    if (summary.empty()) {
        text += "No activity in this range.\n";
    } else {
        Table table{
            {"DATE", "MODEL", "PROVIDER", "REQUESTS", "PROMPT", "COMPLETION", "REASONING", "COST"},
        };
        for (const ActivityRow& row : summary) {
            table.push_back(activity_cells(row));
            add_into(total, row);
        }
        text += render_table(table);
    }
    text += std::format("Total: {} requests; {} prompt, {} completion and {} reasoning tokens; "
                        "{}\n",
                        with_commas(total.requests), with_commas(total.prompt_tokens),
                        with_commas(total.completion_tokens), with_commas(total.reasoning_tokens),
                        format_usd(total.usage));
    return text;
}

} // namespace orcli
