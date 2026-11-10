#ifndef ORCLI_SECRETS_HPP
#define ORCLI_SECRETS_HPP

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orcli {

struct SecretFinding {
    std::string source;   // e.g. "query", "system", "history", or a file path
    std::size_t line = 0; // 1-based
    std::string pattern;  // pattern name; the matched value is never stored
};

struct SecretScan {
    std::string_view text;
    std::string_view source;
};

// Scans text for common credential formats (vendor API keys, private-key blocks, credential
// assignments, URLs with embedded passwords). A safeguard, not proof that text is clean.
std::vector<SecretFinding> scan_text(const SecretScan& input);

// Reason a resolved path must never be sent (e.g. ".env", private keys, SSH or cloud
// credential files), or nullopt. `.env.example` and `.env.sample` are allowed.
std::optional<std::string> credential_path_reason(const std::filesystem::path& resolved);

// Provider error text prepared for stderr: secret matches replaced with "[redacted]", control
// characters removed, and the result cut to max_chars with a trailing "...".
std::string sanitize_for_display(std::string_view text, std::size_t max_chars);

// Terminal-safe metadata; ordinary Unicode is preserved. Never apply to answer content.
std::string display_metadata(std::string_view text);

} // namespace orcli

#endif // ORCLI_SECRETS_HPP
