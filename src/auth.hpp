#ifndef ORCLI_AUTH_HPP
#define ORCLI_AUTH_HPP

#include "common.hpp"
#include "credentials.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace orcli {

// OAuth PKCE for OpenRouter's headless flow
// (https://openrouter.ai/docs/guides/overview/auth/oauth, "Headless Apps").

// RFC 4648 section 5 alphabet, no padding.
std::string base64url(std::span<const std::uint8_t> bytes);

// RFC 3986 percent-encoding: unreserved characters (A-Z a-z 0-9 - . _ ~) kept, every other
// byte encoded as %XX with uppercase hex.
std::string percent_encode(std::string_view text);

// base64url(SHA-256(ASCII bytes of verifier)) through OpenSSL EVP; any OpenSSL failure is
// Exit::config.
Result<std::string> pkce_challenge(std::string_view verifier);

struct Pkce {
    std::string verifier;  // base64url of 64 RAND_bytes: 86 characters (RFC 7636: 43 to 128)
    std::string challenge; // S256
};

// Fresh pair for each login attempt; fails closed (Exit::config) when RAND_bytes or EVP fail.
Result<Pkce> new_pkce();

// "openrouter-cli-on-HOST": the name OpenRouter gives the key this login creates. Whitespace in
// the host name becomes a dash, so the label is one word wherever OpenRouter lists it.
std::string login_key_label(std::string_view host);

// https://openrouter.ai/auth?code_challenge=C&code_challenge_method=S256&key_label=L with every
// query value percent-encoded and no callback_url, so the page shows the code to paste.
std::string authorization_url(std::string_view challenge, std::string_view key_label);

// JSON body for POST /auth/keys: code, code_verifier, code_challenge_method "S256".
std::string exchange_body(std::string_view code, std::string_view verifier);

// Parses the exchange response {"key": "...", "user_id": "..."}; Exit::api when the key is
// missing or not a string. Error messages never contain the response text.
Result<SecretString> parse_exchange(std::string_view json_text);

} // namespace orcli

#endif // ORCLI_AUTH_HPP
