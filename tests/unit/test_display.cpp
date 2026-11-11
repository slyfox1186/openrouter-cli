#include "display.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <string>

TEST_CASE("sanitize_for_display leaves credential-shaped text unchanged") {
    constexpr std::size_t kKeyBody = 64;
    const std::string key = "sk-or-v1-" + std::string(kKeyBody, 'a');
    CHECK(orcli::sanitize_for_display("bad key " + key + " rejected", std::string::npos) ==
          "bad key " + key + " rejected");
    CHECK(orcli::sanitize_for_display("password=\"abcdefghijklmnop\"", std::string::npos) ==
          "password=\"abcdefghijklmnop\"");
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

TEST_CASE("display metadata neutralizes terminal controls and preserves Unicode") {
    CHECK(orcli::display_metadata("a\x1b[31m\n\t\x7f") == "a [31m   ");
    CHECK(orcli::display_metadata("caf\xc3\xa9 \xe2\x82\xac") == "caf\xc3\xa9 \xe2\x82\xac");
    CHECK(orcli::display_metadata("a\xc2\x9b"
                                  "b\x9b"
                                  "c") == "a b c");
}

TEST_CASE("display metadata rejects overlong control encodings") {
    CHECK(orcli::display_metadata("\xe0\x82\x9b") == "   ");
}
