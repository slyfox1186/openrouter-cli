#include "config.hpp"

#include "common.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <map>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef ORCLI_DEFAULT_HOME
#error "ORCLI_DEFAULT_HOME must be defined by the build (see target orcli_core in CMakeLists.txt)"
#endif

namespace orcli {
namespace {

using EnvMap = std::map<std::string, std::string>;

constexpr std::string_view blank_chars = " \t\r";
constexpr std::size_t quote_pair_length = 2;

constexpr auto plaintext_key_names =
    std::to_array<std::string_view>({"OPENROUTER_API_KEY", "OPENROUTER_MANAGEMENT_KEY"});
constexpr std::string_view https_scheme = "https://";
constexpr auto loopback_origins =
    std::to_array<std::string_view>({"http://127.0.0.1", "http://localhost"});

struct TextSetting {
    std::string_view key;
    std::string_view fallback;
};

template <class T> struct NumberSetting {
    std::string_view key;
    T fallback;
};

constexpr TextSetting base_url_setting{
    .key = "OPENROUTER_CLI_BASE_URL",
    .fallback = "https://openrouter.ai/api/v1",
};
constexpr TextSetting app_referer_setting{.key = "OPENROUTER_CLI_APP_REFERER", .fallback = ""};
constexpr TextSetting app_title_setting{
    .key = "OPENROUTER_CLI_APP_TITLE",
    .fallback = "openrouter-cli",
};
constexpr TextSetting app_categories_setting{
    .key = "OPENROUTER_CLI_APP_CATEGORIES",
    .fallback = "cli-agent",
};
constexpr TextSetting default_effort_setting{
    .key = "OPENROUTER_CLI_DEFAULT_EFFORT",
    .fallback = "high",
};
constexpr TextSetting privacy_setting{.key = "OPENROUTER_CLI_PRIVACY", .fallback = "zdr"};

constexpr NumberSetting<long long> max_output_tokens_setting{
    .key = "OPENROUTER_CLI_MAX_OUTPUT_TOKENS",
    .fallback = 64000,
};
constexpr NumberSetting<int> catalog_ttl_setting{
    .key = "OPENROUTER_CLI_CATALOG_TTL_HOURS",
    .fallback = 24,
};
constexpr NumberSetting<int> idle_timeout_setting{
    .key = "OPENROUTER_CLI_IDLE_TIMEOUT_SECONDS",
    .fallback = 300,
};
constexpr NumberSetting<int> total_timeout_setting{
    .key = "OPENROUTER_CLI_TOTAL_TIMEOUT_SECONDS",
    .fallback = 3600,
};
constexpr NumberSetting<std::size_t> max_file_bytes_setting{
    .key = "OPENROUTER_CLI_MAX_FILE_BYTES",
    .fallback = 1048576,
};
constexpr NumberSetting<std::size_t> max_input_bytes_setting{
    .key = "OPENROUTER_CLI_MAX_INPUT_BYTES",
    .fallback = 4194304,
};

std::string_view trim(std::string_view text) {
    const std::size_t first = text.find_first_not_of(blank_chars);
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(blank_chars);
    return text.substr(first, last - first + 1);
}

std::string_view unquote(std::string_view value) {
    const bool quoted = value.size() >= quote_pair_length && value.front() == value.back() &&
                        (value.front() == '"' || value.front() == '\'');
    return quoted ? value.substr(1, value.size() - quote_pair_length) : value;
}

bool is_key_start(char character) {
    return (character >= 'A' && character <= 'Z') || character == '_';
}

bool is_key_char(char character) {
    return is_key_start(character) || (character >= '0' && character <= '9');
}

bool is_valid_key(std::string_view key) {
    return !key.empty() && is_key_start(key.front()) && std::ranges::all_of(key, is_key_char);
}

struct EnvLine {
    enum class Kind : std::uint8_t { skip, assignment, missing_equals, bad_key };
    Kind kind = Kind::skip;
    std::string_view key;
    std::string_view value; // trimmed and unquoted
};

EnvLine non_assignment(EnvLine::Kind kind) {
    return EnvLine{.kind = kind, .key = {}, .value = {}};
}

EnvLine classify_line(std::string_view raw) {
    const std::string_view line = trim(raw);
    if (line.empty() || line.starts_with('#')) {
        return non_assignment(EnvLine::Kind::skip);
    }
    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos) {
        return non_assignment(EnvLine::Kind::missing_equals);
    }
    const std::string_view key = trim(line.substr(0, equals));
    if (!is_valid_key(key)) {
        return non_assignment(EnvLine::Kind::bad_key);
    }
    return EnvLine{
        .kind = EnvLine::Kind::assignment,
        .key = key,
        .value = unquote(trim(line.substr(equals + 1))),
    };
}

// Empty for a missing key, so callers treat missing and empty alike: a copied .env.example with a
// blank setting behaves like one without the line.
std::string_view lookup(const EnvMap& env, std::string_view key) {
    const auto found = env.find(std::string{key});
    return found == env.end() ? std::string_view{} : std::string_view{found->second};
}

std::string text_setting(const EnvMap& env, const TextSetting& setting) {
    const std::string_view value = lookup(env, setting.key);
    return std::string{value.empty() ? setting.fallback : value};
}

template <class T>
Result<void> read_positive(const EnvMap& env, const NumberSetting<T>& setting, T& target) {
    const std::string_view text = lookup(env, setting.key);
    if (text.empty()) {
        target = setting.fallback;
        return {};
    }
    const char* const text_end = std::to_address(text.end());
    T value{};
    const auto [parsed_end, status] =
        std::from_chars(std::to_address(text.begin()), text_end, value);
    if (status != std::errc{} || parsed_end != text_end || value < T{1}) {
        return fail(Exit::config, std::format("{} must be a positive whole number", setting.key));
    }
    target = value;
    return {};
}

Result<void> read_numbers(const EnvMap& env, Config& config) {
    return read_positive(env, max_output_tokens_setting, config.max_output_tokens)
        .and_then([&] { return read_positive(env, catalog_ttl_setting, config.catalog_ttl_hours); })
        .and_then(
            [&] { return read_positive(env, idle_timeout_setting, config.idle_timeout_seconds); })
        .and_then(
            [&] { return read_positive(env, total_timeout_setting, config.total_timeout_seconds); })
        .and_then([&] { return read_positive(env, max_file_bytes_setting, config.max_file_bytes); })
        .and_then(
            [&] { return read_positive(env, max_input_bytes_setting, config.max_input_bytes); });
}

// Plain http is allowed only to loopback, where the integration tests run their mock server.
bool is_allowed_base_url(std::string_view url) {
    if (url.starts_with(https_scheme)) {
        return url.size() > https_scheme.size();
    }
    return std::ranges::any_of(loopback_origins, [url](std::string_view origin) {
        if (!url.starts_with(origin)) {
            return false;
        }
        const std::string_view rest = url.substr(origin.size());
        return rest.empty() || rest.starts_with(':') || rest.starts_with('/');
    });
}

Result<std::string> parse_base_url(std::string url) {
    while (url.ends_with('/')) {
        url.pop_back();
    }
    if (!is_allowed_base_url(url)) {
        return fail(Exit::config, std::format("{} must start with https:// (or http://127.0.0.1 or "
                                              "http://localhost for local testing)",
                                              base_url_setting.key));
    }
    return url;
}

Result<Privacy> parse_privacy(std::string_view value) {
    if (value == "zdr") {
        return Privacy::zdr;
    }
    if (value == "no-training") {
        return Privacy::no_training;
    }
    return fail(Exit::config,
                std::format(R"({} must be "zdr" or "no-training")", privacy_setting.key));
}

Result<Config> config_from_env(const EnvMap& env) {
    Config config;
    Result<std::string> base_url = parse_base_url(text_setting(env, base_url_setting));
    if (!base_url.has_value()) {
        return std::unexpected(std::move(base_url.error()));
    }
    config.base_url = std::move(*base_url);
    const Result<Privacy> privacy = parse_privacy(text_setting(env, privacy_setting));
    if (!privacy.has_value()) {
        return std::unexpected(privacy.error());
    }
    config.privacy = *privacy;
    config.app_referer = text_setting(env, app_referer_setting);
    config.app_title = text_setting(env, app_title_setting);
    config.app_categories = text_setting(env, app_categories_setting);
    config.default_effort = text_setting(env, default_effort_setting);
    const Result<void> numbers = read_numbers(env, config);
    if (!numbers.has_value()) {
        return std::unexpected(numbers.error());
    }
    return config;
}

// Swapping the file between these checks and the read needs write access to the project
// directory, which already gives control of this configuration. A missing file reads as empty,
// so every setting takes its default.
Result<std::string> read_private_file(const std::filesystem::path& path) {
    const std::string shown = path.string();
    std::error_code status_error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, status_error);
    if (status.type() == std::filesystem::file_type::not_found) {
        return std::string{};
    }
    if (status_error) {
        return fail(Exit::config,
                    std::format("cannot inspect {}: {}", shown, status_error.message()));
    }
    if (status.type() == std::filesystem::file_type::symlink) {
        return fail(
            Exit::config,
            std::format("{} is a symlink; replace it with a regular file (mode 600)", shown));
    }
    if (status.type() != std::filesystem::file_type::regular) {
        return fail(Exit::config, std::format("{} is not a regular file", shown));
    }
    constexpr auto group_other =
        std::filesystem::perms::group_all | std::filesystem::perms::others_all;
    if ((status.permissions() & group_other) != std::filesystem::perms::none) {
        return fail(Exit::config, std::format("{} is accessible to group or others; run "
                                              "`chmod 600 {}`",
                                              shown, shown));
    }
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0) {
        const std::error_code lstat_error{errno, std::generic_category()};
        return fail(Exit::config,
                    std::format("cannot inspect {}: {}", shown, lstat_error.message()));
    }
    if (info.st_uid != ::geteuid()) {
        return fail(Exit::config,
                    std::format("{} is not owned by the current user; recreate it as yourself "
                                "with mode 600",
                                shown));
    }
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return fail(Exit::config, std::format("cannot open {}", shown));
    }
    std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) {
        return fail(Exit::config, std::format("cannot read {}", shown));
    }
    return text;
}

} // namespace

