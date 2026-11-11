#include "commands.hpp"

#include "account.hpp"
#include "common.hpp"
#include "config.hpp"
#include "costlog.hpp"
#include "credentials.hpp"
#include "display.hpp"
#include "http.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orcli {
namespace {

constexpr long http_unauthorized = 401;
constexpr long http_forbidden = 403;
constexpr long http_not_found = 404;
// Both paged lists stop here so a server that never returns an empty page cannot loop forever.
constexpr int max_pages = 50;
constexpr int members_page_size = 100;
constexpr std::size_t date_length = 10; // YYYY-MM-DD

constexpr std::string_view login_replace_hint = "openrouter-cli login --replace";
constexpr std::string_view management_replace_hint =
    "openrouter-cli auth set --management --replace";
constexpr std::string_view seen_by_organization = " (as seen by the Management key's organization)";
constexpr std::string_view not_available = "n/a";

Http inference_http(const Config& config, SecretString key) {
    return {config, HttpKeys{.inference = std::move(key), .management = SecretString{}}};
}

Http management_http(const Config& config, SecretString key) {
    return {config, HttpKeys{.inference = SecretString{}, .management = std::move(key)}};
}

GetOptions with_auth(Auth auth) {
    return GetOptions{.auth = auth, .retry_404_for = std::chrono::seconds{0}};
}

// GET for the account commands: the 2xx body, or Exit::api with the fix for a rejected key.
Result<std::string> fetch_body(const Http& http, std::string_view path, Auth auth) {
    const auto response = http.fetch(path, with_auth(auth));
    if (!response) {
        return std::unexpected(response.error());
    }
    if (response->status == http_unauthorized) {
        const bool management = auth == Auth::management;
        return fail(Exit::api,
                    std::format("OpenRouter rejected the stored {} (HTTP 401); run `{}`",
                                management ? "Management key" : "API key",
                                management ? management_replace_hint : login_replace_hint));
    }
    if (!http_success(response->status)) {
        return fail(Exit::api, std::format("GET {} failed with HTTP {}", path, response->status));
    }
    return response->body;
}

// Every page of GET /keys. Nothing is returned unless the listing ended with an empty page, so
// a partial list is never printed as if it were complete.
Result<std::vector<KeyEntry>> fetch_all_keys(const Http& http) {
    std::vector<KeyEntry> keys;
    for (int page = 0; page < max_pages; ++page) {
        const std::string path =
            std::format("/keys?include_disabled=true&include_expired=true&offset={}", keys.size());
        const auto rows = fetch_body(http, path, Auth::management).and_then(parse_keys_page);
        if (!rows) {
            return std::unexpected(rows.error());
        }
        if (rows->empty()) {
            return keys;
        }
        keys.append_range(*rows);
    }
    return fail(Exit::api, std::format("GET /keys did not finish within {} pages; no keys printed",
                                       max_pages));
}

// ----- status ------------------------------------------------------------------------------

std::string text_or(const std::optional<std::string>& text, std::string_view fallback) {
    return text.has_value() ? display_metadata(*text) : std::string(fallback);
}

std::string usd_or(const std::optional<double>& amount, std::string_view fallback) {
    return amount.has_value() ? format_usd(*amount) : std::string(fallback);
}

void print_key_lines(const KeyStatus& status) {
    std::println("logged in: yes");
    std::println("key creator: {}", text_or(status.creator_user_id, not_available));
    if (status.organization_id.has_value()) {
        std::println("owner: organization {}", display_metadata(*status.organization_id));
    } else {
        std::println("owner: individual account");
    }
    std::println("workspace: {}", text_or(status.workspace_id, not_available));
    std::println("key label: {}", display_metadata(status.label));
    std::println("key expires: {}", text_or(status.expires_at, "never"));
    std::println("free tier: {}", status.is_free_tier ? "yes" : "no");
    std::println("usage today: {}", usd_or(status.usage_daily, not_available));
    std::println("usage this week: {}", usd_or(status.usage_weekly, not_available));
    std::println("usage this month: {}", usd_or(status.usage_monthly, not_available));
    std::println("usage total: {}", usd_or(status.usage, not_available));
    // No limit means nothing remains to count down, so all three lines say "none".
    const bool limited = status.limit.has_value();
    std::println("limit: {}", usd_or(status.limit, "none"));
    std::println("limit remaining: {}",
                 limited ? usd_or(status.limit_remaining, not_available) : std::string("none"));
    std::println("limit reset: {}", text_or(status.limit_reset, "none"));
}

// An interrupted request ends `status` at once: no further lines, exit 8.
bool interrupted(const Error& error) {
    return error.code == Exit::interrupted;
}

struct LoginOutcome {
    int code = 0;
    // Set only when GET /key answered; the members lookup needs its creator id.
    std::optional<KeyStatus> status;
};

LoginOutcome show_login(const Config& config) {
    LoginOutcome outcome;
    const auto present = key_stored(config.state_dir, KeyKind::inference);
    if (!present) {
        outcome.code = report(present.error());
        return outcome;
    }
    if (!*present) {
        std::println("logged in: no (run `openrouter-cli login`)");
        return outcome;
    }
    auto key = load_stored_key(config, KeyKind::inference);
    if (!key) {
        outcome.code = report(key.error());
        return outcome;
    }
    const Http http = inference_http(config, std::move(*key));
    const auto response = http.fetch("/key", with_auth(Auth::inference));
    if (!response) {
        if (!interrupted(response.error())) {
            std::println("logged in: unknown (lookup failed: {})", response.error().message);
        }
        outcome.code = exit_code(response.error().code);
        return outcome;
    }
    if (response->status == http_unauthorized) {
        std::println("logged in: key rejected; run {}", login_replace_hint);
        outcome.code = exit_code(Exit::api);
        return outcome;
    }
    if (!http_success(response->status)) {
        std::println("logged in: unknown (lookup failed: HTTP {})", response->status);
        outcome.code = exit_code(Exit::api);
        return outcome;
    }
    auto status = parse_key_status(response->body);
    if (!status) {
        std::println("logged in: unknown (lookup failed: {})", status.error().message);
        outcome.code = exit_code(Exit::api);
        return outcome;
    }
    print_key_lines(*status);
    outcome.status = std::move(*status);
    return outcome;
}

std::string full_name(const MemberEntry& member) {
    const std::string first = member.first_name.value_or(std::string{});
    const std::string last = member.last_name.value_or(std::string{});
    if (first.empty() || last.empty()) {
        const std::string name = first + last;
        return name.empty() ? std::string(not_available) : name;
    }
    return std::format("{} {}", first, last);
}

void print_member(const MemberEntry& member) {
    std::println("email: {}{}", display_metadata(member.email), seen_by_organization);
    std::println("name: {}{}", display_metadata(full_name(member)), seen_by_organization);
    std::println("role: {}{}", display_metadata(member.role), seen_by_organization);
}

// Nullopt for any other non-2xx status or a body that does not parse.
std::optional<Credits> credits_from(const HttpResponse& response) {
    if (!http_success(response.status)) {
        return std::nullopt;
    }
    const auto parsed = parse_credits(response.body);
    if (!parsed) {
        return std::nullopt;
    }
    return *parsed;
}

std::optional<std::vector<MemberEntry>> members_from(const HttpResponse& response) {
    if (!http_success(response.status)) {
        return std::nullopt;
    }
    auto parsed = parse_members_page(response.body);
    if (!parsed) {
        return std::nullopt;
    }
    return std::move(*parsed);
}

int print_management_rejected() {
    std::println("management key: stored but rejected; run {}", management_replace_hint);
    return exit_code(Exit::api);
}

int print_email_lookup_failed() {
    std::println("email: lookup failed");
    return exit_code(Exit::api);
}

// Pages GET /organization/members for the key's creator. Only an explicit "no access" (403,
// 404) or a complete listing without a match is "not available"; anything that stopped the
// search early is "lookup failed", never reported as no match.
int show_creator(const Http& http, const std::string& creator_id) {
    std::size_t offset = 0;
    for (int page = 0; page < max_pages; ++page) {
        const auto response = http.fetch(
            std::format("/organization/members?limit={}&offset={}", members_page_size, offset),
            with_auth(Auth::management));
        if (!response) {
            if (interrupted(response.error())) {
                return exit_code(Exit::interrupted);
            }
            return std::max(print_email_lookup_failed(), exit_code(response.error().code));
        }
        if (response->status == http_unauthorized) {
            return print_management_rejected();
        }
        if (response->status == http_forbidden || response->status == http_not_found) {
            std::println("email: not available");
            return exit_code(Exit::ok);
        }
        const std::optional<std::vector<MemberEntry>> members = members_from(*response);
        if (!members.has_value()) {
            return print_email_lookup_failed();
        }
        if (members->empty()) {
            std::println("email: not available");
            return exit_code(Exit::ok);
        }
        const auto match = std::ranges::find(*members, creator_id, &MemberEntry::id);
        if (match != members->end()) {
            print_member(*match);
            return exit_code(Exit::ok);
        }
        offset += members->size();
    }
    return print_email_lookup_failed();
}

int show_management(const Config& config, const std::optional<KeyStatus>& login) {
    const auto present = key_stored(config.state_dir, KeyKind::management);
    if (!present) {
        return report(present.error());
    }
    if (!*present) {
        std::println("management key: not stored (add one with openrouter-cli auth set "
                     "--management to see credits, email and name)");
        return exit_code(Exit::ok);
    }
    auto key = load_stored_key(config, KeyKind::management);
    if (!key) {
        return report(key.error());
    }
    const Http http = management_http(config, std::move(*key));
    const auto response = http.fetch("/credits", with_auth(Auth::management));
    if (!response) {
        if (!interrupted(response.error())) {
            std::println("management key: stored (credits lookup failed)");
        }
        return exit_code(response.error().code);
    }
    if (response->status == http_unauthorized) {
        return print_management_rejected();
    }
    const std::optional<Credits> credits = credits_from(*response);
    if (!credits.has_value()) {
        std::println("management key: stored (credits lookup failed)");
        return exit_code(Exit::api);
    }
    std::println("management key: stored");
    std::println("credits remaining (Management key's account): {}",
                 format_usd(credits->total_credits - credits->total_usage));
    if (!login.has_value()) {
        return exit_code(Exit::ok);
    }
    if (!login->creator_user_id.has_value()) {
        std::println("email: not available (the key has no creator id)");
        return exit_code(Exit::ok);
    }
    return show_creator(http, *login->creator_user_id);
}

} // namespace

