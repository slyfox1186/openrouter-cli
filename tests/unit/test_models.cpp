#include "models.hpp"

#include "common.hpp"
#include "fsutil.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <map>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using orcli::AliasEntry;
using orcli::EffortSupport;
using orcli::Exit;
using orcli::FetchText;
using orcli::ModelInfo;
using orcli::Resolution;
using orcli::Result;

// Small stand-in for GET /api/v1/models: a ":batch" duplicate, "~...-latest" rows with a present
// and a missing target, every reasoning shape, wrong-typed fields and entries without an id.
constexpr std::string_view catalog_json = R"json({"data": [
  {"id": "google/gemini-4-argon", "name": "Google: Gemini 4 Argon",
   "canonical_slug": "google/gemini-4-argon-20261001", "created": 300,
   "context_length": 1048576, "top_provider": {"max_completion_tokens": 65536},
   "supported_parameters": ["reasoning", "max_tokens"], "expiration_date": null,
   "pricing": {"prompt": "0.000002", "completion": "0.000012",
               "input_cache_read": "0.0000001875", "overrides": [{"prompt": "0.000004"}]},
   "reasoning": {"mandatory": true, "supported_efforts": ["high", "low"],
                 "default_effort": "high"}},
  {"id": "google/gemini-4-argon:batch", "name": "Google: Gemini 4 Argon (batch)",
   "created": 300},
  {"id": "~google/gemini-pro-latest", "name": "Gemini Pro Latest", "created": 400,
   "alias_target": {"slug": "google/gemini-4-argon", "name": "Gemini 4 Argon"}},
  {"id": "~acme/widget-latest", "name": "Widget Latest", "created": 50,
   "alias_target": {"slug": "acme/widget-9", "name": "Widget 9"}},
  {"id": "openai/gpt-5.6-sol", "name": "OpenAI: GPT-5.6 Sol", "created": 100,
   "reasoning": {"mandatory": false, "supported_efforts": null}},
  {"id": "openai/gpt-6-sol", "name": "OpenAI: GPT-6 Sol", "created": 200,
   "reasoning": {"mandatory": false}, "expiration_date": "2026-12-31"},
  {"id": "openai/gpt-6-luna", "name": "OpenAI: GPT-6 Luna", "created": 250, "reasoning": null,
   "context_length": "large", "top_provider": {"max_completion_tokens": null},
   "pricing": "free"},
  {"name": "No id"},
  {"id": 7, "name": "Numeric id"},
  "junk"
]})json";

// Shape of GET /api/v1/models/{author}/{slug}/endpoints.
constexpr std::string_view endpoints_json = R"json({"data": {"id": "google/gemini-4-argon",
  "endpoints": [
    {"tag": "google-vertex/global", "provider_name": "Google",
     "pricing": {"prompt": "0.000002", "completion": "0.00001"},
     "context_length": 1048576, "max_completion_tokens": 943718, "service_tier": null,
     "status": 0},
    {"tag": "google-ai-studio/flex", "provider_name": "Google AI Studio",
     "pricing": {"prompt": "0.000001"}, "context_length": 1048576,
     "max_completion_tokens": null, "service_tier": "flex", "status": -2},
    {"provider_name": "No tag"},
    {"tag": 5, "provider_name": "Numeric tag"}
  ]}})json";

constexpr std::size_t endpoint_count = 2;
constexpr int degraded_status = -2;
constexpr long long vertex_max_output = 943'718;
// Header, one row per endpoint, then the --provider hint.
constexpr std::ptrdiff_t endpoint_table_lines = 4;

constexpr std::size_t catalog_size = 6;
constexpr long long argon_context = 1'048'576;
constexpr long long argon_max_output = 65'536;
constexpr std::chrono::hours day{24};
constexpr std::string_view stale_time = "2000-01-01T00:00:00Z";
constexpr std::size_t utc_length = 20;

std::vector<ModelInfo> test_catalog() {
    return orcli::parse_catalog(catalog_json).value();
}

ModelInfo model_named(const std::vector<ModelInfo>& models, std::string_view model_id) {
    const auto found = std::ranges::find(models, model_id, &ModelInfo::id);
    REQUIRE(found != models.end());
    return *found;
}

std::vector<std::string> ids_of(const std::vector<ModelInfo>& models) {
    return models | std::views::transform(&ModelInfo::id) | std::ranges::to<std::vector>();
}

ModelInfo priced(std::string_view prompt_price) {
    ModelInfo model;
    model.id = "acme/priced";
    model.name = "Acme: Priced";
    model.pricing.prompt = std::string(prompt_price);
    return model;
}

