#include "terminal.hpp"

#include <doctest/doctest.h>

#include <array>
#include <string>

#include <unistd.h>

namespace {

class Descriptor {
public:
    explicit Descriptor(int descriptor) : descriptor_(descriptor) {}
    ~Descriptor() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&&) = delete;
    Descriptor& operator=(Descriptor&&) = delete;
    [[nodiscard]] int get() const { return descriptor_; }

private:
    int descriptor_;
};

// Both ends are owned from the moment pipe() returns them.
class Pipe {
public:
    Pipe() : Pipe(opened()) {}
    [[nodiscard]] int write_end() const { return write_.get(); }

private:
    explicit Pipe(std::array<int, 2> ends) : read_(ends[0]), write_(ends[1]) {}

    static std::array<int, 2> opened() {
        std::array<int, 2> ends{-1, -1};
        REQUIRE(::pipe(ends.data()) == 0);
        return ends;
    }

    Descriptor read_;
    Descriptor write_;
};

} // namespace

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

TEST_CASE("same_destination compares what two descriptors are open on") {
    const Pipe first;
    const Pipe second;
    const Descriptor copy(::dup(first.write_end()));
    REQUIRE(copy.get() >= 0);

    CHECK(orcli::same_destination(first.write_end(), copy.get()));
    CHECK(orcli::same_destination(first.write_end(), first.write_end()));
    CHECK_FALSE(orcli::same_destination(first.write_end(), second.write_end()));
    CHECK_FALSE(orcli::same_destination(first.write_end(), -1));
}
