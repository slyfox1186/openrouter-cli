#include "auth.hpp"

#include "common.hpp"
#include "credentials.hpp"

#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace orcli {

namespace {

using json = nlohmann::json;

constexpr std::string_view base64url_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr unsigned int bits_per_byte = 8;
constexpr unsigned int bits_per_sextet = 6;
constexpr std::uint32_t sextet_mask = 0x3F;
constexpr std::string_view hex_digits = "0123456789ABCDEF";
constexpr unsigned int nibble_bits = 4;
constexpr unsigned int nibble_mask = 0x0F;
// 64 bytes encode to an 86-character verifier, inside RFC 7636's 43 to 128.
constexpr int verifier_bytes = 64;
constexpr int compact_dump = -1;
constexpr std::string_view exchange_error = "unexpected response from POST /auth/keys";

struct DigestContextFree {
    void operator()(EVP_MD_CTX* context) const noexcept { EVP_MD_CTX_free(context); }
};
using DigestContext = std::unique_ptr<EVP_MD_CTX, DigestContextFree>;

bool is_unreserved(char character) {
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '-' || character == '.' ||
           character == '_' || character == '~';
}

} // namespace

std::string base64url(std::span<const std::uint8_t> bytes) {
    std::string encoded;
    encoded.reserve((bytes.size() * 4 + 2) / 3);
    // Only the low `pending` bits of `buffer` are unencoded; higher bits are masked off.
    std::uint32_t buffer = 0;
    unsigned int pending = 0;
    for (const std::uint8_t byte : bytes) {
        buffer = (buffer << bits_per_byte) | std::uint32_t{byte};
        pending += bits_per_byte;
        while (pending >= bits_per_sextet) {
            pending -= bits_per_sextet;
            encoded += base64url_alphabet.at((buffer >> pending) & sextet_mask);
        }
    }
    if (pending > 0) {
        encoded += base64url_alphabet.at((buffer << (bits_per_sextet - pending)) & sextet_mask);
    }
    return encoded;
}

std::string percent_encode(std::string_view text) {
    std::string encoded;
    encoded.reserve(text.size());
    for (const char character : text) {
        if (is_unreserved(character)) {
            encoded += character;
            continue;
        }
        const unsigned int byte = static_cast<unsigned char>(character);
        encoded += '%';
        encoded += hex_digits.at(byte >> nibble_bits);
        encoded += hex_digits.at(byte & nibble_mask);
    }
    return encoded;
}

Result<std::string> pkce_challenge(std::string_view verifier) {
    const DigestContext context(EVP_MD_CTX_new());
    std::array<std::uint8_t, SHA256_DIGEST_LENGTH> digest{};
    unsigned int length = 0;
    if (context == nullptr || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(context.get(), verifier.data(), verifier.size()) != 1 ||
        EVP_DigestFinal_ex(context.get(), digest.data(), &length) != 1 || length != digest.size()) {
        return fail(Exit::config, "OpenSSL SHA-256 failed");
    }
    return base64url(digest);
}

Result<Pkce> new_pkce() {
    std::array<std::uint8_t, verifier_bytes> random{};
    if (RAND_bytes(random.data(), verifier_bytes) != 1) {
        return fail(Exit::config, "OpenSSL could not generate random bytes");
    }
    Pkce pkce{.verifier = base64url(random), .challenge = {}};
    OPENSSL_cleanse(random.data(), random.size());
    auto challenge = pkce_challenge(pkce.verifier);
    if (!challenge) {
        return std::unexpected(challenge.error());
    }
    pkce.challenge = std::move(*challenge);
    return pkce;
}

// No callback_url: OpenRouter's headless flow then shows the code on the page for the user to
// paste back (https://openrouter.ai/docs/guides/overview/auth/oauth, "Headless Apps").
std::string authorization_url(std::string_view challenge, std::string_view key_label) {
    return std::format("https://openrouter.ai/auth?code_challenge={}&code_challenge_method=S256"
                       "&key_label={}",
                       percent_encode(challenge), percent_encode(key_label));
}

std::string exchange_body(std::string_view code, std::string_view verifier) {
    const json body = json::object({{"code", std::string{code}},
                                    {"code_verifier", std::string{verifier}},
                                    {"code_challenge_method", "S256"}});
    // Replacement keeps dump() from throwing on a pasted code that is not valid UTF-8.
    return body.dump(compact_dump, ' ', false, json::error_handler_t::replace);
}

Result<SecretString> parse_exchange(std::string_view json_text) {
    json document = json::parse(json_text, nullptr, false);
    if (!document.is_object()) {
        return fail(Exit::api, std::string{exchange_error});
    }
    const auto found = document.find("key");
    if (found == document.end() || !found->is_string()) {
        return fail(Exit::api, std::string{exchange_error});
    }
    auto* const text = found->get_ptr<std::string*>();
    if (text->empty()) {
        return fail(Exit::api, std::string{exchange_error});
    }
    // Moving the string hands its buffer to the SecretString, so the parsed document keeps no
    // copy of the key.
    return SecretString(std::move(*text));
}

} // namespace orcli
