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

// `-m MODEL:TAG`: the model reference and the provider tag after its last colon.
struct ModelChoice {
    std::string model;
    std::optional<std::string> provider;
};

// Pure functions.
Result<std::vector<ModelInfo>> parse_catalog(std::string_view json_text);
// Splits a -m value at its last colon unless the whole value is an exact catalog id (so
// "author/model:free" stays whole). An OpenRouter variant word after the colon is refused
// (Exit::model) rather than read as a tag; empty parts and malformed tags are Exit::usage.
Result<ModelChoice> split_model_choice(std::string_view value,
                                       const std::vector<ModelInfo>& catalog);
// A provider tag as -m MODEL:TAG and pin accept it: non-empty, with no colon, whitespace or
// control characters (Exit::usage).
Result<void> check_provider_tag(std::string_view tag);
Result<std::vector<Endpoint>> parse_endpoints(std::string_view json_text);
std::string format_endpoints(const std::vector<Endpoint>& endpoints);
std::string normalize_alias(std::string_view name);
std::vector<ModelInfo> search(std::string_view query, const std::vector<ModelInfo>& catalog);
Resolution resolve(std::string_view name, const std::vector<ModelInfo>& catalog,
                   const std::map<std::string, AliasEntry>& aliases);
std::string format_candidates(const std::vector<ModelInfo>& models);
// The output budget a request sends: requested, lowered to the model's listed maximum.
long long capped_output_tokens(const ModelInfo& model, long long requested);
// default_output_tokens is the configured budget used when --max-output-tokens is not given;
// saved_provider holds the tags `pin` saved for this model (none when empty).
std::string format_model(const ModelInfo& model, std::string_view catalog_fetched_at,
                         long long default_output_tokens,
                         const std::vector<std::string>& saved_provider = {});

// Persistence under state_dir: catalog.json (with fetch time) and models.json (aliases).
using FetchText = std::function<Result<std::string>()>;

struct Catalog {
    std::vector<ModelInfo> models;
    std::string fetched_at; // ISO 8601 UTC
    bool stale = false;
};

Result<Catalog> load_catalog(const std::filesystem::path& state_dir, std::chrono::hours ttl,
                             const FetchText& fetch, bool force_refresh);
Result<std::map<std::string, AliasEntry>> load_aliases(const std::filesystem::path& state_dir);
// A model has one alias at a time: saving removes every other alias of model_id and returns
// their names, so the caller can say what was replaced.
Result<std::vector<std::string>> save_alias(const std::filesystem::path& state_dir,
                                            const std::string& alias, const std::string& model_id);

// Removes one alias and returns what it pointed to, or nothing when no alias has that name.
Result<std::optional<AliasEntry>> remove_alias(const std::filesystem::path& state_dir,
                                               const std::string& alias);

} // namespace orcli

#endif // ORCLI_MODELS_HPP
