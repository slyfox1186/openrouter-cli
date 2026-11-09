#include "config.hpp"

#include "common.hpp"

#include <doctest/doctest.h>

#include <unistd.h>

#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

namespace fs = std::filesystem;

constexpr std::string_view test_key = "sk-or-v1-test";
constexpr std::string_view key_line = "OPENROUTER_API_KEY=sk-or-v1-test\n";
constexpr fs::perms owner_read_write = fs::perms::owner_read | fs::perms::owner_write;
constexpr fs::perms world_readable =
    owner_read_write | fs::perms::group_read | fs::perms::others_read;

constexpr long long default_max_output_tokens = 64000;
constexpr int default_catalog_ttl_hours = 24;
constexpr int default_idle_timeout_seconds = 300;
constexpr int default_total_timeout_seconds = 3600;
constexpr std::size_t default_max_file_bytes = 1048576;
constexpr std::size_t default_max_input_bytes = 4194304;

class TempDir {
public:
    explicit TempDir(std::string_view label)
        : path_(fs::temp_directory_path() /
                std::format("orcli-test-config-{}-{}", ::getpid(), label)) {
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    [[nodiscard]] const fs::path& path() const { return path_; }
    [[nodiscard]] fs::path env_file() const { return path_ / ".env"; }
    [[nodiscard]] fs::path user_home() const { return path_ / "user"; }

private:
    fs::path path_;
};

void write_file(const fs::path& file, std::string_view text, fs::perms mode) {
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << text;
        REQUIRE(out.good());
    }
    fs::permissions(file, mode);
}

orcli::Result<orcli::Config> load(const TempDir& dir, std::string_view env_text) {
    write_file(dir.env_file(), env_text, owner_read_write);
    return orcli::load_config(dir.path(), dir.user_home(), fs::path{});
}

orcli::Config load_ok(const TempDir& dir, std::string_view env_text) {
    orcli::Result<orcli::Config> config = load(dir, env_text);
    REQUIRE(config.has_value());
    return std::move(*config);
}

orcli::Error load_error(const TempDir& dir, std::string_view env_text) {
    const orcli::Result<orcli::Config> config = load(dir, env_text);
    REQUIRE_FALSE(config.has_value());
    CHECK(config.error().code == orcli::Exit::config);
    return config.error();
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

} // namespace

TEST_CASE("config: parse_env handles quotes, comments, blanks and whitespace") {
    const orcli::Result<std::map<std::string, std::string>> parsed =
        orcli::parse_env("# comment\n"
                         "\n"
                         "   \n"
                         "  # indented comment\n"
                         "PLAIN=1\n"
                         "SPACED = two words \n"
                         "SINGLE='single quoted'\n"
                         "DOUBLE=\"double quoted\"\n"
                         "MISMATCHED='open\"\n"
                         "EMPTY=\n"
                         "EMPTY_QUOTES=\"\"\n"
                         "WITH_EQUALS=a=b\n"
                         "CRLF=windows\r\n"
                         "_UNDER_9=x");
    REQUIRE(parsed.has_value());
    constexpr std::size_t setting_count = 10;
    const std::map<std::string, std::string>& values = *parsed;
    CHECK(values.size() == setting_count);
    CHECK(values.at("PLAIN") == "1");
    CHECK(values.at("SPACED") == "two words");
    CHECK(values.at("SINGLE") == "single quoted");
    CHECK(values.at("DOUBLE") == "double quoted");
    CHECK(values.at("MISMATCHED") == "'open\"");
    CHECK(values.at("EMPTY").empty());
    CHECK(values.at("EMPTY_QUOTES").empty());
    CHECK(values.at("WITH_EQUALS") == "a=b");
    CHECK(values.at("CRLF") == "windows");
    CHECK(values.at("_UNDER_9") == "x");
}

TEST_CASE("config: parse_env accepts empty input") {
    const auto parsed = orcli::parse_env("");
    REQUIRE(parsed.has_value());
    CHECK(parsed->empty());
}

TEST_CASE("config: parse_env later duplicate wins") {
    const auto parsed = orcli::parse_env("KEY=first\nKEY=second\n");
    REQUIRE(parsed.has_value());
    CHECK(parsed->at("KEY") == "second");
}

