#include "auth.hpp"

#include "common.hpp"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using orcli::Exit;

namespace {

constexpr std::size_t verifier_length = 86;
constexpr std::size_t challenge_length = 43;
constexpr std::string_view base64url_characters =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr std::string_view exchange_error = "unexpected response from POST /auth/keys";
constexpr std::uint8_t byte_fb = 0xFB;
constexpr std::uint8_t byte_ff = 0xFF;

std::vector<std::uint8_t> bytes_of(std::string_view text) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size());
    for (const char character : text) {
        bytes.push_back(static_cast<std::uint8_t>(character));
    }
    return bytes;
}

std::string encode(std::string_view text) {
    return orcli::base64url(bytes_of(text));
}

} // namespace

TEST_CASE("base64url matches the RFC 4648 test vectors without padding") {
    CHECK(encode("").empty());
    CHECK(encode("f") == "Zg");
    CHECK(encode("fo") == "Zm8");
    CHECK(encode("foo") == "Zm9v");
    CHECK(encode("foob") == "Zm9vYg");
    CHECK(encode("fooba") == "Zm9vYmE");
    CHECK(encode("foobar") == "Zm9vYmFy");
    const std::vector<std::uint8_t> url_safe{byte_fb, byte_ff};
    CHECK(orcli::base64url(url_safe) == "-_8");
}

TEST_CASE("pkce_challenge matches RFC 7636 Appendix B") {
    const auto challenge = orcli::pkce_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk");
    REQUIRE(challenge.has_value());
    CHECK(*challenge == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
}

TEST_CASE("new_pkce makes a fresh 86-character verifier and its S256 challenge") {
    const auto first = orcli::new_pkce();
    REQUIRE(first.has_value());
    CHECK(first->verifier.size() == verifier_length);
    CHECK(first->verifier.find_first_not_of(base64url_characters) == std::string::npos);
    CHECK(first->challenge.size() == challenge_length);
    const auto expected = orcli::pkce_challenge(first->verifier);
    REQUIRE(expected.has_value());
    CHECK(first->challenge == *expected);

    const auto second = orcli::new_pkce();
    REQUIRE(second.has_value());
    CHECK(second->verifier != first->verifier);
    CHECK(second->challenge != first->challenge);
}

TEST_CASE("percent_encode keeps unreserved characters and encodes every other byte") {
    CHECK(orcli::percent_encode("AZaz09-._~") == "AZaz09-._~");
    CHECK(orcli::percent_encode("openrouter-cli on box") == "openrouter-cli%20on%20box");
    CHECK(orcli::percent_encode("a&b=c/d?e+f") == "a%26b%3Dc%2Fd%3Fe%2Bf");
    CHECK(orcli::percent_encode("caf\xC3\xA9") == "caf%C3%A9");
    CHECK(orcli::percent_encode("").empty());
}

TEST_CASE("authorization_url carries the challenge and label but no callback_url") {
    const std::string url = orcli::authorization_url("abc-_123", "openrouter-cli on box");
    CHECK(url == "https://openrouter.ai/auth?code_challenge=abc-_123&code_challenge_method=S256"
                 "&key_label=openrouter-cli%20on%20box");
    CHECK(url.find("callback_url") == std::string::npos);
}

TEST_CASE("exchange_body holds the code, verifier and S256 method") {
    const auto body = nlohmann::json::parse(orcli::exchange_body("the-code", "the-verifier"));
    REQUIRE(body.is_object());
    CHECK(body.size() == 3);
    CHECK(body.at("code") == "the-code");
    CHECK(body.at("code_verifier") == "the-verifier");
    CHECK(body.at("code_challenge_method") == "S256");
}

TEST_CASE("parse_exchange returns the key") {
    const auto key = orcli::parse_exchange(R"({"key": "sk-or-v1-abc", "user_id": "user_1"})");
    REQUIRE(key.has_value());
    CHECK(key->view() == "sk-or-v1-abc");
}

TEST_CASE("parse_exchange rejects responses without a usable key and never echoes them") {
    for (const std::string_view text :
         {R"({"user_id": "user_1"})", R"({"key": 5})", R"({"key": ""})", R"({"key": null})",
          R"({"key": "sk-or-v1-secret")", R"(["sk-or-v1-secret"])", ""}) {
        CAPTURE(text);
        const auto key = orcli::parse_exchange(text);
        REQUIRE_FALSE(key.has_value());
        CHECK(key.error().code == Exit::api);
        CHECK(key.error().message == exchange_error);
    }
}
