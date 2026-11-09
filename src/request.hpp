#ifndef ORCLI_REQUEST_HPP
#define ORCLI_REQUEST_HPP

#include "common.hpp"
#include "models.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orcli {

enum class Role : std::uint8_t {
    review,
    plain,
};

// System prompt for a role; empty for Role::plain.
std::string system_prompt(Role role);

// The user message: the query followed by the packed files (if any).
std::string user_message(std::string_view query, std::string_view packed_files);

struct RequestInput {
    const ModelInfo* model = nullptr;
    std::vector<Message> messages; // system (if any), session history, then the new user turn
    std::string effort;            // requested effort name, already defaulted by the caller
    long long max_output_tokens = 0;
    Privacy privacy = Privacy::zdr;
    std::vector<std::string> providers;    // --provider slugs, sent as provider.only when non-empty
    std::optional<std::string> session_id; // set only for --session calls
};

struct BuiltRequest {
    nlohmann::json body;
    std::optional<std::string> effort_sent; // effort placed in the body, if any
    long long max_tokens = 0;               // budget placed in the body after clamping
};

// Builds the Chat Completions body described in README.md. Exit::usage for an unknown effort
// name or "none" on a model whose reasoning is mandatory.
Result<BuiltRequest> build_request(const RequestInput& input);

} // namespace orcli

#endif // ORCLI_REQUEST_HPP