std::string input_price(std::string_view prompt_price) {
    return orcli::format_candidates({priced(prompt_price)});
}

class TempDir {
public:
    explicit TempDir(std::string_view label)
        : path_(std::filesystem::temp_directory_path() /
                std::format("orcli-test-models-{}-{}", label,
                            std::chrono::steady_clock::now().time_since_epoch().count())) {
        std::filesystem::create_directories(path_);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] std::filesystem::path state() const { return path_ / "state"; }

private:
    std::filesystem::path path_;
};

FetchText counting_fetch(int& calls) {
    return [&calls]() -> Result<std::string> {
        ++calls;
        return std::string(catalog_json);
    };
}

FetchText failing_fetch(int& calls) {
    return [&calls]() -> Result<std::string> {
        ++calls;
        return orcli::fail(Exit::api, "offline");
    };
}

void write_stale_cache(const std::filesystem::path& state_dir) {
    REQUIRE(orcli::ensure_private_dir(state_dir).has_value());
    const std::string document =
        std::format(R"({{"fetched_at": "{}", "body": {}}})", stale_time, catalog_json);
    REQUIRE(orcli::atomic_write(state_dir / "catalog.json", document).has_value());
}

std::map<std::string, AliasEntry> alias_to(std::string_view alias, std::string_view model_id) {
    return {{std::string(alias),
             AliasEntry{.model_id = std::string(model_id), .saved = std::string(stale_time)}}};
}

} // namespace

TEST_CASE("parse_catalog keeps models with string ids and drops batch rows") {
    const std::vector<ModelInfo> models = test_catalog();
    CHECK(models.size() == catalog_size);
    CHECK(std::ranges::none_of(
        models, [](const ModelInfo& model) { return model.id.ends_with(":batch"); }));
}

TEST_CASE("parse_catalog maps the fields of a complete entry") {
    const ModelInfo argon = model_named(test_catalog(), "google/gemini-4-argon");
    CHECK(argon.name == "Google: Gemini 4 Argon");
    CHECK(argon.canonical_slug == "google/gemini-4-argon-20261001");
    CHECK(argon.context_length == argon_context);
    CHECK(argon.max_completion_tokens == argon_max_output);
    CHECK(argon.supported_parameters.size() == 2);
    CHECK(argon.pricing.prompt == "0.000002");
}

TEST_CASE("parse_catalog maps listed reasoning efforts and pricing overrides") {
    const ModelInfo argon = model_named(test_catalog(), "google/gemini-4-argon");
    const std::vector<std::string> efforts{"high", "low"};
    CHECK(argon.has_reasoning);
    CHECK(argon.reasoning_mandatory);
    CHECK(argon.effort_support == EffortSupport::listed);
    CHECK(argon.supported_efforts == efforts);
    CHECK(argon.default_effort == "high");
    CHECK(argon.pricing.has_overrides);
}

TEST_CASE("parse_catalog maps null and absent supported_efforts") {
    const std::vector<ModelInfo> models = test_catalog();
    const ModelInfo any_effort = model_named(models, "openai/gpt-5.6-sol");
    const ModelInfo no_effort = model_named(models, "openai/gpt-6-sol");
    CHECK(any_effort.has_reasoning);
    CHECK_FALSE(any_effort.reasoning_mandatory);
    CHECK(any_effort.effort_support == EffortSupport::any);
    CHECK(no_effort.has_reasoning);
    CHECK(no_effort.effort_support == EffortSupport::none);
    CHECK(no_effort.expiration_date == "2026-12-31");
}

TEST_CASE("parse_catalog tolerates null reasoning and missing or wrong-typed fields") {
    const ModelInfo luna = model_named(test_catalog(), "openai/gpt-6-luna");
    CHECK_FALSE(luna.has_reasoning);
    CHECK(luna.effort_support == EffortSupport::none);
    CHECK(luna.context_length == 0);
    CHECK_FALSE(luna.max_completion_tokens.has_value());
    CHECK_FALSE(luna.pricing.prompt.has_value());
    CHECK(luna.canonical_slug.empty());
}

TEST_CASE("parse_catalog reads the alias target of a latest row") {
    const ModelInfo widget = model_named(test_catalog(), "~acme/widget-latest");
    CHECK(widget.alias_target == "acme/widget-9");
}

TEST_CASE("parse_catalog rejects a body without a data array") {
    for (const std::string_view body : {"not json", R"({"data": {}})", R"({"models": []})", "[]"}) {
        const auto parsed = orcli::parse_catalog(body);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().code == Exit::api);
    }
}

