#ifndef ORCLI_TERMINAL_HPP
#define ORCLI_TERMINAL_HPP

#include <cstdlib>
#include <format>
#include <string>
#include <string_view>

#include <sys/stat.h>
#include <unistd.h>

// Header-only because the core library and the help library both color text and neither links
// the other.

namespace orcli {

struct ColorInputs {
    std::string_view no_color; // NO_COLOR
    std::string_view force;    // CLICOLOR_FORCE
    std::string_view term;     // TERM
    bool terminal = false;     // the stream is a terminal
};

// A non-empty NO_COLOR turns color off (no-color.org). Otherwise CLICOLOR_FORCE other than
// empty or "0" turns it on, so pagers such as `less -R` can opt in. Otherwise color needs a
// terminal whose TERM is set and is not "dumb".
[[nodiscard]] inline bool color_enabled(const ColorInputs& inputs) {
    if (!inputs.no_color.empty()) {
        return false;
    }
    if (!inputs.force.empty() && inputs.force != "0") {
        return true;
    }
    return inputs.terminal && !inputs.term.empty() && inputs.term != "dumb";
}

// An environment variable's value; empty when it is unset.
[[nodiscard]] inline std::string_view environment(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string_view{} : std::string_view{value};
}

// color_enabled applied to the environment and one stream, so stdout and stderr are decided
// separately when only one of them is redirected.
[[nodiscard]] inline bool stream_color(int descriptor) {
    return color_enabled(ColorInputs{
        .no_color = environment("NO_COLOR"),
        .force = environment("CLICOLOR_FORCE"),
        .term = environment("TERM"),
        .terminal = ::isatty(descriptor) == 1,
    });
}

// True when text written to one descriptor lands among text written to the other: both are
// open on the same pipe or file, or both are terminals. Two terminals count without comparing
// them because one terminal has several names (/dev/tty, /dev/pts/N) with different inodes.
[[nodiscard]] inline bool same_destination(int first, int second) noexcept {
    if (::isatty(first) == 1 && ::isatty(second) == 1) {
        return true;
    }
    struct stat first_info{};
    struct stat second_info{};
    if (::fstat(first, &first_info) != 0 || ::fstat(second, &second_info) != 0) {
        return false;
    }
    return first_info.st_dev == second_info.st_dev && first_info.st_ino == second_info.st_ino;
}

// The text inside one SGR sequence (codes from the terminal's own 16-color palette, so they
// follow its light or dark theme), or the text unchanged when color is off. The text must
// already be free of terminal controls.
[[nodiscard]] inline std::string paint(std::string_view sgr, std::string_view text, bool color) {
    if (!color || sgr.empty() || text.empty()) {
        return std::string(text);
    }
    return std::format("\x1b[{}m{}\x1b[0m", sgr, text);
}

} // namespace orcli

#endif // ORCLI_TERMINAL_HPP
