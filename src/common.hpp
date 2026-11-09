#pragma once

#include <expected>
#include <optional>
#include <string>
#include <utility>

namespace orcli {

// Process exit codes; documented in README.md and the skill, so values are a public contract.
enum class Exit : int {
    ok = 0,
    usage = 2,
    refused = 3,
    model = 4,
    api = 5,
    incomplete = 6,
    config = 7,
    interrupted = 8,
};

struct Error {
    Exit code;
    std::string message;
};

template <class T> using Result = std::expected<T, Error>;

inline std::unexpected<Error> fail(Exit code, std::string message) {
    return std::unexpected<Error>(Error{.code = code, .message = std::move(message)});
}

enum class Privacy {
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
