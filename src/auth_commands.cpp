#include "account.hpp"
#include "auth.hpp"
#include "commands.hpp"
#include "common.hpp"
#include "config.hpp"
#include "credentials.hpp"
#include "fsutil.hpp"
#include "http.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <format>
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

// The status says what failed without the body, which may echo request details.
Result<std::string> verified_body(const Result<HttpResponse>& response, KeyKind kind) {
    if (!response) {
        return std::unexpected(response.error());
    }
    if (response->status == http_unauthorized) {
        return fail(Exit::api, std::format("OpenRouter rejected this {} (HTTP 401); nothing was "
                                           "stored",
                                           kind_label(kind)));
    }
    if (!http_success(response->status)) {
        return fail(Exit::api, std::format("OpenRouter answered HTTP {} while checking the {}; "
                                           "nothing was stored",
                                           response->status, kind_label(kind)));
    }
    return response->body;
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

Result<void> verify_management_key(const Config& config, const SecretString& key) {
    const Http http(config, HttpKeys{.inference = SecretString(), .management = key.clone()});
    const auto body = verified_body(http.fetch("/credits", GetOptions{.auth = Auth::management}),
                                    KeyKind::management);
    if (!body) {
        return std::unexpected(body.error());
    }
    const auto credits = parse_credits(*body);
    if (!credits) {
        return std::unexpected(credits.error());
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
    std::print(stderr, "Paste the OpenRouter {} (input is hidden): ",
               kind == KeyKind::management ? "Management" : "API");
    std::fflush(stderr);
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
        return fail(Exit::usage, "no code was pasted; run `openrouter-cli auth login` again");
    }
    return code;
}

// After the exchange request is sent, OpenRouter may hold a key this process never stored.
void print_minted_key_hint(std::string_view label, bool certainly_created) {
    diagnostic("openrouter-cli: OpenRouter {} the key '{}'; if "
               "https://openrouter.ai/settings/keys lists it, revoke it",
               certainly_created ? "created" : "may have created", label);
}

} // namespace

int cmd_auth_login(const Config& config, bool replace) {
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
    const std::string label = std::format("openrouter-cli on {}", host_name());
    std::println(stderr, "Sign in to OpenRouter\n\n"
                         "1. Open this link in your browser. Over SSH, use any computer:\n");
    std::println("{}", authorization_url(pkce->challenge, label));
    if (std::fflush(stdout) != 0) {
        return report(Error{.code = Exit::config, .message = "cannot write to standard output"});
    }
    std::println(stderr, "\n2. Sign in to your OpenRouter account and approve access.\n"
                         "3. Copy the code shown in your browser and return to this terminal.\n");
    std::print(stderr, "Paste the code here, then press Enter: ");
    if (std::fflush(stderr) != 0) {
        return report(Error{.code = Exit::config, .message = "cannot write the login prompt"});
    }

    auto code = read_code();
    std::println(stderr, "");
    if (!code) {
        return report(code.error());
    }
    std::println(stderr, "Completing sign-in with OpenRouter...");
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
    warn_if_not_durable(KeyKind::inference, *stored, "openrouter-cli auth login");
    diagnostic("openrouter-cli: logged in; the key is stored encrypted in {}",
               credential_file(config.state_dir, KeyKind::inference).string());
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

int cmd_auth_logout(const Config& config, bool management_only) {
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
