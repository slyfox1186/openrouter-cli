#include "account.hpp"
#include "auth.hpp"
#include "commands.hpp"
#include "common.hpp"
#include "config.hpp"
#include "credentials.hpp"
#include "display.hpp"
#include "fsutil.hpp"
#include "http.hpp"
#include "terminal.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <format>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <termios.h>
#include <unistd.h>

namespace orcli {

namespace {

constexpr long http_unauthorized = 401;
constexpr std::size_t max_key_input_bytes = 4096;
constexpr std::string_view key_prefix = "sk-or-";
constexpr std::string_view blank_chars = " \t\r\n";
constexpr std::string_view revoke_hint =
    "the key stays valid at OpenRouter until you revoke it at https://openrouter.ai/settings/keys";

// The help pages use the same codes for the same roles.
constexpr std::string_view sgr_strong = "1";
constexpr std::string_view sgr_literal = "1;36";
constexpr std::string_view sgr_label = "36";
constexpr std::string_view sgr_quiet = "2";
constexpr std::string_view sgr_success = "1;32";

std::string_view kind_label(KeyKind kind) {
    return kind == KeyKind::management ? "Management key" : "API key";
}

std::string_view set_command(KeyKind kind) {
    return kind == KeyKind::management ? "openrouter-cli auth set --management"
                                       : "openrouter-cli auth set";
}

// Erasing in place keeps the key in one buffer, which cleanse() and SecretString clear in full
// (up to capacity), so trimming leaves no stray copy in freed memory.
void trim_in_place(std::string& text) {
    const std::size_t last = text.find_last_not_of(blank_chars);
    if (last == std::string::npos) {
        text.clear();
        return;
    }
    text.erase(last + 1);
    text.erase(0, text.find_first_not_of(blank_chars));
}

Result<std::string> verified_body(const Result<HttpResponse>& response, KeyKind kind) {
    if (!response) {
        return std::unexpected(response.error());
    }
    if (http_success(response->status)) {
        return response->body;
    }
    const std::optional<std::string> reason = provider_reason(response->body);
    const std::string status = reason.has_value()
                                   ? std::format("HTTP {}: {}", response->status, *reason)
                                   : std::format("HTTP {}", response->status);
    if (response->status == http_unauthorized) {
        return fail(Exit::api, std::format("OpenRouter rejected this {} ({}); nothing was stored",
                                           kind_label(kind), status));
    }
    return fail(Exit::api,
                std::format("OpenRouter answered {} while checking the {}; nothing was stored",
                            status, kind_label(kind)));
}

Result<void> verify_inference_key(const Config& config, const SecretString& key) {
    const Http http(config, HttpKeys{.inference = key.clone(), .management = SecretString()});
    const auto body =
        verified_body(http.fetch("/key", GetOptions{.auth = Auth::inference}), KeyKind::inference);
    if (!body) {
        return std::unexpected(body.error());
    }
    const auto status = parse_key_status(*body);
    if (!status) {
        return std::unexpected(status.error());
    }
    if (status->is_management_key) {
        return fail(Exit::usage, "this is a Management key; store it with `openrouter-cli auth "
                                 "set --management`");
    }
    return {};
}

// A readable /credits reply does not prove the key type, so the key's own record decides.
Result<void> verify_management_key(const Config& config, const SecretString& key) {
    const Http http(config, HttpKeys{.inference = SecretString(), .management = key.clone()});
    const auto body = verified_body(http.fetch("/key", GetOptions{.auth = Auth::management}),
                                    KeyKind::management);
    if (!body) {
        return std::unexpected(body.error());
    }
    const auto status = parse_key_status(*body);
    if (!status) {
        return std::unexpected(status.error());
    }
    if (!status->is_management_key) {
        return fail(Exit::usage, "this is an API key, not a Management key; store it with "
                                 "`openrouter-cli auth set`");
    }
    return {};
}

VerifyKey verifier_for(const Config& config, KeyKind kind) {
    if (kind == KeyKind::management) {
        return [&config](const SecretString& key) { return verify_management_key(config, key); };
    }
    return [&config](const SecretString& key) { return verify_inference_key(config, key); };
}

void warn_if_not_durable(KeyKind kind, const StoreOutcome& outcome,
                         std::string_view repeat_command) {
    if (!outcome.durable) {
        diagnostic("openrouter-cli: warning: the {} is stored and working, but the directory "
                   "sync failed, so its survival of a power loss is unconfirmed; after a "
                   "restart run `openrouter-cli status` and repeat `{}` if it is gone",
                   kind_label(kind), repeat_command);
    }
}

// Clears ECHO for the life of the guard so a pasted key never appears on screen; TCSAFLUSH
// also discards anything typed before the prompt.
class HiddenTerminal {
public:
    explicit HiddenTerminal(int descriptor) : descriptor_(descriptor) {
        if (::tcgetattr(descriptor_, &saved_) != 0) {
            return;
        }
        termios hidden = saved_;
        hidden.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        active_ = ::tcsetattr(descriptor_, TCSAFLUSH, &hidden) == 0;
    }
    HiddenTerminal(HiddenTerminal&&) = delete;
    HiddenTerminal& operator=(HiddenTerminal&&) = delete;
    HiddenTerminal(const HiddenTerminal&) = delete;
    HiddenTerminal& operator=(const HiddenTerminal&) = delete;
    ~HiddenTerminal() {
        if (active_) {
            ::tcsetattr(descriptor_, TCSAFLUSH, &saved_);
        }
    }
    [[nodiscard]] bool active() const { return active_; }

private:
    int descriptor_;
    termios saved_{};
    bool active_ = false;
};

enum class ReadUntil : std::uint8_t {
    newline,
    end_of_input,
};

Result<void> read_bounded(std::string& text, ReadUntil until) {
    auto input = read_input(max_key_input_bytes, until == ReadUntil::newline);
    if (!input) {
        return std::unexpected(input.error());
    }
    text.swap(*input);
    cleanse(*input);
    return {};
}

Result<void> read_hidden_line(KeyKind kind, std::string& text) {
    if (!write_stderr(std::format("Paste the OpenRouter {} (input is hidden): ",
                                  kind == KeyKind::management ? "Management" : "API"))) {
        return fail(Exit::config, "cannot write the key prompt");
    }
    const HiddenTerminal hidden(STDIN_FILENO);
    if (!hidden.active()) {
        std::print(stderr, "\n");
        return fail(Exit::config, "cannot turn off terminal echo; pipe the key on standard input "
                                  "instead");
    }
    const auto read = read_bounded(text, ReadUntil::newline);
    std::print(stderr, "\n");
    if (!read) {
        return std::unexpected(read.error());
    }
    return {};
}

Result<SecretString> read_key(KeyKind kind) {
    std::string text;
    const auto read = ::isatty(STDIN_FILENO) == 1 ? read_hidden_line(kind, text)
                                                  : read_bounded(text, ReadUntil::end_of_input);
    if (!read) {
        cleanse(text);
        return std::unexpected(read.error());
    }
    trim_in_place(text);
    if (text.empty()) {
        cleanse(text);
        return fail(Exit::usage, "no key was given");
    }
    if (!text.starts_with(key_prefix)) {
        cleanse(text);
        return fail(Exit::usage, "an OpenRouter key starts with sk-or-");
    }
    // read_bounded reserved a heap buffer, so the move hands that buffer to SecretString and
    // leaves no second copy behind.
    return SecretString(std::move(text));
}

Result<std::string> read_code() {
    std::string code;
    if (const auto read = read_bounded(code, ReadUntil::newline); !read) {
        cleanse(code);
        return std::unexpected(read.error());
    }
    trim_in_place(code);
    if (code.empty()) {
        return fail(Exit::usage, "no code was pasted; run `openrouter-cli login` again");
    }
    return code;
}

std::string login_heading(bool color) {
    return std::format("{}\n\n"
                       "  {} Open this link in a browser. Over SSH, a browser on any computer "
                       "works.\n\n",
                       paint(sgr_strong, "Sign in to OpenRouter", color),
                       paint(sgr_strong, "1.", color));
}

// Ends with the prompt, without a newline, so the pasted code lands on the prompt's line.
std::string login_steps_and_prompt(bool color) {
    return std::format("\n  {} Sign in to your OpenRouter account and approve access.\n"
                       "  {} Copy the code the browser shows you, then come back to this "
                       "terminal.\n\n"
                       "{} ",
                       paint(sgr_strong, "2.", color), paint(sgr_strong, "3.", color),
                       paint(sgr_strong, "Paste the code here, then press Enter:", color));
}

// The key name and the path are shown on their own short rows so neither wraps mid-sentence.
std::string login_success(std::string_view label, std::string_view file, bool color) {
    return std::format("\n{} Your new API key is stored encrypted.\n\n"
                       "  {}  {}\n"
                       "  {}  {}\n\n"
                       "Next: {}\n",
                       paint(sgr_success, "Signed in to OpenRouter.", color),
                       paint(sgr_label, "Name", color), display_metadata(label),
                       paint(sgr_label, "File", color), display_metadata(file),
                       paint(sgr_literal, "openrouter-cli status", color));
}

// After the exchange request is sent, OpenRouter may hold a key this process never stored.
void print_minted_key_hint(std::string_view label, bool certainly_created) {
    diagnostic("openrouter-cli: OpenRouter {} the key '{}'; if "
               "https://openrouter.ai/settings/keys lists it, revoke it",
               certainly_created ? "created" : "may have created", label);
}

} // namespace

int cmd_login(const Config& config, bool replace) {
    const auto tool = find_creds_tool();
    if (!tool) {
        return report(tool.error());
    }
    const auto already = key_stored(config.state_dir, KeyKind::inference);
    if (!already) {
        return report(already.error());
    }
    if (*already && !replace) {
        return report(Error{
            .code = Exit::refused,
            .message = "an API key is already stored; pass --replace",
        });
    }
    const auto pkce = new_pkce();
    if (!pkce) {
        return report(pkce.error());
    }
    const std::string label = login_key_label(host_name());
    const bool color = stream_color(STDERR_FILENO);
    write_stderr(login_heading(color));
    // The link is the only thing on stdout, so a script can capture it.
    if (!write_stdout(paint(sgr_literal, authorization_url(pkce->challenge, label),
                            stream_color(STDOUT_FILENO)) +
                      "\n")) {
        return report(Error{.code = Exit::config, .message = "cannot write to standard output"});
    }
    if (!write_stderr(login_steps_and_prompt(color))) {
        return report(Error{.code = Exit::config, .message = "cannot write the login prompt"});
    }

    auto code = read_code();
    write_stderr("\n");
    if (!code) {
        return report(code.error());
    }
    write_stderr(paint(sgr_quiet, "Completing sign-in with OpenRouter...", color) + "\n");
    // OpenRouter's OAuth guide sends the exchange without an Authorization header, while the
    // generated API reference lists bearer security for it; this follows the guide.
    const Http http(config, HttpKeys{});
    auto response = http.post_json("/auth/keys", exchange_body(*code, pkce->verifier), Auth::none);
    cleanse(*code);
    if (!response) {
        const int status = report(response.error());
        // Exit::config here means the request was never sent (no usable configuration); every
        // other failure, interruption included, may have happened after OpenRouter minted a key.
        if (response.error().code != Exit::config) {
            print_minted_key_hint(label, false);
        }
        return status;
    }
    std::string body = std::move(*response);
    const auto key = parse_exchange(body);
    cleanse(body);
    if (!key) {
        const int status = report(key.error());
        print_minted_key_hint(label, false);
        return status;
    }

    const auto stored = store_key(*tool, config.state_dir, KeyKind::inference, *key, replace,
                                  verifier_for(config, KeyKind::inference));
    if (!stored) {
        const int status = report(stored.error());
        print_minted_key_hint(label, true);
        return status;
    }
    warn_if_not_durable(KeyKind::inference, *stored, "openrouter-cli login");
    write_stderr(login_success(
        label, credential_file(config.state_dir, KeyKind::inference).string(), color));
    warn_plaintext_keys(config);
    return exit_code(Exit::ok);
}

int cmd_auth_set(const Config& config, bool management, bool replace) {
    const KeyKind kind = management ? KeyKind::management : KeyKind::inference;
    const auto key = read_key(kind);
    if (!key) {
        return report(key.error());
    }
    const auto tool = find_creds_tool();
    if (!tool) {
        return report(tool.error());
    }
    const auto stored =
        store_key(*tool, config.state_dir, kind, *key, replace, verifier_for(config, kind));
    if (!stored) {
        return report(stored.error());
    }
    warn_if_not_durable(kind, *stored, set_command(kind));
    diagnostic("openrouter-cli: the {} is stored encrypted in {}", kind_label(kind),
               credential_file(config.state_dir, kind).string());
    warn_plaintext_keys(config);
    return exit_code(Exit::ok);
}

int cmd_logout(const Config& config, bool management_only) {
    const auto removed = remove_keys(config.state_dir, !management_only);
    if (!removed) {
        return report(removed.error());
    }
    for (const KeyKind kind : *removed) {
        diagnostic("openrouter-cli: removed the stored {}", kind_label(kind));
    }
    if (removed->empty()) {
        diagnostic("openrouter-cli: already logged out");
    }
    diagnostic("openrouter-cli: {}", revoke_hint);
    warn_plaintext_keys(config);
    return exit_code(Exit::ok);
}

} // namespace orcli
