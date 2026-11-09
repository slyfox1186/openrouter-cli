#include "help.hpp"

#include <CLI/CLI.hpp>
#include <doctest/doctest.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using orcli::ColorInputs;
using orcli::HelpContent;
using orcli::HelpExample;
using orcli::HelpFormatter;
using orcli::HelpStyle;

constexpr std::size_t kWide = 80;
constexpr std::size_t kNarrow = 40;
constexpr std::size_t kMiddle = 90;
constexpr std::size_t kTooNarrow = 20;
constexpr std::size_t kTooWide = 200;

struct Values {
    std::string model;
    std::string effort;
    std::string secret;
    std::string query;
    bool refresh = false;
    std::string name;
    std::string alias;
    std::string model_id;
};

// The shape of the real command line: question options and a query on the root, a command
// group with leaves, a plain leaf, and one command name too long for the left column.
struct Fixture {
    explicit Fixture(HelpStyle style) {
        app.formatter(std::make_shared<HelpFormatter>(
            HelpContent{
                .title = "prog 1.0",
                .examples = {HelpExample{.caption = "Ask a question",
                                         .commands = {R"(prog -m alias "A question")"}}},
                .notes = {"Options come first."},
            },
            style));
        app.set_help_flag("-h,--help", "Show this help and exit")->group("GENERAL");
        app.option_defaults()->group("QUESTION OPTIONS");
        app.add_option("-m,--model", values.model, "Model alias");
        app.add_option("--effort", values.effort,
                       "Reasoning effort with a long description that has to wrap across more "
                       "than one line at eighty columns")
            ->option_text("LEVEL");
        app.add_option("--secret", values.secret, "Never shown")->group("");
        app.add_option("query", values.query, "The question");
        app.option_defaults()->group("OPTIONS");
        models = app.add_subcommand("models", "Model commands");
        find = models->add_subcommand("find", "List models matching words");
        find->add_flag("--refresh", values.refresh, "Refetch first");
        find->add_option("name", values.name, "Words to match")->required();
        save = models->add_subcommand("save", "Save an alias");
        save->add_option("alias", values.alias, "Alias to save")->required();
        save->add_option("model_id", values.model_id, "Exact model id")->required();
        app.add_subcommand("status", "Who is logged in");
        app.add_subcommand("a-very-long-command-name-that-overflows", "Overflowing row");
    }

    Values values;
    CLI::App app{"A test program.", "prog"};
    CLI::App* models = nullptr;
    CLI::App* find = nullptr;
    CLI::App* save = nullptr;
};

bool has(std::string_view page, std::string_view piece) {
    return page.find(piece) != std::string_view::npos;
}

std::vector<std::string> lines_of(std::string_view page) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < page.size()) {
        const std::size_t end = page.find('\n', start);
        const std::size_t stop = end == std::string_view::npos ? page.size() : end;
        lines.emplace_back(page.substr(start, stop - start));
        start = stop + 1;
    }
    return lines;
}

// The first line that starts with prefix; empty when there is none.
std::string line_starting(std::string_view page, std::string_view prefix) {
    for (const std::string& line : lines_of(page)) {
        if (line.starts_with(prefix)) {
            return line;
        }
    }
    return {};
}

std::string strip_escapes(std::string_view text) {
    std::string plain;
    bool in_escape = false;
    for (const char character : text) {
        if (in_escape) {
            in_escape = character != 'm';
            continue;
        }
        if (character == '\x1b') {
            in_escape = true;
            continue;
        }
        plain += character;
    }
    return plain;
}

} // namespace

TEST_CASE("help color follows NO_COLOR, CLICOLOR_FORCE, the terminal and TERM") {
    CHECK(orcli::help_color({.no_color = "", .force = "", .term = "xterm", .terminal = true}));
    CHECK_FALSE(
        orcli::help_color({.no_color = "1", .force = "", .term = "xterm", .terminal = true}));
    CHECK_FALSE(
        orcli::help_color({.no_color = "1", .force = "1", .term = "xterm", .terminal = true}));
    CHECK(orcli::help_color({.no_color = "", .force = "1", .term = "", .terminal = false}));
    CHECK_FALSE(orcli::help_color({.no_color = "", .force = "0", .term = "", .terminal = false}));
    CHECK_FALSE(
        orcli::help_color({.no_color = "", .force = "", .term = "xterm", .terminal = false}));
    CHECK_FALSE(orcli::help_color({.no_color = "", .force = "", .term = "", .terminal = true}));
    CHECK_FALSE(orcli::help_color({.no_color = "", .force = "", .term = "dumb", .terminal = true}));
}

TEST_CASE("help width prefers the terminal, then COLUMNS, then 80, clamped to 40..100") {
    CHECK(orcli::help_width(std::nullopt, "") == kWide);
    CHECK(orcli::help_width(std::size_t{0}, "") == kWide);
    CHECK(orcli::help_width(std::size_t{0}, "90") == kMiddle);
    CHECK(orcli::help_width(kMiddle, "60") == kMiddle);
    CHECK(orcli::help_width(std::nullopt, "90") == kMiddle);
    CHECK(orcli::help_width(std::nullopt, "abc") == kWide);
    CHECK(orcli::help_width(std::nullopt, "0") == kWide);
    CHECK(orcli::help_width(std::nullopt, "-5") == kWide);
    CHECK(orcli::help_width(std::nullopt, "90x") == kWide);
    CHECK(orcli::help_width(kTooNarrow, "") == kNarrow);
    CHECK(orcli::help_width(kTooWide, "") == std::size_t{100});
}

