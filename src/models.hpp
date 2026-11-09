#ifndef ORCLI_MODELS_HPP
#define ORCLI_MODELS_HPP

#include "common.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orcli {

enum class EffortSupport : std::uint8_t {
    none,   // no reasoning.supported_efforts field: effort cannot be selected
    any,    // supported_efforts is null: every effort is accepted
    listed, // supported_efforts is a list
};

struct Pricing { // decimal USD per token as published; absent means not published
    std::optional<std::string> prompt;
    std::optional<std::string> completion;
    std::optional<std::string> input_cache_read;
    std::optional<std::string> input_cache_write;
    std::optional<std::string> internal_reasoning;
    std::optional<std::string> request;
    bool has_overrides = false;
};

struct ModelInfo {
    std::string id;
    std::string name;
    std::string canonical_slug;
    std::optional<std::string> alias_target; // concrete slug for "~author/family-latest" rows
    long long context_length = 0;
    std::optional<long long> max_completion_tokens; // top_provider.max_completion_tokens
    bool has_reasoning = false;
    bool reasoning_mandatory = false;
    EffortSupport effort_support = EffortSupport::none;
    std::vector<std::string> supported_efforts;
    std::optional<std::string> default_effort;
    std::vector<std::string> supported_parameters;
    Pricing pricing;
    std::optional<std::string> expiration_date;
    long long created = 0;
};

struct AliasEntry {
    std::string model_id;
    std::string saved; // ISO 8601 UTC
};

struct Resolution {
    enum class Kind : std::uint8_t { found, ambiguous, none };
    Kind kind = Kind::none;
    std::optional<ModelInfo> model;       // concrete model (a "~...-latest" alias is followed)
    std::vector<ModelInfo> candidates;    // for ambiguous
    std::optional<std::string> new_alias; // set when a plain-name match should be saved
};

// One provider endpoint of a model, from GET /api/v1/models/{id}/endpoints.
struct Endpoint {
    std::string tag; // routing slug, e.g. "google-vertex/global"; accepted by --provider
    std::string provider_name;
    std::optional<std::string> prompt_price; // decimal USD per token as published
    std::optional<std::string> completion_price;
    long long context_length = 0;
    std::optional<long long> max_completion_tokens;
    std::optional<std::string> service_tier;
    int status = 0; // 0 normal; OpenRouter reports negative values for degraded endpoints
};

// Pure functions.
Result<std::vector<ModelInfo>> parse_catalog(std::string_view json_text);
Result<std::vector<Endpoint>> parse_endpoints(std::string_view json_text);
std::string format_endpoints(const std::vector<Endpoint>& endpoints);
std::string normalize_alias(std::string_view name);
std::vector<ModelInfo> search(std::string_view query, const std::vector<ModelInfo>& catalog);
Resolution resolve(std::string_view name, const std::vector<ModelInfo>& catalog,
                   const std::map<std::string, AliasEntry>& aliases);
std::string format_candidates(const std::vector<ModelInfo>& models);
std::string format_model(const ModelInfo& model, std::string_view catalog_fetched_at);

// Persistence under state_dir: catalog.json (with fetch time) and models.json (aliases).
using FetchText = std::function<Result<std::string>()>;

struct Catalog {
    std::vector<ModelInfo> models;
    std::string fetched_at; // ISO 8601 UTC
};

Result<Catalog> load_catalog(const std::filesystem::path& state_dir, std::chrono::hours ttl,
                             const FetchText& fetch, bool force_refresh);
Result<std::map<std::string, AliasEntry>> load_aliases(const std::filesystem::path& state_dir);
Result<void> save_alias(const std::filesystem::path& state_dir, const std::string& alias,
                        const std::string& model_id);

} // namespace orcli

#endif // ORCLI_MODELS_HPP
