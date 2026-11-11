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
constexpr std::string_view date_pattern = "9999-99-99";
constexpr std::string_view timestamp_pattern = "9999-99-99 99:99:99";
constexpr std::size_t hour_offset = 11;
constexpr std::size_t minute_offset = 14;
constexpr std::size_t second_offset = 17;
constexpr int max_hour = 23;
constexpr int max_minute = 59;
constexpr int max_second = 59;
constexpr std::size_t column_gap = 2;
constexpr std::size_t label_width = 20;
constexpr std::size_t max_reason_chars = 400;

// Response text is never quoted: an error body can echo what was sent. `detail` names only
// fields, JSON types and row numbers.
std::unexpected<Error> malformed(std::string_view path, std::string_view detail) {
    return fail(Exit::api, std::format("unexpected response from GET {}: {}", path, detail));
}

// The response's `data` member, owned so callers can keep references into it; it must have the
// JSON type `type` (an object or an array). `path` names the request in the error.
Result<json> response_data(std::string_view json_text, json::value_t type, std::string_view path) {
    json document = json::parse(json_text, nullptr, false);
    if (!document.is_object()) {
        return malformed(path, "the reply is not a JSON object with a data member");
    }
    const auto found = document.find("data");
    if (found == document.end()) {
        return malformed(path, "the reply is not a JSON object with a data member");
    }
    if (found->type() != type) {
        return malformed(path,
                         std::format("data is {}, expected {}", found->type_name(),
                                     type == json::value_t::array ? "an array" : "an object"));
    }
    return std::move(*found);
}

// Reads typed fields from one JSON object. Absent and null fields are std::nullopt; a present
// field of the wrong type, or a missing required field, marks the object invalid so the caller
// rejects the whole response instead of showing a guessed value. The first such problem is kept
// for the error message.
class FieldReader {
public:
    explicit FieldReader(const json& object) : object_(&object) {}

    [[nodiscard]] bool valid() const { return !problem_.has_value(); }
    [[nodiscard]] std::string problem() const { return problem_.value_or(""); }

    std::optional<std::string> optional_text(std::string_view key) {
        const json* value = lookup(key);
        if (value == nullptr) {
            return std::nullopt;
        }
        if (!value->is_string()) {
            wrong_type(key, *value, "a string");
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
            wrong_type(key, *value, "a number");
            return std::nullopt;
        }
        return value->get<double>();
    }

    std::optional<long long> optional_count(std::string_view key) {
        const json* value = lookup(key);
        if (value == nullptr) {
            return std::nullopt;
        }
        if (value->is_number_unsigned()) {
            if (std::in_range<long long>(value->get<std::uint64_t>())) {
                return static_cast<long long>(value->get<std::uint64_t>());
            }
            note(std::format("field {} is out of range", key));
            return std::nullopt;
        }
        if (value->is_number_integer()) {
            return value->get<long long>();
        }
        wrong_type(key, *value, "a whole number");
        return std::nullopt;
    }

    std::optional<bool> optional_flag(std::string_view key) {
        const json* value = lookup(key);
        if (value == nullptr) {
            return std::nullopt;
        }
        if (!value->is_boolean()) {
            wrong_type(key, *value, "a boolean");
            return std::nullopt;
        }
        return value->get<bool>();
    }

    std::string text(std::string_view key) { return required(key, optional_text(key)); }
    double number(std::string_view key) { return required(key, optional_number(key)); }
    long long count(std::string_view key) { return required(key, optional_count(key)); }
    bool flag(std::string_view key) { return required(key, optional_flag(key)); }

private:
    [[nodiscard]] const json* lookup(std::string_view key) const {
        const auto found = object_->find(key);
        if (found == object_->end() || found->is_null()) {
            return nullptr;
        }
        return &*found;
    }

    // A wrong type found by the optional_ reader is kept over this later "missing".
    template <class T> T required(std::string_view key, std::optional<T> value) {
        if (!value.has_value()) {
            note(std::format("field {} is null or missing", key));
            return T{};
        }
        return std::move(*value);
    }

    void wrong_type(std::string_view key, const json& value, std::string_view expected) {
        note(std::format("field {} is {}, expected {}", key, value.type_name(), expected));
    }

    void note(std::string problem) {
        if (!problem_.has_value()) {
            problem_ = std::move(problem);
        }
    }

