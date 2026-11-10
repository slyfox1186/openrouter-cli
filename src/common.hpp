#ifndef ORCLI_COMMON_HPP
#define ORCLI_COMMON_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace orcli {

// The same recovery identifier is accepted from headers, stream chunks and stored records.
inline bool valid_generation_id(std::string_view value) {
    constexpr std::size_t max_id_length = 256;
    constexpr std::string_view characters =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-";
    return !value.empty() && value.size() <= max_id_length &&
           value.find_first_not_of(characters) == std::string_view::npos;
}

// Process exit codes; documented in README.md and the skill, so values are a public contract.
enum class Exit : std::uint8_t {
    ok = 0,
    usage = 2,
    refused = 3,
    model = 4,
    api = 5,
    incomplete = 6,
    config = 7,
    interrupted = 8,
    // The request completed and may have been billed, but the answer, cost record or session
    // could not be saved; callers must not retry automatically.
    persist = 9,
};

struct Error {
    Exit code;
    std::string message;
};

template <class T> using Result = std::expected<T, Error>;

inline std::unexpected<Error> fail(Exit code, std::string message) {
    return std::unexpected<Error>(Error{.code = code, .message = std::move(message)});
}

enum class Privacy : std::uint8_t {
    zdr,
    no_training,
};

struct Message {
    std::string role;
    std::string content;
};

// Token counts and charge reported by OpenRouter. Every field is optional because the API
// marks them nullable or omits them; an absent cost means unknown, never zero.
struct Usage {
    std::optional<long long> prompt_tokens;
    std::optional<long long> cached_tokens;
    std::optional<long long> cache_write_tokens;
    std::optional<long long> completion_tokens;
    std::optional<long long> reasoning_tokens;
    std::optional<double> cost;
};

} // namespace orcli

#endif // ORCLI_COMMON_HPP