TEST_CASE("normalize_alias lowercases and joins runs with single dashes") {
    CHECK(orcli::normalize_alias("Gemini 4 Argon") == "gemini-4-argon");
    CHECK(orcli::normalize_alias("  GPT-5.6   Sol? ") == "gpt-5.6-sol");
    CHECK(orcli::normalize_alias("openai/gpt-6") == "openai-gpt-6");
    CHECK(orcli::normalize_alias("--").empty());
}

TEST_CASE("search matches whole tokens only") {
    const std::vector<ModelInfo> catalog = test_catalog();
    const std::vector<std::string> gpt6{"openai/gpt-6-luna", "openai/gpt-6-sol"};
    const std::vector<std::string> gpt56{"openai/gpt-5.6-sol"};
    CHECK(ids_of(orcli::search("gpt 6", catalog)) == gpt6);
    CHECK(ids_of(orcli::search("gpt 5.6", catalog)) == gpt56);
    CHECK(orcli::search("sol 7", catalog).empty());
    CHECK(orcli::search("--", catalog).empty());
}

TEST_CASE("search splits letter and digit boundaries in the query") {
    const std::vector<ModelInfo> catalog = test_catalog();
    CHECK(ids_of(orcli::search("gpt6", catalog)) == ids_of(orcli::search("GPT 6", catalog)));
    CHECK(ids_of(orcli::search("gpt5.6sol", catalog)) ==
          std::vector<std::string>{"openai/gpt-5.6-sol"});
}

TEST_CASE("search finds a unique model and hides latest rows that duplicate a present target") {
    const std::vector<ModelInfo> catalog = test_catalog();
    const std::vector<std::string> argon{"google/gemini-4-argon"};
    const std::vector<std::string> widget{"~acme/widget-latest"};
    CHECK(ids_of(orcli::search("gemini 4 argon", catalog)) == argon);
    CHECK(ids_of(orcli::search("gemini", catalog)) == argon);
    CHECK(ids_of(orcli::search("widget", catalog)) == widget);
}

TEST_CASE("search orders results by created, newest first") {
    const std::vector<std::string> newest_first{"openai/gpt-6-luna", "openai/gpt-6-sol",
                                                "openai/gpt-5.6-sol"};
    CHECK(ids_of(orcli::search("openai", test_catalog())) == newest_first);
}

TEST_CASE("resolve uses a saved alias by normalized or exact name") {
    const std::vector<ModelInfo> catalog = test_catalog();
    const auto aliases = alias_to("argon", "google/gemini-4-argon");
    const Resolution resolution = orcli::resolve("Argon", catalog, aliases);
    REQUIRE(resolution.kind == Resolution::Kind::found);
    CHECK(resolution.model.value().id == "google/gemini-4-argon");
    CHECK_FALSE(resolution.new_alias.has_value());
}

TEST_CASE("resolve falls through when an alias names a model that left the catalog") {
    const std::vector<ModelInfo> catalog = test_catalog();
    const auto aliases = alias_to("gemini-4-argon", "google/gemini-3-retired");
    const Resolution resolution = orcli::resolve("gemini 4 argon", catalog, aliases);
    REQUIRE(resolution.kind == Resolution::Kind::found);
    CHECK(resolution.model.value().id == "google/gemini-4-argon");
    CHECK(resolution.new_alias == "gemini-4-argon");
}

TEST_CASE("resolve accepts an exact catalog id") {
    const Resolution resolution = orcli::resolve("openai/gpt-6-sol", test_catalog(), {});
    REQUIRE(resolution.kind == Resolution::Kind::found);
    CHECK(resolution.model.value().id == "openai/gpt-6-sol");
    CHECK_FALSE(resolution.new_alias.has_value());
}

TEST_CASE("resolve follows a latest alias to its concrete model") {
    const std::vector<ModelInfo> catalog = test_catalog();
    const Resolution followed = orcli::resolve("~google/gemini-pro-latest", catalog, {});
    REQUIRE(followed.kind == Resolution::Kind::found);
    CHECK(followed.model.value().id == "google/gemini-4-argon");
    const Resolution missing = orcli::resolve("~acme/widget-latest", catalog, {});
    CHECK(missing.kind == Resolution::Kind::none);
    CHECK_FALSE(missing.model.has_value());
}

TEST_CASE("resolve saves an alias for a unique search match") {
    const Resolution resolution = orcli::resolve("Gemini 4 Argon", test_catalog(), {});
    REQUIRE(resolution.kind == Resolution::Kind::found);
    CHECK(resolution.model.value().id == "google/gemini-4-argon");
    CHECK(resolution.new_alias == "gemini-4-argon");
}

