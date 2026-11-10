#include "help.hpp"

#include "terminal.hpp"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// TIOCGWINSZ is defined in the kernel header; <sys/ioctl.h> reaches it only indirectly.
#include <asm-generic/ioctls.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace orcli {
namespace {

constexpr std::size_t min_help_width = 40;
constexpr std::size_t max_help_width = 100;
constexpr std::size_t page_indent = 2;
constexpr std::size_t example_indent = 4;
constexpr std::size_t stacked_indent = 6;
constexpr std::size_t column_gap = 2;
// A wider left cell moves its description to the next line instead of widening the column for
// every row on the page.
constexpr std::size_t max_left_cell = 30;
// Narrower description columns wrap into a ragged strip, so rows stack instead.
constexpr std::size_t min_description_room = 24;
constexpr std::size_t min_wrap_width = 10;
// Long names of options without a short name line up under those that have one ("-m, ").
constexpr std::string_view short_name_slot = "    ";

enum class Tone : std::uint8_t { plain, title, header, literal, placeholder, caption };

// SGR codes from the terminal's own 16-color palette, so they follow its light or dark theme.
std::string_view sgr_code(Tone tone) {
    switch (tone) {
    case Tone::title:
        return "1";
    case Tone::header:
        return "1;32";
    case Tone::literal:
        return "1;36";
    case Tone::placeholder:
        return "36";
    case Tone::caption:
        return "2";
    case Tone::plain:
        break;
    }
    return {};
}

// One output line. Its width counts only visible characters, so padding and wrapping come out
// the same with and without color.
class Line {
public:
    explicit Line(bool color) : color_(color) {}

    Line& add(std::string_view text, Tone tone = Tone::plain) {
        text_ += paint(sgr_code(tone), text, color_);
        width_ += text.size();
        return *this;
    }

    Line& append(const Line& other) {
        text_ += other.text_;
        width_ += other.width_;
        return *this;
    }

    Line& pad_to(std::size_t column) {
        if (width_ < column) {
            text_.append(column - width_, ' ');
            width_ = column;
        }
        return *this;
    }

    [[nodiscard]] std::size_t width() const { return width_; }
    [[nodiscard]] const std::string& text() const { return text_; }

private:
    bool color_ = false;
    std::string text_;
    std::size_t width_ = 0;
};

struct Row {
    Line left;
    std::string description;
};

struct Section {
    std::string title;
    std::vector<Row> rows;
};

struct Margin {
    std::size_t indent = 0;
    std::size_t width = 0;
};

// Where descriptions start on a page; stacked pages put every description under its cell.
struct Geometry {
    std::size_t width = 0;
    std::size_t column = 0;
    bool stacked = false;
};

std::size_t description_room(const Geometry& geometry) {
    return std::max(geometry.width > geometry.column ? geometry.width - geometry.column
                                                     : std::size_t{0},
                    min_wrap_width);
}

void emit(std::string& out, const Line& line) {
    out += line.text();
    out += '\n';
}

// Greedy word wrap; a word longer than the width gets a line of its own.
std::vector<std::string> wrap(std::string_view text, std::size_t width) {
    std::vector<std::string> lines;
    std::string current;
    for (const auto part : std::views::split(text, ' ')) {
        const std::string_view word(part.begin(), part.end());
        if (word.empty()) {
            continue;
        }
        if (!current.empty() && current.size() + 1 + word.size() > width) {
            lines.push_back(std::move(current));
            current.clear();
        }
        if (!current.empty()) {
            current += ' ';
        }
        current += word;
    }
    if (!current.empty()) {
        lines.push_back(std::move(current));
    }
    return lines;
}

void render_paragraph(std::string& out, std::string_view text, Margin margin, Tone tone,
                      bool color) {
    const std::size_t room =
        std::max(margin.width > margin.indent ? margin.width - margin.indent : std::size_t{0},
                 min_wrap_width);
    for (const std::string& piece : wrap(text, room)) {
        Line line(color);
        line.pad_to(margin.indent).add(piece, tone);
        emit(out, line);
    }
}

void render_header(std::string& out, std::string_view title, bool color) {
    Line line(color);
    line.add(title, Tone::header);
    emit(out, line);
}

std::string upper(std::string_view text) {
    std::string result(text);
    for (char& character : result) {
        if (character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 'A');
        }
    }
    return result;
}

