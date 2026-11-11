#include "request.hpp"

#include "common.hpp"
#include "models.hpp"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr long long budget = 64000;
constexpr long long small_model_limit = 32000;
constexpr long long large_model_limit = 128000;

orcli::ModelInfo plain_model() {
    orcli::ModelInfo model;
    model.id = "openai/plain-model";
    return model;
}

orcli::ModelInfo reasoning_model(orcli::EffortSupport support) {
    orcli::ModelInfo model;
    model.id = "google/reasoning-model";
    model.has_reasoning = true;
    model.effort_support = support;
    return model;
}

orcli::RequestInput input_for(const orcli::ModelInfo& model, std::string effort) {
    orcli::RequestInput input;
    input.model = &model;
    input.messages = std::vector<orcli::Message>{
        orcli::Message{.role = "system", .content = "system text"},
        orcli::Message{.role = "user", .content = "user text"},
    };
    input.effort = std::move(effort);
    input.max_output_tokens = budget;
    return input;
}

orcli::BuiltRequest build_ok(const orcli::RequestInput& input) {
    orcli::Result<orcli::BuiltRequest> built = orcli::build_request(input);
    REQUIRE(built.has_value());
    return std::move(*built);
}

orcli::Error build_error(const orcli::RequestInput& input) {
    const orcli::Result<orcli::BuiltRequest> built = orcli::build_request(input);
    REQUIRE_FALSE(built.has_value());
    return built.error();
}

nlohmann::json reasoning_with(std::string_view effort) {
    return nlohmann::json::object({{"effort", effort}, {"exclude", true}});
}

} // namespace

TEST_CASE("request: core fields") {
    const orcli::ModelInfo model = plain_model();
    const orcli::BuiltRequest built = build_ok(input_for(model, "high"));
    const nlohmann::json& body = built.body;
    CHECK(body.at("model") == nlohmann::json(model.id));
    CHECK(body.at("stream") == nlohmann::json(true));
    const nlohmann::json expected_messages = nlohmann::json::array(
        {nlohmann::json::object({{"role", "system"}, {"content", "system text"}}),
         nlohmann::json::object({{"role", "user"}, {"content", "user text"}})});
    CHECK(body.at("messages") == expected_messages);
}

TEST_CASE("request: model without reasoning gets no reasoning object") {
    const orcli::ModelInfo model = plain_model();
    const orcli::BuiltRequest built = build_ok(input_for(model, "high"));
    CHECK_FALSE(built.body.contains("reasoning"));
    CHECK_FALSE(built.effort_sent.has_value());
}

TEST_CASE("request: effort support none sends exclude only") {
    const orcli::ModelInfo model = reasoning_model(orcli::EffortSupport::none);
    const orcli::BuiltRequest built = build_ok(input_for(model, "high"));
    CHECK(built.body.at("reasoning") == nlohmann::json::object({{"exclude", true}}));
    CHECK_FALSE(built.effort_sent.has_value());
}

TEST_CASE("request: effort support any sends the requested effort") {
    const orcli::ModelInfo model = reasoning_model(orcli::EffortSupport::any);
    const orcli::BuiltRequest built = build_ok(input_for(model, "xhigh"));
    CHECK(built.body.at("reasoning") == reasoning_with("xhigh"));
    CHECK(built.effort_sent.value_or("") == "xhigh");
}

TEST_CASE("request: listed effort that the model supports") {
    orcli::ModelInfo model = reasoning_model(orcli::EffortSupport::listed);
    model.supported_efforts = {"low", "high"};
    const orcli::BuiltRequest built = build_ok(input_for(model, "high"));
    CHECK(built.body.at("reasoning") == reasoning_with("high"));
    CHECK(built.effort_sent.value_or("") == "high");
}

TEST_CASE("request: listed effort that the model lacks is left for OpenRouter to map") {
    orcli::ModelInfo model = reasoning_model(orcli::EffortSupport::listed);
    model.supported_efforts = {"low", "high"};
    const orcli::BuiltRequest built = build_ok(input_for(model, "max"));
    CHECK(built.body.at("reasoning") == reasoning_with("max"));
    CHECK(built.effort_sent.value_or("") == "max");
}

TEST_CASE("request: none is sent when reasoning is optional") {
    orcli::ModelInfo model = reasoning_model(orcli::EffortSupport::listed);
    model.supported_efforts = {"none", "low", "high"};
    const orcli::BuiltRequest built = build_ok(input_for(model, "none"));
    CHECK(built.body.at("reasoning") == reasoning_with("none"));
}

