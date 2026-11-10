#include "commands.hpp"

#include "common.hpp"
#include "config.hpp"
#include "credentials.hpp"
#include "secrets.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <print>
#include <string>

#include <unistd.h>

namespace orcli {

namespace {

constexpr std::size_t host_name_bytes = 256;
constexpr long http_ok_min = 200;
constexpr long http_ok_max = 299;

} // namespace

int exit_code(Exit exit) {
    return static_cast<int>(exit);
}

int report(const Error& error) {
    std::println(stderr, "openrouter-cli: {}", display_metadata(error.message));
    return exit_code(error.code);
}

std::string host_name() {
    std::array<char, host_name_bytes> buffer{};
    if (::gethostname(buffer.data(), buffer.size() - 1) != 0) {
        return "this PC";
    }
    const std::string name(buffer.data());
    return name.empty() ? std::string("this PC") : name;
}

Result<SecretString> load_stored_key(const Config& config, KeyKind kind) {
    return find_creds_tool().and_then(
        [&](const CredsTool& tool) { return load_key(tool, config.state_dir, kind); });
}

bool warn_plaintext_keys(const Config& config) {
    for (const auto& found : config.plaintext_keys) {
        const bool management = found.name == "OPENROUTER_MANAGEMENT_KEY";
        std::println(stderr,
                     "openrouter-cli: {} line {} still holds {} in plain text; store it with "
                     "`openrouter-cli auth set{}` and then delete that line",
                     config.env_file.string(), found.line, found.name,
                     management ? " --management" : "");
    }
    return !config.plaintext_keys.empty();
}

bool http_success(long status) {
    return status >= http_ok_min && status <= http_ok_max;
}

} // namespace orcli
