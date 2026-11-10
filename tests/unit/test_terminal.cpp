#include "terminal.hpp"

#include <doctest/doctest.h>

#include <string>

TEST_CASE("color follows NO_COLOR, CLICOLOR_FORCE, the terminal and TERM") {
    CHECK(orcli::color_enabled({.no_color = "", .force = "", .term = "xterm", .terminal = true}));
    CHECK_FALSE(
        orcli::color_enabled({.no_color = "1", .force = "", .term = "xterm", .terminal = true}));
    CHECK_FALSE(
        orcli::color_enabled({.no_color = "1", .force = "1", .term = "xterm", .terminal = true}));
    CHECK(orcli::color_enabled({.no_color = "", .force = "1", .term = "", .terminal = false}));
    CHECK_FALSE(
        orcli::color_enabled({.no_color = "", .force = "0", .term = "", .terminal = false}));
    CHECK_FALSE(
        orcli::color_enabled({.no_color = "", .force = "", .term = "xterm", .terminal = false}));
    CHECK_FALSE(orcli::color_enabled({.no_color = "", .force = "", .term = "", .terminal = true}));
    CHECK_FALSE(
        orcli::color_enabled({.no_color = "", .force = "", .term = "dumb", .terminal = true}));
}

TEST_CASE("paint wraps text in one SGR sequence only when color is on") {
    CHECK(orcli::paint("1;32", "done", true) == "\x1b[1;32mdone\x1b[0m");
    CHECK(orcli::paint("1;32", "done", false) == "done");
    CHECK(orcli::paint("", "done", true) == "done");
    CHECK(orcli::paint("1", "", true).empty());
}