TEST_CASE("request: none on a model whose reasoning is mandatory is a usage error") {
    orcli::ModelInfo model = reasoning_model(orcli::EffortSupport::listed);
    model.supported_efforts = {"low", "high"};
    model.reasoning_mandatory = true;
    const orcli::Error error = build_error(input_for(model, "none"));
    CHECK(error.code == orcli::Exit::usage);
    CHECK(error.message.find(model.id) != std::string::npos);

    model.effort_support = orcli::EffortSupport::any;
    CHECK(build_error(input_for(model, "none")).code == orcli::Exit::usage);

    const orcli::BuiltRequest built = build_ok(input_for(model, "low"));
    CHECK(built.effort_sent.value_or("") == "low");
}

TEST_CASE("request: unknown effort name is a usage error") {
    const orcli::ModelInfo model = reasoning_model(orcli::EffortSupport::any);
    const orcli::Error error = build_error(input_for(model, "extreme"));
    CHECK(error.code == orcli::Exit::usage);
    CHECK(error.message.find("extreme") != std::string::npos);

    const orcli::ModelInfo no_reasoning = plain_model();
    CHECK(build_error(input_for(no_reasoning, "")).code == orcli::Exit::usage);
}

TEST_CASE("request: every documented effort name is accepted") {
    const orcli::ModelInfo model = reasoning_model(orcli::EffortSupport::any);
    for (const std::string_view effort :
         {"max", "xhigh", "high", "medium", "low", "minimal", "none"}) {
        CAPTURE(effort);
        CHECK(orcli::build_request(input_for(model, std::string{effort})).has_value());
    }
}

TEST_CASE("request: output budget is clamped to the model limit") {
    orcli::ModelInfo model = plain_model();

    const orcli::BuiltRequest unlimited = build_ok(input_for(model, "high"));
    CHECK(unlimited.max_tokens == budget);
    CHECK(unlimited.body.at("max_tokens") == nlohmann::json(budget));

    model.max_completion_tokens = small_model_limit;
    const orcli::BuiltRequest clamped = build_ok(input_for(model, "high"));
    CHECK(clamped.max_tokens == small_model_limit);
    CHECK(clamped.body.at("max_tokens") == nlohmann::json(small_model_limit));

    model.max_completion_tokens = large_model_limit;
    CHECK(build_ok(input_for(model, "high")).max_tokens == budget);
}

TEST_CASE("request: output budget below one is a usage error") {
    orcli::ModelInfo model = plain_model();
    orcli::RequestInput input = input_for(model, "high");
    input.max_output_tokens = 0;
    CHECK(build_error(input).code == orcli::Exit::usage);

    model.max_completion_tokens = 0;
    CHECK(build_error(input_for(model, "high")).code == orcli::Exit::usage);
}

TEST_CASE("request: missing model is a usage error") {
    const orcli::ModelInfo model = plain_model();
    orcli::RequestInput input = input_for(model, "high");
    input.model = nullptr;
    CHECK(build_error(input).code == orcli::Exit::usage);
}

TEST_CASE("request: no sampling or max_completion_tokens fields") {
    const orcli::ModelInfo model = reasoning_model(orcli::EffortSupport::any);
    orcli::RequestInput input = input_for(model, "high");
    input.session_id = "session-1";
    const orcli::BuiltRequest built = build_ok(input);
    for (const std::string_view field :
         {"temperature", "top_p", "top_k", "max_completion_tokens"}) {
        CAPTURE(field);
        CHECK_FALSE(built.body.contains(field));
    }
}

TEST_CASE("request: privacy levels") {
    const orcli::ModelInfo model = plain_model();
    orcli::RequestInput input = input_for(model, "high");

    input.privacy = orcli::Privacy::zdr;
    CHECK(build_ok(input).body.at("provider") ==
          nlohmann::json::object(
              {{"data_collection", "deny"}, {"require_parameters", true}, {"zdr", true}}));

    input.privacy = orcli::Privacy::no_training;
    CHECK(build_ok(input).body.at("provider") ==
          nlohmann::json::object({{"data_collection", "deny"}, {"require_parameters", true}}));
}

