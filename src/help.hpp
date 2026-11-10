#ifndef ORCLI_HELP_HPP
#define ORCLI_HELP_HPP

#include <CLI/CLI.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orcli {

struct HelpExample {
    std::string caption;
    std::vector<std::string> commands; // never wrapped, so they stay copyable
};

// Material shown only on the top-level page.
struct HelpContent {
    std::string title; // replaces the program path as the page title
    std::vector<HelpExample> examples;
    std::vector<std::string> notes; // paragraphs after the examples
};

inline constexpr std::size_t default_help_width = 80;

struct HelpStyle {
    bool color = false;
    std::size_t width = default_help_width;
};

// The terminal's width when known and above 0, else COLUMNS when it is a whole number above
// 0, else default_help_width; clamped to 40..100.
[[nodiscard]] std::size_t help_width(std::optional<std::size_t> terminal_columns,
                                     std::string_view columns_env);

// stream_color and help_width applied to the environment and stdout.
[[nodiscard]] HelpStyle detect_help_style();

// Help pages with aligned two-column sections, leaf commands listed by path, examples on the
// top-level page, and color that never changes the plain layout. Subcommands inherit the
// formatter only when it is set before they are added (CLI11 copies it at creation).
class HelpFormatter final : public CLI::FormatterBase {
public:
    // The style is detected each time a page is rendered.
    explicit HelpFormatter(HelpContent content);
    HelpFormatter(HelpContent content, HelpStyle style);

    // --help-all is never enabled, so every format mode renders the same page.
    [[nodiscard]] std::string make_help(const CLI::App* app, std::string path,
                                        CLI::AppFormatMode /*mode*/) const override;

private:
    HelpContent content_;
    std::optional<HelpStyle> style_;
};

} // namespace orcli

#endif // ORCLI_HELP_HPP