Result<std::map<std::string, std::string>> parse_env(std::string_view text) {
    EnvMap values;
    for (const auto [index, piece] : text | std::views::split('\n') | std::views::enumerate) {
        const EnvLine line = classify_line(std::string_view{piece.begin(), piece.end()});
        switch (line.kind) {
        case EnvLine::Kind::skip:
            break;
        case EnvLine::Kind::missing_equals:
            return fail(Exit::config, std::format("line {}: expected KEY=VALUE", index + 1));
        case EnvLine::Kind::bad_key:
            return fail(Exit::config,
                        std::format("line {}: a key must match [A-Z_][A-Z0-9_]*", index + 1));
        case EnvLine::Kind::assignment:
            values.insert_or_assign(std::string{line.key}, std::string{line.value});
            break;
        }
    }
    return values;
}

std::vector<PlaintextKey> find_plaintext_keys(std::string_view env_text) {
    std::vector<PlaintextKey> keys;
    for (const auto [index, piece] : env_text | std::views::split('\n') | std::views::enumerate) {
        const EnvLine line = classify_line(std::string_view{piece.begin(), piece.end()});
        if (line.kind == EnvLine::Kind::assignment && !line.value.empty() &&
            std::ranges::contains(plaintext_key_names, line.key)) {
            keys.push_back(PlaintextKey{
                .name = std::string{line.key},
                .line = static_cast<std::size_t>(index) + 1,
            });
        }
    }
    return keys;
}