std::string placeholder_name(std::string_view name) {
    std::string result = upper(name);
    std::ranges::replace(result, '-', '_');
    return result;
}

// CLI11 keeps a type size of 1 on flags and records "no value" in the expected count, so the
// expected count is what tells a flag from an option that takes a value.
bool takes_value(const CLI::Option& option) {
    return option.get_items_expected_max() > 0;
}

std::string placeholder(const CLI::Option& option) {
    if (!option.get_option_text().empty()) {
        return option.get_option_text();
    }
    if (!option.get_lnames().empty()) {
        return placeholder_name(option.get_lnames().front());
    }
    if (!option.get_snames().empty()) {
        return placeholder_name(option.get_snames().front());
    }
    return placeholder_name(option.get_name());
}

std::string positional_name(const CLI::Option& option) {
    return placeholder_name(option.get_name(true));
}

// An empty group hides an option from help in CLI11; this formatter keeps that convention.
std::vector<const CLI::Option*> positionals_of(const CLI::App& app) {
    return app.get_options([](const CLI::Option* option) {
        return option->get_positional() && !option->get_group().empty();
    });
}

bool has_options(const CLI::App& app) {
    const CLI::Option* help = app.get_help_ptr();
    const std::vector<const CLI::Option*> options =
        app.get_options([help](const CLI::Option* option) {
            return option != help && option->nonpositional() && !option->get_group().empty();
        });
    return !options.empty();
}

std::vector<const CLI::App*> listed_commands(const CLI::App& app) {
    return app.get_subcommands([](const CLI::App* command) {
        return !command->get_disabled() && !command->get_name().empty() &&
               !command->get_group().empty();
    });
}

// The command path, then [OPTIONS] and positional placeholders. Every positional in this
// program is required in the form it belongs to, so none is bracketed.
Line command_cell(const CLI::App& command, std::string_view path, bool color) {
    Line cell(color);
    cell.add(path, Tone::literal);
    if (has_options(command)) {
        cell.add(" ").add("[OPTIONS]", Tone::placeholder);
    }
    for (const CLI::Option* positional : positionals_of(command)) {
        cell.add(" ").add(positional_name(*positional), Tone::placeholder);
    }
    return cell;
}

Line option_cell(const CLI::Option& option, bool color) {
    Line cell(color);
    bool first = true;
    for (const std::string& name : option.get_snames()) {
        if (!first) {
            cell.add(", ");
        }
        cell.add(std::format("-{}", name), Tone::literal);
        first = false;
    }
    if (first) {
        cell.add(short_name_slot);
    }
    for (const std::string& name : option.get_lnames()) {
        if (!first) {
            cell.add(", ");
        }
        cell.add(std::format("--{}", name), Tone::literal);
        first = false;
    }
    if (takes_value(option)) {
        cell.add(" ").add(placeholder(option), Tone::placeholder);
    }
    return cell;
}

// Leaf commands in definition order, each with its path relative to the page; commands that
// only group others get no row. A work stack replaces recursion (misc-no-recursion).
std::vector<Row> command_rows(const CLI::App& app, bool color) {
    struct Pending {
        const CLI::App* command = nullptr;
        std::string path;
    };
    std::vector<Pending> stack;
    const auto push_children = [&stack](const CLI::App& parent, const std::string& prefix) {
        const std::vector<const CLI::App*> children = listed_commands(parent);
        for (const CLI::App* child : children | std::views::reverse) {
            stack.push_back(Pending{
                .command = child,
                .path = prefix.empty() ? child->get_name()
                                       : std::format("{} {}", prefix, child->get_name()),
            });
        }
    };
    push_children(app, "");
    std::vector<Row> rows;
    while (!stack.empty()) {
        const Pending next = std::move(stack.back());
        stack.pop_back();
        if (!listed_commands(*next.command).empty()) {
            push_children(*next.command, next.path);
            continue;
        }
        rows.push_back(Row{
            .left = command_cell(*next.command, next.path, color),
            .description = next.command->get_description(),
        });
    }
    return rows;
}

