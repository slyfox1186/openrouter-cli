#include "secrets.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace {

using orcli::SecretFinding;
using Patterns = std::vector<std::string>;

// Fake credential bodies are generated at run time so this file never holds text that the
// scanner itself would flag.
constexpr std::string_view kHexChars = "0123456789abcdef";
constexpr std::string_view kTokenChars = "aB3dE5_gH7-jK9mN1p";
constexpr std::string_view kWordChars = "aB3dE5_gH7jK9mN1p";
constexpr std::string_view kAlnumChars = "aB3dE5gH7jK9mN1pQ2";
constexpr std::string_view kUpperChars = "Q7W3E9R2T5Y1U8I4O6";
constexpr std::string_view kAlphaChars = "abcXYZdefUVW";

constexpr std::size_t kOpenRouterBody = 64;
constexpr std::size_t kVendorBody = 24;
constexpr std::size_t kAwsBody = 16;
constexpr std::size_t kGithubBody = 36;
constexpr std::size_t kGithubPatBody = 60;
constexpr std::size_t kGoogleBody = 35;
constexpr std::size_t kHuggingFaceBody = 34;
constexpr std::size_t kJwtSegment = 20;
constexpr std::size_t kSecretValue = 16;
constexpr std::size_t kShortBody = 8;

std::string cycle(std::string_view alphabet, std::size_t length) {
    std::string out;
    out.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
        out += alphabet.at(i % alphabet.size());
    }
    return out;
}

Patterns patterns_in(std::string_view text) {
    return orcli::scan_text(text, "test") | std::views::transform(&SecretFinding::pattern) |
           std::ranges::to<Patterns>();
}

std::string private_key_header() {
    return "-----BEGIN RSA " + std::string("PRIVATE") + " KEY-----";
}

std::string fake_jwt() {
    return "eyJ" + cycle(kTokenChars, kJwtSegment) + ".eyJ" + cycle(kTokenChars, kJwtSegment) +
           "." + cycle(kTokenChars, kJwtSegment);
}

} // namespace

TEST_CASE("vendor key formats are detected") {
    CHECK(patterns_in("key sk-or-v1-" + cycle(kHexChars, kOpenRouterBody)) ==
          Patterns{"openrouter-key"});
    CHECK(patterns_in("sk-ant-api03-" + cycle(kTokenChars, kVendorBody)) ==
          Patterns{"anthropic-key"});
    CHECK(patterns_in("sk-proj-" + cycle(kTokenChars, kVendorBody)) == Patterns{"openai-key"});
    CHECK(patterns_in("sk-svcacct-" + cycle(kTokenChars, kVendorBody)) == Patterns{"openai-key"});
    CHECK(patterns_in("id = AKIA" + cycle(kUpperChars, kAwsBody)) == Patterns{"aws-access-key"});
    CHECK(patterns_in("ASIA" + cycle(kUpperChars, kAwsBody) + ",") == Patterns{"aws-access-key"});
    CHECK(patterns_in("ghp_" + cycle(kAlnumChars, kGithubBody)) == Patterns{"github-token"});
    CHECK(patterns_in("github_pat_" + cycle(kWordChars, kGithubPatBody)) ==
          Patterns{"github-token"});
    CHECK(patterns_in("glpat-" + cycle(kTokenChars, kVendorBody)) == Patterns{"gitlab-token"});
    CHECK(patterns_in("xoxb-" + cycle(kAlnumChars, kVendorBody) + "-" +
                      cycle(kAlnumChars, kShortBody)) == Patterns{"slack-token"});
    CHECK(patterns_in("key=AIza" + cycle(kTokenChars, kGoogleBody)) == Patterns{"google-api-key"});
    CHECK(patterns_in("sk_live_" + cycle(kAlnumChars, kVendorBody)) == Patterns{"stripe-key"});
    CHECK(patterns_in("rk_live_" + cycle(kAlnumChars, kVendorBody)) == Patterns{"stripe-key"});
    CHECK(patterns_in("hf_" + cycle(kAlphaChars, kHuggingFaceBody)) ==
          Patterns{"huggingface-token"});
}