    const json* object_;
    std::optional<std::string> problem_;
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
// Dates.
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

// '9' stands for one ASCII digit; every other character must match exactly. std::chrono::parse
// alone cannot enforce the shape: its field widths are maxima ("2026-11-08 0:0:0" parses) and a
// space in the format matches any run of whitespace, including none.
bool matches_pattern(std::string_view text, std::string_view pattern) {
    return text.size() == pattern.size() &&
           std::ranges::equal(text, pattern, [](char got, char want) {
               return want == '9' ? (got >= '0' && got <= '9') : got == want;
           });
}

// The two digits at `first` read as a number no greater than `max`.
bool two_digits_within(std::string_view text, std::size_t first, int max) {
    const std::string_view digits = text.substr(first, 2);
    const char* const digits_end = std::to_address(digits.end());
    int value = 0;
    const auto parsed = std::from_chars(std::to_address(digits.begin()), digits_end, value);
    return parsed.ec == std::errc{} && parsed.ptr == digits_end && value <= max;
}

// The UTC day of an activity row. OpenRouter's API reference gives `date` as YYYY-MM-DD, but the
// live API (seen 2026-11-11) sends YYYY-MM-DD HH:MM:SS; both are accepted. Rows are totals per
// UTC day, so the day is the date part whatever the time. The time is checked by hand because
// libstdc++ 16's %T accepts minute 60 and carries 23:60:00 into the next day.
std::optional<std::string> activity_day(std::string_view text) {
    if (matches_pattern(text, timestamp_pattern)) {
        if (!two_digits_within(text, hour_offset, max_hour) ||
            !two_digits_within(text, minute_offset, max_minute) ||
            !two_digits_within(text, second_offset, max_second)) {
            return std::nullopt;
        }
        text = text.substr(0, date_length);
    }
    if (!matches_pattern(text, date_pattern) || !parse_date(text).has_value()) {
        return std::nullopt;
    }
    return std::string(text);
}

// ---------------------------------------------------------------------------------------------
// Parsing of list rows. Each parser gets a JSON object and returns the row or the problem, which
// names a field but never quotes a value.
// ---------------------------------------------------------------------------------------------

template <class Row>
Result<std::vector<Row>> parse_rows(std::string_view json_text, std::string_view path,
                                    std::expected<Row, std::string> (*parse_row)(const json&)) {
    const auto data = response_data(json_text, json::value_t::array, path);
    if (!data) {
        return std::unexpected(data.error());
    }
    std::vector<Row> rows;
    rows.reserve(data->size());
    std::size_t number = 0;
    for (const json& item : *data) {
        ++number;
        if (!item.is_object()) {
            return malformed(path, std::format("row {} is not an object", number));
        }
        auto row = parse_row(item);
        if (!row) {
            return malformed(path, std::format("row {}: {}", number, row.error()));
        }
        rows.push_back(std::move(*row));
    }
    return rows;
}

std::expected<KeyEntry, std::string> parse_key_entry(const json& item) {
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
        return std::unexpected(fields.problem());
    }
    return key;
}

std::expected<MemberEntry, std::string> parse_member(const json& item) {
    FieldReader fields(item);
    MemberEntry member{
        .id = fields.text("id"),
        .email = fields.text("email"),
        .first_name = fields.optional_text("first_name"),
        .last_name = fields.optional_text("last_name"),
        .role = fields.text("role"),
    };
    if (!fields.valid()) {
        return std::unexpected(fields.problem());
    }
    return member;
}

std::expected<ActivityRow, std::string> parse_activity_row(const json& item) {
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
        return std::unexpected(fields.problem());
    }
    std::optional<std::string> day = activity_day(row.date);
    if (!day.has_value()) {
        return std::unexpected<std::string>("field date is not a YYYY-MM-DD date");
    }
    row.date = std::move(*day);
    return row;
}

// ---------------------------------------------------------------------------------------------
// Activity window.
// ---------------------------------------------------------------------------------------------

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
    const auto data = response_data(json_text, json::value_t::object, key_path);
    if (!data) {
        return std::unexpected(data.error());
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
        return malformed(key_path, fields.problem());
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
    return parse_rows(json_text, keys_path, parse_key_entry);
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
    return parse_rows(json_text, members_path, parse_member);
}

Result<Credits> parse_credits(std::string_view json_text) {
    const auto data = response_data(json_text, json::value_t::object, credits_path);
    if (!data) {
        return std::unexpected(data.error());
    }
    FieldReader fields(*data);
    const Credits credits{
        .total_credits = fields.number("total_credits"),
        .total_usage = fields.number("total_usage"),
    };
    if (!fields.valid()) {
        return malformed(credits_path, fields.problem());
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
    return parse_rows(json_text, activity_path, parse_activity_row);
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

std::optional<std::string> api_error_message(std::string_view body) {
    const json document = json::parse(body, nullptr, false);
    if (!document.is_object()) {
        return std::nullopt;
    }
    const auto error = document.find("error");
    if (error == document.end() || !error->is_object()) {
        return std::nullopt;
    }
    const auto message = error->find("message");
    if (message == error->end() || !message->is_string()) {
        return std::nullopt;
    }
    std::string text = message->get<std::string>();
    if (text.empty()) {
        return std::nullopt;
    }
    return text;
}

std::optional<std::string> provider_reason(std::string_view body) {
    const std::optional<std::string> message = api_error_message(body);
    if (!message.has_value()) {
        return std::nullopt;
    }
    return sanitize_for_display(display_metadata(*message), max_reason_chars);
}

} // namespace orcli