TEST_CASE("the top-level page lists usage, aligned sections, leaf commands and examples") {
    const Fixture fixture(HelpStyle{.color = false, .width = kWide});
    const std::string page = fixture.app.help();

    CHECK_FALSE(has(page, "\x1b"));
    CHECK(page.starts_with("prog 1.0\nA test program.\n\nUSAGE\n  prog [OPTIONS] QUERY\n"
                           "  prog COMMAND\n"));
    CHECK(has(page, "\nARGUMENTS\n  QUERY "));
    CHECK(has(page, "\nQUESTION OPTIONS\n  -m, --model MODEL "));
    CHECK(has(page, "\n      --effort LEVEL "));
    CHECK_FALSE(has(page, "--secret"));
    CHECK(has(page, "\n  models find [OPTIONS] NAME "));
    CHECK(has(page, "\n  models save ALIAS MODEL_ID "));
    CHECK(has(page, "\n  status "));
    // A command that only groups others has no row of its own (it would be padded).
    CHECK_FALSE(has(page, "\n  models  "));
    CHECK(has(page, "\nGENERAL\n  -h, --help "));
    CHECK_FALSE(has(page, "HELP"));
    CHECK(page.find("COMMANDS") < page.find("GENERAL"));
    CHECK(has(page, "\nEXAMPLES\n  Ask a question\n    prog -m alias \"A question\"\n"));
    CHECK(has(page, "\nOptions come first.\n"));
    CHECK(page.ends_with("\nFor details on a command, run:\n  prog COMMAND --help\n"));

    for (const std::string& line : lines_of(page)) {
        CHECK(line.size() <= kWide);
    }

    // Every description on the page starts in one column.
    const std::size_t column = line_starting(page, "  QUERY").find("The question");
    CHECK(column != std::string::npos);
    CHECK(line_starting(page, "  -m, --model").find("Model alias") == column);
    CHECK(line_starting(page, "      --effort").find("Reasoning effort") == column);
    CHECK(line_starting(page, "  models find").find("List models") == column);
    CHECK(line_starting(page, "  -h, --help").find("Show this help") == column);

    // A wrapped description continues in the same column.
    const std::vector<std::string> lines = lines_of(page);
    for (std::size_t index = 0; index + 1 < lines.size(); ++index) {
        if (lines.at(index).starts_with("      --effort")) {
            const std::string& next = lines.at(index + 1);
            CHECK(next.find_first_not_of(' ') == column);
        }
    }

    // A left cell wider than the column cap puts its description on the next line.
    CHECK(has(page, "\n  a-very-long-command-name-that-overflows\n"));
    CHECK(line_starting(page, std::string(column, ' ') + "Overflowing row").size() ==
          column + std::string_view("Overflowing row").size());
}

TEST_CASE("color wraps finished pieces and leaves the plain layout unchanged") {
    const Fixture plain(HelpStyle{.color = false, .width = kWide});
    const Fixture colored(HelpStyle{.color = true, .width = kWide});
    const std::string plain_page = plain.app.help();
    const std::string colored_page = colored.app.help();

    CHECK(has(colored_page, "\x1b[1;32mUSAGE\x1b[0m"));
    CHECK(has(colored_page, "\x1b[1;36m-m\x1b[0m, \x1b[1;36m--model\x1b[0m \x1b[36mMODEL\x1b[0m"));
    CHECK(has(colored_page, "\x1b[1mprog 1.0\x1b[0m"));
    CHECK(has(colored_page, "\x1b[2mAsk a question\x1b[0m"));
    CHECK(strip_escapes(colored_page) == plain_page);
}

TEST_CASE("a leaf page shows its own usage, arguments and options") {
    const Fixture fixture(HelpStyle{.color = false, .width = kWide});
    const std::string page = fixture.find->help("prog models");

    CHECK(page.starts_with("prog models find\nList models matching words\n\nUSAGE\n"
                           "  prog models find [OPTIONS] NAME\n"));
    CHECK(has(page, "\nARGUMENTS\n  NAME "));
    CHECK(has(page, "\nOPTIONS\n"));
    CHECK(line_starting(page, "      --refresh").ends_with("Refetch first"));
    CHECK_FALSE(has(page, "REFRESH"));
    CHECK(has(page, "\n  -h, --help "));
    CHECK_FALSE(has(page, "EXAMPLES"));
    CHECK_FALSE(has(page, "Options come first."));
    CHECK_FALSE(has(page, "For details"));

    const std::string save_page = fixture.save->help("prog models");
    CHECK(has(save_page, "USAGE\n  prog models save ALIAS MODEL_ID\n"));
}

TEST_CASE("a command group page lists its leaves relative to itself") {
    const Fixture fixture(HelpStyle{.color = false, .width = kWide});
    const std::string page = fixture.models->help("prog");

    CHECK(page.starts_with("prog models\nModel commands\n\nUSAGE\n  prog models COMMAND\n\n"));
    CHECK(has(page, "\nCOMMANDS\n  find [OPTIONS] NAME "));
    CHECK(has(page, "\n  save ALIAS MODEL_ID "));
    CHECK(page.ends_with("\nFor details on a command, run:\n  prog models COMMAND --help\n"));
}

TEST_CASE("a narrow page stacks every description under its cell") {
    const Fixture fixture(HelpStyle{.color = false, .width = kNarrow});
    const std::string page = fixture.app.help();

    CHECK(has(page, "\n  -m, --model MODEL\n      Model alias\n"));
    CHECK(has(page, "\n  status\n      Who is logged in\n"));
    for (const std::string& line : lines_of(page)) {
        if (!line.starts_with("  a-very-long-command-name")) {
            CHECK(line.size() <= kNarrow);
        }
    }
}

TEST_CASE("a null app renders nothing") {
    const HelpFormatter formatter(HelpContent{}, HelpStyle{});
    CHECK(formatter.make_help(nullptr, "prog", CLI::AppFormatMode::Normal).empty());
}