TEST_CASE("near-miss vendor formats are not detected") {
    CHECK(patterns_in("sk-or-v1-" + cycle(kHexChars, kShortBody)).empty());
    CHECK(patterns_in("sk-ant-" + cycle(kTokenChars, kShortBody)).empty());
    CHECK(patterns_in("sk-proj-" + cycle(kTokenChars, kShortBody)).empty());
    CHECK(patterns_in("AKIA" + cycle(kUpperChars, kAwsBody + 1)).empty());
    CHECK(patterns_in("XAKIA" + cycle(kUpperChars, kAwsBody)).empty());
    CHECK(patterns_in("ghp_" + cycle(kAlnumChars, kShortBody)).empty());
    CHECK(patterns_in("glpat-short").empty());
    CHECK(patterns_in("xoxb-12").empty());
    CHECK(patterns_in("AIza" + cycle(kTokenChars, kShortBody)).empty());
    CHECK(patterns_in("sk_test_" + cycle(kAlnumChars, kVendorBody)).empty());
    CHECK(patterns_in("hf_" + cycle(kAlphaChars, kShortBody)).empty());
    CHECK(patterns_in("const char* prefix = \"sk-or-v1-\";").empty());
}

TEST_CASE("private key blocks and JWTs are detected") {
    CHECK(patterns_in(private_key_header()) == Patterns{"private-key-block"});
    CHECK(patterns_in("-----BEGIN " + std::string("PRIVATE") + " KEY-----") ==
          Patterns{"private-key-block"});
    CHECK(patterns_in("-----BEGIN PUBLIC KEY-----").empty());
    CHECK(patterns_in("-----BEGIN CERTIFICATE-----").empty());
    CHECK(patterns_in("Authorization: Bearer " + fake_jwt()) == Patterns{"jwt"});
    CHECK(patterns_in("eyJ" + cycle(kTokenChars, kJwtSegment)).empty());
}

TEST_CASE("URLs with embedded passwords are detected") {
    CHECK(patterns_in("postgres://admin:" + cycle(kTokenChars, kSecretValue) +
                      "@db.internal:5432/app") == Patterns{"url-credentials"});
    CHECK(patterns_in("redis://:" + cycle(kAlnumChars, kSecretValue) + "@cache:6379") ==
          Patterns{"url-credentials"});
    CHECK(patterns_in("https://user:${DB_PASSWORD}@db.local/app").empty());
    CHECK(patterns_in("https://user:<password>@db.local/app").empty());
    CHECK(patterns_in("redis://:****@cache:6379").empty());
    CHECK(patterns_in("https://user:password@db.local/app").empty());
    CHECK(patterns_in("https://example.com:8080/path").empty());
    CHECK(patterns_in("https://user@example.com/path").empty());
}

TEST_CASE("credential assignments are detected") {
    const std::string value = cycle(kAlnumChars, kSecretValue);
    CHECK(patterns_in("db_password = \"" + value + "\"") == Patterns{"credential-assignment"});
    CHECK(patterns_in("{\"api_key\": \"" + value + "\"}") == Patterns{"credential-assignment"});
    CHECK(patterns_in("'secret' => '" + value + "',") == Patterns{"credential-assignment"});
    CHECK(patterns_in("AUTH_TOKEN:=`" + value + "`") == Patterns{"credential-assignment"});
    CHECK(patterns_in("DB_PASSWORD=" + value) == Patterns{"credential-assignment"});
    CHECK(patterns_in("  export OPENAI_APIKEY=" + value + "  # rotated") ==
          Patterns{"credential-assignment"});
}

TEST_CASE("ordinary code near credential names is not detected") {
    const std::string value = cycle(kAlnumChars, kSecretValue);
    CHECK(patterns_in("password = getenv(\"X\")").empty());
    CHECK(patterns_in("api_key=${API_KEY}").empty());
    CHECK(patterns_in("token = \"<your token>\"").empty());
    CHECK(patterns_in("password = \"changeme_changeme\"").empty());
    CHECK(patterns_in("secret = \"" + std::string(kSecretValue, 'x') + "\"").empty());
    CHECK(patterns_in("api_key = os.environ[\"OPENROUTER_API_KEY\"]").empty());
    CHECK(patterns_in("api_key=os.environ[\"OPENROUTER_API_KEY_VALUE\"]").empty());
    CHECK(patterns_in("if (password == \"" + value + "\") {").empty());
    CHECK(patterns_in("PASSWORD_PROMPT = \"Enter the password for this account\"").empty());
    CHECK(patterns_in("password = \"short\"").empty());
    CHECK(patterns_in("DB_PASSWORD=short").empty());
    CHECK(patterns_in("run --password=" + value).empty());
    CHECK(patterns_in("API_KEY=your_api_key_goes_here").empty());
}