TEST_CASE("config: malformed line names the line number, never the value") {
    const auto parsed = orcli::parse_env("GOOD=1\n\nsk-or-v1-leaked-secret\n");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == orcli::Exit::config);
    CHECK(contains(parsed.error().message, "line 3"));
    CHECK_FALSE(contains(parsed.error().message, "leaked-secret"));
}

TEST_CASE("config: invalid key names are rejected without echoing the line") {
    for (const std::string_view line :
         {"lower=hidden-value", "1START=hidden-value", "BAD-KEY=hidden-value", "=hidden-value"}) {
        CAPTURE(line);
        const auto parsed = orcli::parse_env(std::format("OK=1\n{}\n", line));
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().code == orcli::Exit::config);
        CHECK(contains(parsed.error().message, "line 2"));
        CHECK_FALSE(contains(parsed.error().message, "hidden-value"));
    }
}

TEST_CASE("config: defaults with only the API key") {
    const TempDir dir("defaults");
    const orcli::Config config = load_ok(dir, key_line);
    CHECK(config.api_key == test_key);
    CHECK(config.base_url == "https://openrouter.ai/api/v1");
    CHECK(config.app_referer.empty());
    CHECK(config.app_title == "openrouter-cli");
    CHECK(config.app_categories == "cli-agent");
    CHECK(config.default_effort == "high");
    CHECK(config.max_output_tokens == default_max_output_tokens);
    CHECK(config.privacy == orcli::Privacy::zdr);
    CHECK(config.catalog_ttl_hours == default_catalog_ttl_hours);
    CHECK(config.idle_timeout_seconds == default_idle_timeout_seconds);
    CHECK(config.total_timeout_seconds == default_total_timeout_seconds);
    CHECK(config.max_file_bytes == default_max_file_bytes);
    CHECK(config.max_input_bytes == default_max_input_bytes);
    CHECK(config.home == dir.path());
    CHECK(config.state_dir == dir.path() / "state");
    CHECK(config.cost_log ==
          dir.user_home() / ".local" / "state" / "openrouter-cli" / "costs.jsonl");
    CHECK_FALSE(fs::exists(config.state_dir));
}

TEST_CASE("config: every setting can be overridden and unknown keys are ignored") {
    constexpr long long max_output_tokens = 1000;
    constexpr int catalog_ttl_hours = 12;
    constexpr int idle_timeout_seconds = 60;
    constexpr int total_timeout_seconds = 600;
    constexpr std::size_t max_file_bytes = 2048;
    constexpr std::size_t max_input_bytes = 8192;
    const TempDir dir("overrides");
    const orcli::Config config =
        load_ok(dir, std::format("OPENROUTER_API_KEY=\"sk-or-v1-custom\"\n"
                                 "OPENROUTER_CLI_BASE_URL=https://eu.openrouter.ai/api/v1/\n"
                                 "OPENROUTER_CLI_APP_REFERER=https://example.com\n"
                                 "OPENROUTER_CLI_APP_TITLE='My Tool'\n"
                                 "OPENROUTER_CLI_APP_CATEGORIES=cli-agent,review\n"
                                 "OPENROUTER_CLI_DEFAULT_EFFORT=medium\n"
                                 "OPENROUTER_CLI_MAX_OUTPUT_TOKENS={}\n"
                                 "OPENROUTER_CLI_PRIVACY=no-training\n"
                                 "OPENROUTER_CLI_CATALOG_TTL_HOURS={}\n"
                                 "OPENROUTER_CLI_IDLE_TIMEOUT_SECONDS={}\n"
                                 "OPENROUTER_CLI_TOTAL_TIMEOUT_SECONDS={}\n"
                                 "OPENROUTER_CLI_MAX_FILE_BYTES={}\n"
                                 "OPENROUTER_CLI_MAX_INPUT_BYTES={}\n"
                                 "UNRELATED_SETTING=whatever\n",
                                 max_output_tokens, catalog_ttl_hours, idle_timeout_seconds,
                                 total_timeout_seconds, max_file_bytes, max_input_bytes));
    CHECK(config.api_key == "sk-or-v1-custom");
    CHECK(config.base_url == "https://eu.openrouter.ai/api/v1");
    CHECK(config.app_referer == "https://example.com");
    CHECK(config.app_title == "My Tool");
    CHECK(config.app_categories == "cli-agent,review");
    CHECK(config.default_effort == "medium");
    CHECK(config.max_output_tokens == max_output_tokens);
    CHECK(config.privacy == orcli::Privacy::no_training);
    CHECK(config.catalog_ttl_hours == catalog_ttl_hours);
    CHECK(config.idle_timeout_seconds == idle_timeout_seconds);
    CHECK(config.total_timeout_seconds == total_timeout_seconds);
    CHECK(config.max_file_bytes == max_file_bytes);
    CHECK(config.max_input_bytes == max_input_bytes);
}

