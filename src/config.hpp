#pragma once

#include "common.hpp"

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace orcli {

struct Config {
    std::filesystem::path home;      // project directory holding .env and state/
    std::filesystem::path state_dir; // home / "state"
    std::filesystem::path cost_log;  // $XDG_STATE_HOME or ~/.local/state, then openrouter-cli/costs.jsonl
    std::string api_key;
    std::string base_url;
    std::string app_referer;
    std::string app_title;
    std::string app_categories;
    std::string default_effort;
    long long max_output_tokens = 0;
    Privacy privacy = Privacy::zdr;
    int catalog_ttl_hours = 0;
    int idle_timeout_seconds = 0;
    int total_timeout_seconds = 0;
    std::size_t max_file_bytes = 0;
    std::size_t max_input_bytes = 0;
};

// Parses KEY=VALUE lines. Blank lines and lines starting with '#' are ignored; one pair of
// matching single or double quotes around a value is removed. Malformed lines are errors
// (Exit::config) naming the line number, never the value.
Result<std::map<std::string, std::string>> parse_env(std::string_view text);

// Project directory: OPENROUTER_CLI_HOME if set, else the directory compiled in at build time.
std::filesystem::path default_home();

// Loads home/.env. Requires a regular file owned by the user with no group or other permission
// bits, a non-empty OPENROUTER_API_KEY, and valid values for every setting (Exit::config with
// the path otherwise). user_home is $HOME; xdg_state_home is $XDG_STATE_HOME or empty.
Result<Config> load_config(const std::filesystem::path& home,
                           const std::filesystem::path& user_home,
                           const std::filesystem::path& xdg_state_home);

} // namespace orcli