TEST_CASE("request: provider restriction") {
    const orcli::ModelInfo model = plain_model();
    orcli::RequestInput input = input_for(model, "high");

    CHECK_FALSE(build_ok(input).body.at("provider").contains("only"));

    input.providers = {"google-vertex", "anthropic"};
    const orcli::BuiltRequest two = build_ok(input);
    CHECK(two.body.at("provider").at("only") ==
          nlohmann::json::array({"google-vertex", "anthropic"}));
    CHECK(two.body.at("provider").at("data_collection") == nlohmann::json("deny"));
    CHECK(two.body.at("provider").at("require_parameters") == nlohmann::json(true));
    CHECK(two.body.at("provider").at("zdr") == nlohmann::json(true));

    input.providers = {"anthropic", "google-vertex", "anthropic"};
    CHECK(build_ok(input).body.at("provider").at("only") ==
          nlohmann::json::array({"anthropic", "google-vertex"}));
}

TEST_CASE("request: web and context-compression plugins are disabled") {
    const orcli::ModelInfo model = plain_model();
    const orcli::BuiltRequest built = build_ok(input_for(model, "high"));
    CHECK(built.body.at("plugins") ==
          nlohmann::json::array(
              {nlohmann::json::object({{"id", "web"}, {"enabled", false}}),
               nlohmann::json::object({{"id", "context-compression"}, {"enabled", false}})}));
}

TEST_CASE("request: session_id appears only for sessions and no model gets vendor fields") {
    for (const char* model_id : {"anthropic/claude-model", "test/plain-model"}) {
        orcli::ModelInfo model = plain_model();
        model.id = model_id;

        const orcli::BuiltRequest one_shot = build_ok(input_for(model, "high"));
        CHECK_FALSE(one_shot.body.contains("session_id"));
        CHECK_FALSE(one_shot.body.contains("cache_control"));

        orcli::RequestInput input = input_for(model, "high");
        input.session_id = "review-42";
        const orcli::BuiltRequest session = build_ok(input);
        CHECK(session.body.at("session_id") == nlohmann::json("review-42"));
        CHECK_FALSE(session.body.contains("cache_control"));
    }
}

TEST_CASE("request: review prompt states the verdicts and uses no dashes") {
    const std::string prompt = orcli::system_prompt(orcli::Role::review);
    for (const std::string_view verdict : {"`APPROVE`", "`APPROVE WITH CHANGES`", "`RETHINK`"}) {
        CAPTURE(verdict);
        CHECK(prompt.find(verdict) != std::string::npos);
    }
    CHECK(prompt.find("`path:line`") != std::string::npos);
    constexpr auto dashes = std::to_array<std::string_view>({"\u2014", "\u2013"});
    for (const std::string_view dash : dashes) {
        CHECK(prompt.find(dash) == std::string::npos);
    }
    CHECK(orcli::system_prompt(orcli::Role::plain).empty());
}

TEST_CASE("request: user message appends packed files after a blank line") {
    CHECK(orcli::user_message("check this", "") == "check this");
    CHECK(orcli::user_message("check this", "## a.cpp\n```\n1 | x\n```\n") ==
          "check this\n\n## a.cpp\n```\n1 | x\n```\n");
}

namespace {

constexpr long long incident_budget = 24000;
constexpr long long incident_reasoning = 17465;
constexpr long long below_budget_completion = 9000;
constexpr long long below_budget_reasoning = 8465;
constexpr long long reported_completion = 14998;
constexpr long long reported_reasoning = 15806;
constexpr long long lowest = std::numeric_limits<long long>::min();
constexpr long long highest = std::numeric_limits<long long>::max();

orcli::Usage usage_of(std::optional<long long> completion, std::optional<long long> reasoning) {
    orcli::Usage usage;
    usage.completion_tokens = completion;
    usage.reasoning_tokens = reasoning;
    return usage;
}

std::string stop_reason(long long sent, std::optional<long long> completion,
                        std::optional<long long> reasoning) {
    return orcli::length_stop_reason(sent, std::nullopt, usage_of(completion, reasoning));
}

std::string stop_reason_with_limit(long long sent, long long model_limit,
                                   std::optional<long long> completion,
                                   std::optional<long long> reasoning) {
    return orcli::length_stop_reason(sent, model_limit, usage_of(completion, reasoning));
}

} // namespace