TEST_CASE("config: empty setting values select the defaults") {
    const TempDir dir("empty-values");
    const orcli::Config config = load_ok(dir, std::format("{}OPENROUTER_CLI_MAX_OUTPUT_TOKENS=\n"
                                                          "OPENROUTER_CLI_PRIVACY=\n"
                                                          "OPENROUTER_CLI_APP_TITLE=\n",
                                                          key_line));
    CHECK(config.max_output_tokens == default_max_output_tokens);
    CHECK(config.privacy == orcli::Privacy::zdr);
    CHECK(config.app_title == "openrouter-cli");
}

TEST_CASE("config: .env readable by group or others is rejected") {
    const TempDir dir("mode");
    write_file(dir.env_file(), key_line, world_readable);
    const auto config = orcli::load_config(dir.path(), dir.user_home(), fs::path{});
    REQUIRE_FALSE(config.has_value());
    CHECK(config.error().code == orcli::Exit::config);
    CHECK(contains(config.error().message, dir.env_file().string()));
    CHECK(contains(config.error().message, "chmod 600"));
}

TEST_CASE("config: symlinked .env is rejected") {
    const TempDir dir("symlink");
    const fs::path target = dir.path() / "real.env";
    write_file(target, key_line, owner_read_write);
    fs::create_symlink(target, dir.env_file());
    const auto config = orcli::load_config(dir.path(), dir.user_home(), fs::path{});
    REQUIRE_FALSE(config.has_value());
    CHECK(config.error().code == orcli::Exit::config);
    CHECK(contains(config.error().message, dir.env_file().string()));
    CHECK(contains(config.error().message, "symlink"));
}

TEST_CASE("config: missing .env or a directory in its place is rejected") {
    const TempDir dir("missing");
    const auto missing = orcli::load_config(dir.path(), dir.user_home(), fs::path{});
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == orcli::Exit::config);
    CHECK(contains(missing.error().message, dir.env_file().string()));

    REQUIRE(fs::create_directory(dir.env_file()));
    fs::permissions(dir.env_file(), fs::perms::owner_all);
    const auto directory = orcli::load_config(dir.path(), dir.user_home(), fs::path{});
    REQUIRE_FALSE(directory.has_value());
    CHECK(directory.error().code == orcli::Exit::config);
}

TEST_CASE("config: missing or empty API key is rejected") {
    const TempDir dir("api-key");
    CHECK(contains(load_error(dir, "OPENROUTER_CLI_PRIVACY=zdr\n").message, "OPENROUTER_API_KEY"));
    CHECK(contains(load_error(dir, "OPENROUTER_API_KEY=\n").message, "OPENROUTER_API_KEY"));
    CHECK(contains(load_error(dir, "OPENROUTER_API_KEY=''\n").message, "OPENROUTER_API_KEY"));
}

TEST_CASE("config: malformed .env line is reported with the path and line") {
    const TempDir dir("malformed");
    const orcli::Error error = load_error(dir, std::format("{}not-a-setting\n", key_line));
    CHECK(contains(error.message, dir.env_file().string()));
    CHECK(contains(error.message, "line 2"));
    CHECK_FALSE(contains(error.message, "not-a-setting"));
}

