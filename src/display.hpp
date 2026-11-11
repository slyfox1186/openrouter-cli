#ifndef ORCLI_DISPLAY_HPP
#define ORCLI_DISPLAY_HPP

#include <cstddef>
#include <string>
#include <string_view>

namespace orcli {

// Provider error text prepared for stderr: control characters other than newline and tab
// replaced with spaces, and the result cut to max_chars with a trailing "...".
std::string sanitize_for_display(std::string_view text, std::size_t max_chars);

// Terminal-safe metadata; ordinary Unicode is preserved. Never apply to answer content.
std::string display_metadata(std::string_view text);

} // namespace orcli

#endif // ORCLI_DISPLAY_HPP