Section group_section(const CLI::App& app, const std::string& group, bool color) {
    Section section{.title = upper(group), .rows = {}};
    const std::vector<const CLI::Option*> options =
        app.get_options([&group](const CLI::Option* option) {
            return option->nonpositional() && option->get_group() == group;
        });
    for (const CLI::Option* option : options) {
        section.rows.push_back(Row{
            .left = option_cell(*option, color),
            .description = option->get_description(),
        });
    }
    return section;
}

// Arguments, the option groups, the commands, then the group holding the help flag, so the
// options that apply everywhere come last.
std::vector<Section> sections_of(const CLI::App& app, bool color) {
    std::vector<Section> sections;
    Section arguments{.title = "ARGUMENTS", .rows = {}};
    for (const CLI::Option* positional : positionals_of(app)) {
        Line cell(color);
        cell.add(positional_name(*positional), Tone::placeholder);
        arguments.rows.push_back(Row{
            .left = std::move(cell),
            .description = positional->get_description(),
        });
    }
    sections.push_back(std::move(arguments));

    const CLI::Option* help = app.get_help_ptr();
    const std::string help_group = help == nullptr ? std::string{} : help->get_group();
    for (const std::string& group : app.get_groups()) {
        if (!group.empty() && group != help_group) {
            sections.push_back(group_section(app, group, color));
        }
    }
    sections.push_back(Section{.title = "COMMANDS", .rows = command_rows(app, color)});
    if (!help_group.empty()) {
        sections.push_back(group_section(app, help_group, color));
    }
    std::erase_if(sections, [](const Section& section) { return section.rows.empty(); });
    return sections;
}

Geometry geometry_of(const std::vector<Section>& sections, std::size_t width) {
    std::size_t widest = 0;
    for (const Section& section : sections) {
        for (const Row& row : section.rows) {
            if (row.left.width() <= max_left_cell) {
                widest = std::max(widest, row.left.width());
            }
        }
    }
    const std::size_t column = page_indent + widest + column_gap;
    if (width < column + min_description_room) {
        return Geometry{.width = width, .column = stacked_indent, .stacked = true};
    }
    return Geometry{.width = width, .column = column, .stacked = false};
}

void render_row(std::string& out, const Row& row, const Geometry& geometry, bool color) {
    Line line(color);
    line.pad_to(page_indent).append(row.left);
    const std::vector<std::string> pieces = wrap(row.description, description_room(geometry));
    std::span<const std::string> remaining(pieces);
    const bool beside = !geometry.stacked && line.width() + column_gap <= geometry.column;
    if (beside && !remaining.empty()) {
        line.pad_to(geometry.column).add(remaining.front());
        remaining = remaining.subspan(1);
    }
    emit(out, line);
    for (const std::string& piece : remaining) {
        Line more(color);
        more.pad_to(geometry.column).add(piece);
        emit(out, more);
    }
}

void render_usage(std::string& out, const CLI::App& app, std::string_view path, bool color) {
    render_header(out, "USAGE", color);
    const bool has_commands = !listed_commands(app).empty();
    if (!positionals_of(app).empty() || !has_commands) {
        Line line(color);
        line.pad_to(page_indent).append(command_cell(app, path, color));
        emit(out, line);
    }
    if (has_commands) {
        Line line(color);
        line.pad_to(page_indent)
            .add(path, Tone::literal)
            .add(" ")
            .add("COMMAND", Tone::placeholder);
        emit(out, line);
    }
}

