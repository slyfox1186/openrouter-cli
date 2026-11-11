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

// Output budget, reasoning included, when neither OPENROUTER_CLI_MAX_OUTPUT_TOKENS nor
// --max-output-tokens is given. Fixed rather than each model's catalog maximum: that figure is
// often a placeholder, and a budget above a provider's per-request limit can switch it into
// continuation modes this client does not drive.
inline constexpr long long default_max_output_tokens = 64000;

// A non-empty OPENROUTER_API_KEY or OPENROUTER_MANAGEMENT_KEY assignment found in .env. Keys
// are stored encrypted (credentials.hpp); this records where a plaintext one still sits so the
// command can refuse to run. Never holds the value.
struct PlaintextKey {
    std::string name;
    std::size_t line = 0; // 1-based
};

struct Config {
    std::filesystem::path home; // legacy override, empty for XDG storage
    std::filesystem::path env_file;
    std::filesystem::path state_dir;
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

// Legacy override from OPENROUTER_CLI_HOME, or empty for XDG defaults.
std::filesystem::path default_home();

// Loads XDG config.env, or home/.env for a nonempty absolute legacy override. Missing settings
// use defaults without creating directories. Existing settings require ownership and no group or
// other permission bits, and every setting must be valid (Exit::config with the path otherwise).
// Plaintext key assignments are reported in Config::plaintext_keys, not rejected here; the
// caller decides which credential-recovery commands may proceed.
// user_home is HOME; relative or empty XDG paths use the corresponding HOME fallback.
Result<Config> load_config(const std::filesystem::path& home,
                           const std::filesystem::path& user_home,
                           const std::filesystem::path& xdg_state_home,
                           const std::filesystem::path& xdg_config_home = {});

} // namespace orcli

#endif // ORCLI_CONFIG_HPP
