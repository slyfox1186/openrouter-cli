#ifndef ORCLI_COMMANDS_HPP
#define ORCLI_COMMANDS_HPP

#include "common.hpp"
#include "config.hpp"
#include "credentials.hpp"
#include "display.hpp"

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace orcli {

// Subcommands that talk to the account (auth_commands.cpp, account_commands.cpp). Each returns
// the process exit code and prints its own errors through report(). main.cpp owns the grammar,
// the plaintext-key guard and dispatch.

// ----- shared helpers (commands.cpp) -------------------------------------------------------

[[nodiscard]] int exit_code(Exit exit);

// Write and flush; false when either failed. They never throw, so a closed or full stream
// cannot replace an exit code or stop the work that follows a message.
bool write_stdout(std::string_view text) noexcept;
bool write_stderr(std::string_view text) noexcept;

// One stderr line with terminal controls neutralized, which includes newlines: multi-line text
// goes through write_stderr instead.
template <typename... Args>
bool diagnostic(std::format_string<Args...> format, Args&&... args) noexcept {
    try {
        std::string line = display_metadata(std::format(format, std::forward<Args>(args)...));
        line.push_back('\n');
        return write_stderr(line);
    } catch (...) {
        return false;
    }
}

// Prints "openrouter-cli: <message>" to stderr and returns the error's exit code.
int report(const Error& error);

// This PC's host name, or "this PC" when it cannot be read.
std::string host_name();

// find_creds_tool() then load_key(); errors already name the fix.
Result<SecretString> load_stored_key(const Config& config, KeyKind kind);

// When .env still holds plaintext key lines, prints one warning per line ("line N: NAME") with
// the fix: store the key with `auth set` and delete the line. Returns true when any exist.
bool warn_plaintext_keys(const Config& config);

[[nodiscard]] bool http_success(long status);

// ----- auth (auth_commands.cpp) ------------------------------------------------------------

// Headless OAuth PKCE: prints the authorization URL, reads the pasted code from stdin,
// exchanges it (POST /auth/keys, no Authorization header, never retried), verifies the new key
// (GET /key, is_management_key must be false) and stores it with store_key().
int cmd_login(const Config& config, bool replace);

// Reads a key from a hidden terminal prompt or from piped stdin, checks the sk-or- prefix,
// verifies it with GET /key (is_management_key must be false for an API key and true for a
// Management key) and stores it with store_key().
int cmd_auth_set(const Config& config, bool management, bool replace);

// Deletes the stored credentials (only the Management key when management_only). Never
// decrypts, never needs systemd-creds, never contacts OpenRouter.
int cmd_logout(const Config& config, bool management_only);

// ----- status and account (account_commands.cpp) ------------------------------------------

// Identity and usage of the logged-in account; see the plan's `status` section.
int cmd_status(const Config& config);

int cmd_account_balance(const Config& config);            // GET /key, inference key
int cmd_account_keys(const Config& config);               // GET /keys, paged, management key
int cmd_account_credits(const Config& config);            // GET /credits, management key
int cmd_account_activity(const Config& config, int days); // GET /activity, management key

} // namespace orcli

#endif // ORCLI_COMMANDS_HPP