TEST_CASE("resolve reports ambiguous and unknown names") {
    const std::vector<ModelInfo> catalog = test_catalog();
    const std::vector<std::string> candidates{"openai/gpt-6-luna", "openai/gpt-6-sol"};
    const Resolution ambiguous = orcli::resolve("gpt 6", catalog, {});
    CHECK(ambiguous.kind == Resolution::Kind::ambiguous);
    CHECK(ids_of(ambiguous.candidates) == candidates);
    const Resolution unknown = orcli::resolve("llama 9", catalog, {});
    CHECK(unknown.kind == Resolution::Kind::none);
    CHECK(unknown.candidates.empty());
}

TEST_CASE("prices per million are converted exactly from the decimal strings") {
    CHECK(input_price("0.000002").contains("$2.00/M"));
    CHECK(input_price("0.0000001875").contains("$0.1875/M"));
    CHECK(input_price("0").contains("$0.00/M"));
    CHECK(input_price("0.00000123456").contains("$1.2346/M"));
    CHECK(input_price("0.00000999995").contains("$10.00/M"));
    CHECK(input_price("-1").contains("variable"));
}

TEST_CASE("format_candidates prints one line per model with n/a for absent values") {
    const std::string table = orcli::format_candidates({priced("0.000002")});
    CHECK(std::ranges::count(table, '\n') == 2);
    CHECK(table.contains("acme/priced"));
    CHECK(table.contains("n/a"));
    CHECK_FALSE(table.contains(" \n"));
}

TEST_CASE("format_model shows limits, reasoning and catalog prices") {
    const ModelInfo argon = model_named(test_catalog(), "google/gemini-4-argon");
    const std::string text = orcli::format_model(argon, "2026-11-09T00:00:00Z");
    CHECK(text.contains("1,048,576 tokens"));
    CHECK(text.contains("65,536"));
    CHECK(text.contains("mandatory; efforts high, low; default high"));
    CHECK(text.contains("$0.1875/M"));
    CHECK(text.contains("Prices are OpenRouter catalog rates fetched 2026-11-09T00:00:00Z; the "
                        "serving provider's rate can differ."));
}

TEST_CASE("format_model notes overrides, absent prices and expiration") {
    const std::vector<ModelInfo> catalog = test_catalog();
    const std::string argon =
        orcli::format_model(model_named(catalog, "google/gemini-4-argon"), "t");
    const std::string sol = orcli::format_model(model_named(catalog, "openai/gpt-6-sol"), "t");
    CHECK(argon.contains("different catalog rates"));
    CHECK_FALSE(sol.contains("different catalog rates"));
    CHECK(sol.contains("n/a"));
    CHECK(sol.contains("2026-12-31"));
}

TEST_CASE("load_catalog fetches once and serves a fresh cache") {
    const TempDir temp("fresh");
    int calls = 0;
    const auto first = orcli::load_catalog(temp.state(), day, counting_fetch(calls), false);
    REQUIRE(first.has_value());
    const auto second = orcli::load_catalog(temp.state(), day, counting_fetch(calls), false);
    REQUIRE(second.has_value());
    CHECK(calls == 1);
    CHECK(second.value().models.size() == catalog_size);
    CHECK(second.value().fetched_at == first.value().fetched_at);
}

TEST_CASE("load_catalog refetches on force_refresh") {
    const TempDir temp("force");
    int calls = 0;
    REQUIRE(orcli::load_catalog(temp.state(), day, counting_fetch(calls), false).has_value());
    REQUIRE(orcli::load_catalog(temp.state(), day, counting_fetch(calls), true).has_value());
    CHECK(calls == 2);
}

TEST_CASE("load_catalog refetches an expired cache") {
    const TempDir temp("expired");
    write_stale_cache(temp.state());
    int calls = 0;
    const auto loaded = orcli::load_catalog(temp.state(), day, counting_fetch(calls), false);
    REQUIRE(loaded.has_value());
    CHECK(calls == 1);
    CHECK(loaded.value().fetched_at != stale_time);
    CHECK(loaded.value().fetched_at.size() == utc_length);
}

TEST_CASE("load_catalog returns the stale cache when fetching fails") {
    const TempDir temp("stale");
    write_stale_cache(temp.state());
    int calls = 0;
    const auto loaded = orcli::load_catalog(temp.state(), day, failing_fetch(calls), false);
    REQUIRE(loaded.has_value());
    CHECK(calls == 1);
    CHECK(loaded.value().fetched_at == stale_time);
    CHECK(loaded.value().models.size() == catalog_size);
}

