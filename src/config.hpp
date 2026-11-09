#ifndef ORCLI_CONFIG_HPP
#define ORCLI_CONFIG_HPP

#include "common.hpp"

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace orcli {

// A non-empty OPENROUTER_API_KEY or OPENROUTER_MANAGEMENT_KEY assignment found in .env. Keys
// are stored encrypted (credentials.hpp); this records where a plaintext one still sits so the
// command can refuse to run. Never holds the value.
struct PlaintextKey {
    std::string name;
    std::size_t line = 0; // 1-based
};

struct Config {
    std::filesystem::path home;      // project directory holding .env and state/
    std::filesystem::path state_dir; // home / "state"
    std::filesystem::path
        cost_log; // $XDG_STATE_HOME or ~/.local/state, then openrouter-cli/costs.jsonl
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
    std::vector<PlaintextKey> plaintext_keys; // every such assignment, duplicates included
};

// Parses KEY=VALUE lines. Blank lines and lines starting with '#' are ignored; one pair of
// matching single or double quotes around a value is removed. Malformed lines are errors
// (Exit::config) naming the line number, never the value.
Result<std::map<std::string, std::string>> parse_env(std::string_view text);

// Every line assigning a non-empty value (after unquoting) to OPENROUTER_API_KEY or
// OPENROUTER_MANAGEMENT_KEY, in file order. Checked line by line because parse_env keeps only
// the last of duplicate keys, which would hide a plaintext key followed by an empty duplicate.
std::vector<PlaintextKey> find_plaintext_keys(std::string_view env_text);

// Project directory: OPENROUTER_CLI_HOME if set, else the directory compiled in at build time.
std::filesystem::path default_home();

// Loads the non-secret settings from home/.env. A missing .env means every setting takes its
// default. An existing .env must be a regular file owned by the user with no group or other
// permission bits, and every setting must be valid (Exit::config with the path otherwise).
// Plaintext key assignments are reported in Config::plaintext_keys, not rejected here; the
// caller decides (every command except `auth set` and `auth login` refuses).
// user_home is $HOME; xdg_state_home is $XDG_STATE_HOME or empty.
Result<Config> load_config(const std::filesystem::path& home,
                           const std::filesystem::path& user_home,
                           const std::filesystem::path& xdg_state_home);

} // namespace orcli

#endif // ORCLI_CONFIG_HPP