void render_examples(std::string& out, const std::vector<HelpExample>& examples,
                     const HelpStyle& style) {
    if (examples.empty()) {
        return;
    }
    out += '\n';
    render_header(out, "EXAMPLES", style.color);
    for (const HelpExample& example : examples) {
        render_paragraph(out, example.caption, Margin{.indent = page_indent, .width = style.width},
                         Tone::caption, style.color);
        for (const std::string& command : example.commands) {
            Line line(style.color);
            line.pad_to(example_indent).add(command);
            emit(out, line);
        }
    }
}

void render_command_hint(std::string& out, std::string_view path, const HelpStyle& style) {
    out += '\n';
    const Margin page{.indent = 0, .width = style.width};
    render_paragraph(out, "For details on a command, run:", page, Tone::plain, style.color);
    Line line(style.color);
    line.pad_to(page_indent)
        .add(path, Tone::literal)
        .add(" ")
        .add("COMMAND", Tone::placeholder)
        .add(" ")
        .add("--help", Tone::literal);
    emit(out, line);
}

std::optional<std::size_t> read_terminal_columns() {
    winsize size{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0) {
        return std::nullopt;
    }
    return std::size_t{size.ws_col};
}

std::optional<std::size_t> positive_number(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    const char* const text_end = std::to_address(text.end());
    std::size_t value = 0;
    const auto [parsed_end, status] =
        std::from_chars(std::to_address(text.begin()), text_end, value);
    if (status != std::errc{} || parsed_end != text_end || value == 0) {
        return std::nullopt;
    }
    return value;
}

} // namespace

std::size_t help_width(std::optional<std::size_t> terminal_columns, std::string_view columns_env) {
    std::optional<std::size_t> columns = terminal_columns;
    if (columns.has_value() && *columns == 0) {
        columns.reset();
    }
    if (!columns.has_value()) {
        columns = positive_number(columns_env);
    }
    return std::clamp(columns.value_or(default_help_width), min_help_width, max_help_width);
}

HelpStyle detect_help_style() {
    const bool terminal = ::isatty(STDOUT_FILENO) == 1;
    return HelpStyle{
        .color = stream_color(STDOUT_FILENO),
        .width =
            help_width(terminal ? read_terminal_columns() : std::nullopt, environment("COLUMNS")),
    };
}

HelpFormatter::HelpFormatter(HelpContent content) : content_(std::move(content)) {}

HelpFormatter::HelpFormatter(HelpContent content, HelpStyle style)
    : content_(std::move(content)), style_(style) {}

std::string HelpFormatter::make_help(const CLI::App* app, std::string path,
                                     CLI::AppFormatMode /*mode*/) const {
    if (app == nullptr) {
        return {};
    }
    const HelpStyle style = style_.has_value() ? *style_ : detect_help_style();
    const bool root = app->get_parent() == nullptr;
    std::string out;

    Line title(style.color);
    title.add(root && !content_.title.empty() ? std::string_view{content_.title}
                                              : std::string_view{path},
              Tone::title);
    emit(out, title);
    render_paragraph(out, app->get_description(), Margin{.indent = 0, .width = style.width},
                     Tone::plain, style.color);
    out += '\n';
    render_usage(out, *app, path, style.color);

    const std::vector<Section> sections = sections_of(*app, style.color);
    const Geometry geometry = geometry_of(sections, style.width);
    for (const Section& section : sections) {
        out += '\n';
        render_header(out, section.title, style.color);
        for (const Row& row : section.rows) {
            render_row(out, row, geometry, style.color);
        }
    }

    if (root) {
        render_examples(out, content_.examples, style);
        for (const std::string& note : content_.notes) {
            out += '\n';
            render_paragraph(out, note, Margin{.indent = 0, .width = style.width}, Tone::plain,
                             style.color);
        }
    }
    if (!listed_commands(*app).empty()) {
        render_command_hint(out, path, style);
    }
    return out;
}

} // namespace orcli