TEST_CASE("findings carry the source and 1-based line, once per line and pattern") {
    const std::string key = "sk-or-v1-" + cycle(kHexChars, kOpenRouterBody);
    const std::string aws = "AKIA" + cycle(kUpperChars, kAwsBody);
    const std::string text = "first line\n" + key + " and " + key + "\n" + aws + "\r\n" + aws +
                             " " + key + "\nlast line";
    const std::vector<SecretFinding> findings = orcli::scan_text(text, "query");
    REQUIRE(findings.size() == 4);
    CHECK(findings.at(0).source == "query");
    CHECK(findings.at(0).line == 2);
    CHECK(findings.at(0).pattern == "openrouter-key");
    CHECK(findings.at(1).line == 3);
    CHECK(findings.at(1).pattern == "aws-access-key");
    CHECK(findings.at(2).line == 4);
    CHECK(findings.at(3).line == 4);
    CHECK(orcli::scan_text("no secrets here\n", "system").empty());
}

TEST_CASE("credential paths are refused by name and location") {
    using orcli::credential_path_reason;
    CHECK_FALSE(credential_path_reason("/srv/app/.env.example").has_value());
    CHECK_FALSE(credential_path_reason("/srv/app/.env.sample").has_value());
    CHECK_FALSE(credential_path_reason("/srv/app/.env.template").has_value());
    CHECK_FALSE(credential_path_reason("/srv/app/src/main.cpp").has_value());
    CHECK_FALSE(credential_path_reason("/srv/keys/id_ed25519.pub").has_value());
    CHECK_FALSE(credential_path_reason("/srv/app/.config/settings.json").has_value());
    CHECK(credential_path_reason("/srv/app/.env").has_value());
    CHECK(credential_path_reason("/srv/app/.env.local").has_value());
    CHECK(credential_path_reason("/srv/keys/id_ed25519").has_value());
    CHECK(credential_path_reason("/srv/keys/id_rsa_work").has_value());
    CHECK(credential_path_reason("/srv/tls/server.key").has_value());
    CHECK(credential_path_reason("/srv/tls/Server.PEM").has_value());
    CHECK(credential_path_reason("/srv/app/service-account-prod.json").has_value());
    CHECK(credential_path_reason("/home/someone/.netrc").has_value());
    CHECK(credential_path_reason("/home/someone/.ssh/config").has_value());
    CHECK(credential_path_reason("/home/someone/.aws/config").has_value());
    CHECK(credential_path_reason("/home/someone/.config/gcloud/access_tokens.db").has_value());
    CHECK(credential_path_reason("/home/someone/.config/openrouter/key").has_value());
    CHECK(credential_path_reason("/home/someone/.docker/config.json").has_value());
    CHECK(credential_path_reason("/home/someone/.kube/config").has_value());
}

TEST_CASE("sanitize_for_display redacts secret values") {
    const std::string key = "sk-or-v1-" + cycle(kHexChars, kOpenRouterBody);
    CHECK(orcli::sanitize_for_display("bad key " + key + " rejected", std::string::npos) ==
          "bad key [redacted] rejected");
    const std::string value = cycle(kAlnumChars, kSecretValue);
    CHECK(orcli::sanitize_for_display("password=\"" + value + "\"", std::string::npos) ==
          "password=\"[redacted]\"");
    CHECK(orcli::sanitize_for_display("x\n" + private_key_header() +
                                          "\nMIIEow\n-----END RSA PRIVATE KEY-----\ny",
                                      std::string::npos) == "x\n[redacted]\ny");
}

TEST_CASE("sanitize_for_display replaces control characters") {
    CHECK(orcli::sanitize_for_display("a\x1b[31mb\rc\nd\te\x7f", std::string::npos) ==
          "a [31mb c\nd\te ");
}

TEST_CASE("sanitize_for_display truncates without splitting UTF-8") {
    constexpr std::size_t kMaxChars = 4;
    CHECK(orcli::sanitize_for_display("abcdefgh", kMaxChars) == "abcd...");
    CHECK(orcli::sanitize_for_display("abcd", kMaxChars) == "abcd");
    const std::string euro_text = "a\xE2\x82\xAC"
                                  "b";
    CHECK(orcli::sanitize_for_display(euro_text, 2) == "a...");
    CHECK(orcli::sanitize_for_display(euro_text, kMaxChars) == "a\xE2\x82\xAC...");
    const std::string accent_text = "ab\xC3\xA9"
                                    "cd";
    CHECK(orcli::sanitize_for_display(accent_text, 3) == "ab...");
}