TEST_CASE("length stop: the whole sent budget used reports reasoning and answer tokens") {
    CHECK(stop_reason(incident_budget, incident_budget, incident_reasoning) ==
          "the answer used the whole 24000 token output budget after 17465 reasoning tokens, "
          "with 6535 answer tokens; retry with a higher --max-output-tokens or a lower --effort");
    CHECK(stop_reason(incident_budget, incident_budget + 2, incident_reasoning) ==
          "the answer used the whole 24000 token output budget after 17465 reasoning tokens, "
          "with 6537 answer tokens; retry with a higher --max-output-tokens or a lower --effort");
    CHECK(stop_reason(incident_budget, incident_budget, incident_budget) ==
          "the answer used the whole 24000 token output budget after 24000 reasoning tokens, "
          "with 0 answer tokens; retry with a higher --max-output-tokens or a lower --effort");
    CHECK(stop_reason(incident_budget, incident_budget, std::nullopt) ==
          "the answer used the whole 24000 token output budget; retry with a higher "
          "--max-output-tokens or a lower --effort");
}

TEST_CASE("length stop: below the sent budget says a higher budget will not help") {
    CHECK(stop_reason(budget, below_budget_completion, below_budget_reasoning) ==
          "the answer stopped at 9000 output tokens, below the 64000 token budget, at a provider "
          "or context limit after 8465 reasoning tokens, with 535 answer tokens; a higher "
          "--max-output-tokens will not help; retry with a lower --effort or less input");
    CHECK(stop_reason(budget, reported_completion, reported_reasoning) ==
          "the answer stopped at 14998 output tokens, below the 64000 token budget, at a "
          "provider or context limit after 15806 reasoning tokens; a higher --max-output-tokens "
          "will not help; retry with a lower --effort or less input");
}

TEST_CASE("length stop: unknown completion gives the general advice") {
    CHECK(stop_reason(incident_budget, std::nullopt, std::nullopt) ==
          "the answer stopped at an output limit (budget 24000 tokens); retry with a lower "
          "--effort or a higher --max-output-tokens");
    CHECK(stop_reason(incident_budget, std::nullopt, incident_reasoning) ==
          "the answer stopped at an output limit (budget 24000 tokens) after 17465 reasoning "
          "tokens; retry with a lower --effort or a higher --max-output-tokens");
    CHECK(stop_reason(incident_budget, lowest, incident_reasoning) ==
          "the answer stopped at an output limit (budget 24000 tokens) after 17465 reasoning "
          "tokens; retry with a lower --effort or a higher --max-output-tokens");
}

TEST_CASE("length stop: negative and extreme counts never reach the subtraction") {
    CHECK(stop_reason(incident_budget, incident_budget, -1) ==
          "the answer used the whole 24000 token output budget; retry with a higher "
          "--max-output-tokens or a lower --effort");
    CHECK(stop_reason(incident_budget, incident_budget, lowest) ==
          "the answer used the whole 24000 token output budget; retry with a higher "
          "--max-output-tokens or a lower --effort");
    CHECK(stop_reason(incident_budget, highest, 0) ==
          std::format("the answer used the whole 24000 token output budget after 0 reasoning "
                      "tokens, with {} answer tokens; retry with a higher --max-output-tokens "
                      "or a lower --effort",
                      highest));
}

TEST_CASE("length stop: a budget at the model's listed maximum is not advised to grow") {
    CHECK(stop_reason_with_limit(incident_budget, incident_budget, incident_budget,
                                 incident_reasoning) ==
          "the answer used the whole 24000 token output budget (the model's listed maximum) "
          "after 17465 reasoning tokens, with 6535 answer tokens; retry with a lower --effort or "
          "less input");
    CHECK(stop_reason_with_limit(incident_budget, incident_budget, std::nullopt, std::nullopt) ==
          "the answer stopped at an output limit (budget 24000 tokens, the model's listed "
          "maximum); retry with a lower --effort or less input");
    CHECK(stop_reason_with_limit(incident_budget, budget, incident_budget, incident_reasoning) ==
          stop_reason(incident_budget, incident_budget, incident_reasoning));
    CHECK(stop_reason_with_limit(incident_budget, budget, std::nullopt, std::nullopt) ==
          stop_reason(incident_budget, std::nullopt, std::nullopt));
    CHECK(stop_reason_with_limit(budget, budget, below_budget_completion, below_budget_reasoning) ==
          stop_reason(budget, below_budget_completion, below_budget_reasoning));
}