int cmd_status(const Config& config) {
    std::println("credentials: systemd-creds --user on {}", host_name());
    const LoginOutcome login = show_login(config);
    if (login.code == exit_code(Exit::interrupted) || interrupt_requested()) {
        return exit_code(Exit::interrupted);
    }
    const int management = show_management(config, login.status);
    // Exit codes are ordered by severity (config 7 over api 5 over ok 0), so the highest wins.
    return std::max(login.code, management);
}

int cmd_account_balance(const Config& config) {
    auto key = load_stored_key(config, KeyKind::inference);
    if (!key) {
        return report(key.error());
    }
    const Http http = inference_http(config, std::move(*key));
    const auto status = fetch_body(http, "/key", Auth::inference).and_then(parse_key_status);
    if (!status) {
        return report(status.error());
    }
    std::print("{}", format_key_status(*status));
    return exit_code(Exit::ok);
}

int cmd_account_keys(const Config& config) {
    auto key = load_stored_key(config, KeyKind::management);
    if (!key) {
        return report(key.error());
    }
    const Http http = management_http(config, std::move(*key));
    const auto keys = fetch_all_keys(http);
    if (!keys) {
        return report(keys.error());
    }
    std::print("{}", format_keys(*keys));
    return exit_code(Exit::ok);
}

int cmd_account_credits(const Config& config) {
    auto key = load_stored_key(config, KeyKind::management);
    if (!key) {
        return report(key.error());
    }
    const Http http = management_http(config, std::move(*key));
    const auto credits = fetch_body(http, "/credits", Auth::management).and_then(parse_credits);
    if (!credits) {
        return report(credits.error());
    }
    std::print("{}", format_credits(*credits));
    return exit_code(Exit::ok);
}

int cmd_account_activity(const Config& config, int days) {
    auto key = load_stored_key(config, KeyKind::management);
    if (!key) {
        return report(key.error());
    }
    const Http http = management_http(config, std::move(*key));
    const auto rows = fetch_body(http, "/activity", Auth::management).and_then(parse_activity);
    if (!rows) {
        return report(rows.error());
    }
    const std::string today = utc_now_iso().substr(0, date_length);
    std::print("{}", format_activity(*rows, days, today));
    return exit_code(Exit::ok);
}

} // namespace orcli