std::filesystem::path default_home() {
    const char* const variable = std::getenv("OPENROUTER_CLI_HOME");
    if (variable != nullptr && *variable != '\0') {
        return std::filesystem::path{variable};
    }
    return std::filesystem::path{ORCLI_DEFAULT_HOME};
}

Result<Config> load_config(const std::filesystem::path& home,
                           const std::filesystem::path& user_home,
                           const std::filesystem::path& xdg_state_home) {
    const std::filesystem::path env_path = home / ".env";
    const Result<std::string> text = read_private_file(env_path);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    Result<Config> config =
        parse_env(*text).and_then(config_from_env).transform_error([&env_path](const Error& error) {
            return Error{
                .code = error.code,
                .message = std::format("{}: {}", env_path.string(), error.message),
            };
        });
    if (!config.has_value()) {
        return config;
    }
    // XDG Base Directory: a relative XDG_STATE_HOME is invalid and must be ignored.
    const std::filesystem::path state_root =
        xdg_state_home.is_absolute() ? xdg_state_home : user_home / ".local" / "state";
    if (!state_root.is_absolute()) {
        return fail(Exit::config, "HOME must be an absolute path to locate the cost log");
    }
    config->home = home;
    config->state_dir = home / "state";
    config->cost_log = state_root / "openrouter-cli" / "costs.jsonl";
    config->plaintext_keys = find_plaintext_keys(*text);
    return config;
}

} // namespace orcli