TEST_CASE("load_catalog reports the fetch error when there is no cache") {
    const TempDir temp("offline");
    int calls = 0;
    const auto loaded = orcli::load_catalog(temp.state(), day, failing_fetch(calls), false);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code == Exit::api);
}

TEST_CASE("load_aliases returns an empty map when no registry exists") {
    const TempDir temp("no-aliases");
    const auto empty = orcli::load_aliases(temp.state());
    REQUIRE(empty.has_value());
    CHECK(empty.value().empty());
}

TEST_CASE("aliases round trip through save_alias and load_aliases") {
    const TempDir temp("aliases");
    REQUIRE(orcli::save_alias(temp.state(), "argon", "google/gemini-4-argon").has_value());
    REQUIRE(orcli::save_alias(temp.state(), "sol", "openai/gpt-6-sol").has_value());
    REQUIRE(orcli::save_alias(temp.state(), "argon", "openai/gpt-6-luna").has_value());
    const std::map<std::string, AliasEntry> aliases = orcli::load_aliases(temp.state()).value();
    CHECK(aliases.size() == 2);
    CHECK(aliases.at("argon").model_id == "openai/gpt-6-luna");
    CHECK(aliases.at("sol").model_id == "openai/gpt-6-sol");
    CHECK(aliases.at("sol").saved.size() == utc_length);
}

TEST_CASE("load_aliases reports a corrupt registry as a configuration error") {
    const TempDir temp("corrupt");
    REQUIRE(orcli::ensure_private_dir(temp.state()).has_value());
    const std::filesystem::path file = temp.state() / "models.json";
    REQUIRE(orcli::atomic_write(file, R"({"aliases": {"argon": {"saved": "x"}}})").has_value());
    const auto loaded = orcli::load_aliases(temp.state());
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code == Exit::config);
    CHECK(loaded.error().message.contains(file.string()));
}

TEST_CASE("parse_endpoints reads tagged endpoints and skips the rest") {
    const std::vector<orcli::Endpoint> endpoints = orcli::parse_endpoints(endpoints_json).value();
    REQUIRE(endpoints.size() == endpoint_count);
    const orcli::Endpoint& vertex = endpoints.front();
    CHECK(vertex.tag == "google-vertex/global");
    CHECK(vertex.provider_name == "Google");
    CHECK(vertex.prompt_price == "0.000002");
    CHECK(vertex.context_length == argon_context);
    CHECK(vertex.max_completion_tokens == vertex_max_output);
    CHECK_FALSE(vertex.service_tier.has_value());
}

TEST_CASE("parse_endpoints tolerates absent prices, null limits and degraded status") {
    const std::vector<orcli::Endpoint> endpoints = orcli::parse_endpoints(endpoints_json).value();
    REQUIRE(endpoints.size() == endpoint_count);
    const orcli::Endpoint& flex = endpoints.back();
    CHECK_FALSE(flex.completion_price.has_value());
    CHECK_FALSE(flex.max_completion_tokens.has_value());
    CHECK(flex.service_tier == "flex");
    CHECK(flex.status == degraded_status);
}

TEST_CASE("parse_endpoints rejects a body without data.endpoints") {
    for (const std::string_view body : {"not json", R"({"data": []})",
                                        R"({"data": {"endpoints": {}}})", R"({"endpoints": []})"}) {
        const auto parsed = orcli::parse_endpoints(body);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().code == Exit::api);
    }
}

TEST_CASE("format_endpoints lists each endpoint and the --provider hint") {
    const std::string table =
        orcli::format_endpoints(orcli::parse_endpoints(endpoints_json).value());
    CHECK(table.contains("google-vertex/global"));
    CHECK(table.contains("$2.00/M"));
    CHECK(table.contains("$10.00/M"));
    CHECK(table.contains("943,718"));
    CHECK(table.contains("flex; status -2"));
    CHECK(table.ends_with("Pass a tag (or its provider prefix, e.g. google-vertex) to "
                          "--provider to use only those endpoints.\n"));
}

TEST_CASE("format_endpoints shows status only when it is not zero") {
    const std::string table =
        orcli::format_endpoints(orcli::parse_endpoints(endpoints_json).value());
    CHECK(std::ranges::count(table, '\n') == endpoint_table_lines);
    CHECK_FALSE(table.contains("status 0"));
    CHECK_FALSE(table.contains(" \n"));
}