TEST_CASE("config: bad numbers name the key, never the value") {
    const TempDir dir("numbers");
    for (const std::string_view value :
         {"abc", "0", "-5", "12abc", "1.5", "+7", "99999999999999999999999"}) {
        CAPTURE(value);
        const orcli::Error error = load_error(
            dir, std::format("{}OPENROUTER_CLI_MAX_OUTPUT_TOKENS={}\n", key_line, value));
        CHECK(contains(error.message, "OPENROUTER_CLI_MAX_OUTPUT_TOKENS"));
        CHECK_FALSE(contains(error.message, std::format("={}", value)));
    }
    const orcli::Error overflow =
        load_error(dir, std::format("{}OPENROUTER_CLI_CATALOG_TTL_HOURS=4294967296\n", key_line));
    CHECK(contains(overflow.message, "OPENROUTER_CLI_CATALOG_TTL_HOURS"));
    const orcli::Error negative_size =
        load_error(dir, std::format("{}OPENROUTER_CLI_MAX_FILE_BYTES=-1\n", key_line));
    CHECK(contains(negative_size.message, "OPENROUTER_CLI_MAX_FILE_BYTES"));
}

TEST_CASE("config: privacy values") {
    const TempDir dir("privacy");
    CHECK(load_ok(dir, std::format("{}OPENROUTER_CLI_PRIVACY=zdr\n", key_line)).privacy ==
          orcli::Privacy::zdr);
    CHECK(load_ok(dir, std::format("{}OPENROUTER_CLI_PRIVACY=no-training\n", key_line)).privacy ==
          orcli::Privacy::no_training);
    const orcli::Error error =
        load_error(dir, std::format("{}OPENROUTER_CLI_PRIVACY=off\n", key_line));
    CHECK(contains(error.message, "OPENROUTER_CLI_PRIVACY"));
}

TEST_CASE("config: base URL validation") {
    const TempDir dir("base-url");
    const auto base_url_env = [](std::string_view url) {
        return std::format("{}OPENROUTER_CLI_BASE_URL={}\n", key_line, url);
    };

    CHECK(load_ok(dir, base_url_env("https://openrouter.ai/api/v1//")).base_url ==
          "https://openrouter.ai/api/v1");
    CHECK(load_ok(dir, base_url_env("http://127.0.0.1:8080/api/v1/")).base_url ==
          "http://127.0.0.1:8080/api/v1");
    CHECK(load_ok(dir, base_url_env("http://localhost:9000")).base_url == "http://localhost:9000");
    CHECK(load_ok(dir, base_url_env("http://localhost")).base_url == "http://localhost");

    for (const std::string_view url :
         {"http://openrouter.ai/api/v1", "http://localhost.example.com/api",
          "http://127.0.0.1.example.com", "https://", "ftp://openrouter.ai", "openrouter.ai"}) {
        CAPTURE(url);
        const orcli::Error error = load_error(dir, base_url_env(url));
        CHECK(contains(error.message, "OPENROUTER_CLI_BASE_URL"));
    }
}

TEST_CASE("config: cost log follows XDG_STATE_HOME when it is absolute") {
    const TempDir dir("cost-log");
    write_file(dir.env_file(), key_line, owner_read_write);
    const fs::path xdg = dir.path() / "xdg-state";

    const auto with_xdg = orcli::load_config(dir.path(), dir.user_home(), xdg);
    REQUIRE(with_xdg.has_value());
    CHECK(with_xdg->cost_log == xdg / "openrouter-cli" / "costs.jsonl");

    const fs::path fallback =
        dir.user_home() / ".local" / "state" / "openrouter-cli" / "costs.jsonl";
    const auto without_xdg = orcli::load_config(dir.path(), dir.user_home(), fs::path{});
    REQUIRE(without_xdg.has_value());
    CHECK(without_xdg->cost_log == fallback);

    const auto relative_xdg =
        orcli::load_config(dir.path(), dir.user_home(), fs::path{"relative/state"});
    REQUIRE(relative_xdg.has_value());
    CHECK(relative_xdg->cost_log == fallback);

    const auto relative_home =
        orcli::load_config(dir.path(), fs::path{"relative-home"}, fs::path{});
    REQUIRE_FALSE(relative_home.has_value());
    CHECK(relative_home.error().code == orcli::Exit::config);
}
