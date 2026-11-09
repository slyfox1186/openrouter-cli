#include "mock_server.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace mock {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int http_ok = 200;
constexpr int http_payment_required = 402;
constexpr int hex_repeats = 4;
constexpr time_t keep_alive_seconds = 1;
constexpr std::chrono::milliseconds keepalive_interval{200};
constexpr std::chrono::milliseconds check_interval{50};
constexpr const char* loopback = "127.0.0.1";
constexpr const char* json_type = "application/json";
constexpr const char* event_stream_type = "text/event-stream";
constexpr const char* generation_id = "gen-test-1";

constexpr std::string_view catalog_body = R"json({"data":[
{"id":"test/alpha-1","canonical_slug":"test/alpha-1-20261101","name":"Test: Alpha 1",
 "created":1793000000,"description":"Mock reasoning model","context_length":128000,
 "architecture":{"input_modalities":["text"],"output_modalities":["text"],"tokenizer":"Other"},
 "pricing":{"prompt":"0.0000025","completion":"0.00001","input_cache_read":"0.00000025"},
 "top_provider":{"context_length":128000,"max_completion_tokens":8000,"is_moderated":false},
 "per_request_limits":null,
 "supported_parameters":["max_tokens","reasoning","include_reasoning","temperature","top_p"],
 "reasoning":{"mandatory":false,"supported_efforts":["high","medium","low"]},
 "expiration_date":null},
{"id":"test/beta-2","canonical_slug":"test/beta-2","name":"Beta 2","created":1792000000,
 "description":"Mock model","context_length":32000,
 "architecture":{"input_modalities":["text"],"output_modalities":["text"],"tokenizer":"Other"},
 "pricing":{"prompt":"0.000001","completion":"0.000002"},
 "top_provider":{"context_length":32000,"max_completion_tokens":4000,"is_moderated":false},
 "per_request_limits":null,"supported_parameters":["max_tokens","temperature","top_p"],
 "expiration_date":null},
{"id":"test/beta-3","canonical_slug":"test/beta-3","name":"Beta 3","created":1792500000,
 "description":"Mock model","context_length":32000,
 "architecture":{"input_modalities":["text"],"output_modalities":["text"],"tokenizer":"Other"},
 "pricing":{"prompt":"0.000001","completion":"0.000002"},
 "top_provider":{"context_length":32000,"max_completion_tokens":4000,"is_moderated":false},
 "per_request_limits":null,"supported_parameters":["max_tokens","temperature","top_p"],
 "expiration_date":null},
{"id":"anthropic/claude-test","canonical_slug":"anthropic/claude-test-20261001",
 "name":"Anthropic: Claude Test","created":1791000000,"description":"Mock model that always reasons",
 "context_length":200000,
 "architecture":{"input_modalities":["text"],"output_modalities":["text"],"tokenizer":"Claude"},
 "pricing":{"prompt":"0.000003","completion":"0.000015","input_cache_read":"0.0000003",
            "input_cache_write":"0.00000375"},
 "top_provider":{"context_length":200000,"max_completion_tokens":16000,"is_moderated":false},
 "per_request_limits":null,
 "supported_parameters":["max_tokens","reasoning","include_reasoning"],
 "reasoning":{"mandatory":true,"supported_efforts":["high","medium","low"]},
 "expiration_date":null}
]})json";

constexpr std::string_view generation_body =
    R"({"data":{"id":"gen-test-1","total_cost":0.0123,"provider_name":"MockProvider",)"
    R"("model":"test/alpha-1","native_tokens_prompt":10,"native_tokens_completion":5,)"
    R"("finish_reason":"stop"}})";

constexpr std::string_view generation_missing_body =
    R"({"error":{"code":404,"message":"Generation not found"}})";

constexpr std::string_view key_body =
    R"({"data":{"usage":1.5,"usage_daily":0.25,"usage_weekly":0.5,"usage_monthly":1.0,)"
    R"("limit":null,"limit_remaining":null,"limit_reset":null}})";

constexpr std::string_view provider_error_body =
    R"({"error":{"code":502,"message":"Upstream provider failed",)"
    R"("metadata":{"error_type":"provider_error"}}})";

constexpr std::string_view processing = ": OPENROUTER PROCESSING\n\n";

constexpr std::string_view hello_chunk =
    R"(data: {"id":"gen-test-1","model":"test/alpha-1","choices":[{"index":0,)"
    R"("delta":{"role":"assistant","content":"Hello"},"finish_reason":null}]})"
    "\n\n";

constexpr std::string_view world_chunk =
    R"(data: {"id":"gen-test-1","model":"test/alpha-1","choices":[{"index":0,)"
    R"("delta":{"content":" world"},"finish_reason":null}]})"
    "\n\n";

constexpr std::string_view partial_chunk =
    R"(data: {"id":"gen-test-1","model":"test/alpha-1","choices":[{"index":0,)"
    R"("delta":{"role":"assistant","content":"Partial"},"finish_reason":null}]})"
    "\n\n";

constexpr std::string_view stop_chunk =
    R"(data: {"id":"gen-test-1","model":"test/alpha-1","choices":[{"index":0,"delta":{},)"
    R"("finish_reason":"stop","native_finish_reason":"stop"}]})"
    "\n\n";

constexpr std::string_view usage_chunk =
    R"(data: {"id":"gen-test-1","model":"test/alpha-1","choices":[],"usage":{)"
    R"("prompt_tokens":10,"prompt_tokens_details":{"cached_tokens":0},"completion_tokens":5,)"
    R"("completion_tokens_details":{"reasoning_tokens":0},"cost":0.0042},)"
    R"("openrouter_metadata":{"endpoints":{"available":[)"
    R"({"provider":"MockProvider","selected":true}]}}})"
    "\n\n";

constexpr std::string_view usage_no_cost_chunk =
    R"(data: {"id":"gen-test-1","model":"test/alpha-1","choices":[],"usage":{)"
    R"("prompt_tokens":10,"prompt_tokens_details":{"cached_tokens":0},"completion_tokens":5,)"
    R"("completion_tokens_details":{"reasoning_tokens":0},"cost":null},)"
    R"("openrouter_metadata":{"endpoints":{"available":[)"
    R"({"provider":"MockProvider","selected":true}]}}})"
    "\n\n";

constexpr std::string_view error_chunk =
    R"(data: {"id":"gen-test-1","model":"test/alpha-1","choices":[{"index":0,)"
    R"("delta":{"content":""},"finish_reason":"error"}],)"
    R"("error":{"code":502,"message":"Upstream provider failed",)"
    R"("metadata":{"error_type":"provider_error"}}})"
    "\n\n";

constexpr std::string_view usage_after_error_chunk =
    R"(data: {"id":"gen-test-1","model":"test/alpha-1","choices":[],"usage":{)"
    R"("prompt_tokens":10,"completion_tokens":1,"cost":0.0031}})"
    "\n\n";

constexpr std::string_view length_chunk =
    R"(data: {"id":"gen-test-1","model":"test/alpha-1","choices":[{"index":0,)"
    R"("delta":{"role":"assistant","content":""},"finish_reason":"length",)"
    R"("native_finish_reason":"max_tokens"}]})"
    "\n\n";

constexpr std::string_view usage_length_chunk =
    R"(data: {"id":"gen-test-1","model":"test/alpha-1","choices":[],"usage":{)"
    R"("prompt_tokens":10,"completion_tokens":8000,)"
    R"("completion_tokens_details":{"reasoning_tokens":8000},"cost":0.0042}})"
    "\n\n";

constexpr std::string_view done_event = "data: [DONE]\n\n";

std::vector<std::string_view> events_for(Scenario scenario) {
    switch (scenario) {
    case Scenario::success:
    case Scenario::drip:
        return {processing, hello_chunk, world_chunk, stop_chunk, usage_chunk, done_event};
    case Scenario::success_no_cost:
        return {processing, hello_chunk, world_chunk, stop_chunk, usage_no_cost_chunk, done_event};
    case Scenario::midstream_error:
        return {processing, partial_chunk, error_chunk, usage_after_error_chunk};
    case Scenario::early_eof:
    case Scenario::slow:
        return {processing, hello_chunk};
    case Scenario::length_empty:
        return {processing, length_chunk, usage_length_chunk, done_event};
    case Scenario::http_error:
    case Scenario::error_in_200:
        return {};
    }
    return {};
}

std::string payment_error_body() {
    return R"({"error":{"code":402,"message":"Insufficient credits for )" + fake_api_key() +
           R"(","metadata":{"error_type":"payment_required"}}})";
}

bool send_event(httplib::DataSink& sink, std::string_view text) {
    return sink.is_writable() && sink.write(text.data(), text.size());
}

} // namespace

std::string fake_api_key() {
    std::string key = "sk-or-v1-";
    for (int repeat = 0; repeat < hex_repeats; ++repeat) {
        key += "0123456789abcdef";
    }
    return key;
}

MockServer::MockServer()
    : generation_otherwise_(http_ok), server_(std::make_unique<httplib::Server>()) {
    server_->set_keep_alive_timeout(keep_alive_seconds);
    server_->Get("/api/v1/models", [](const httplib::Request&, httplib::Response& response) {
        response.set_content(std::string(catalog_body), json_type);
    });
    server_->Get("/api/v1/key", [](const httplib::Request&, httplib::Response& response) {
        response.set_content(std::string(key_body), json_type);
    });
    server_->Get("/api/v1/generation",
                 [this](const httplib::Request& request, httplib::Response& response) {
                     handle_generation(request, response);
                 });
    server_->Post("/api/v1/chat/completions",
                  [this](const httplib::Request& request, httplib::Response& response) {
                      handle_chat(request, response);
                  });

    port_ = server_->bind_to_any_port(loopback);
    if (port_ <= 0) {
        throw std::runtime_error("mock server could not bind a loopback port");
    }
    listener_ = std::jthread([this] { server_->listen_after_bind(); });
    server_->wait_until_ready();
}

MockServer::~MockServer() {
    stopping_.store(true);
    server_->stop();
    if (listener_.joinable()) {
        listener_.join();
    }
}

void MockServer::set_scenario(Scenario scenario) {
    const std::scoped_lock lock(mutex_);
    scenario_ = scenario;
}

void MockServer::set_hold(std::chrono::milliseconds hold) {
    const std::scoped_lock lock(mutex_);
    hold_ = hold;
}

void MockServer::set_generation_statuses(std::vector<int> leading, int otherwise) {
    const std::scoped_lock lock(mutex_);
    generation_leading_ = std::move(leading);
    generation_otherwise_ = otherwise;
}

int MockServer::post_count() const {
    const std::scoped_lock lock(mutex_);
    return static_cast<int>(posts_.size());
}

int MockServer::generation_count() const {
    const std::scoped_lock lock(mutex_);
    return generation_count_;
}

std::vector<nlohmann::json> MockServer::posts() const {
    const std::scoped_lock lock(mutex_);
    return posts_;
}

nlohmann::json MockServer::last_post() const {
    const std::scoped_lock lock(mutex_);
    return posts_.empty() ? nlohmann::json() : posts_.back();
}

std::string MockServer::last_authorization() const {
    const std::scoped_lock lock(mutex_);
    return last_authorization_;
}

std::string MockServer::last_generation_id() const {
    const std::scoped_lock lock(mutex_);
    return last_generation_id_;
}

void MockServer::handle_generation(const httplib::Request& request, httplib::Response& response) {
    int status = http_ok;
    {
        const std::scoped_lock lock(mutex_);
        last_generation_id_ = request.get_param_value("id");
        const auto index = static_cast<std::size_t>(generation_count_);
        status = index < generation_leading_.size() ? generation_leading_.at(index)
                                                    : generation_otherwise_;
        ++generation_count_;
    }
    response.status = status;
    response.set_content(std::string(status == http_ok ? generation_body : generation_missing_body),
                         json_type);
}

void MockServer::handle_chat(const httplib::Request& request, httplib::Response& response) {
    Scenario scenario = Scenario::success;
    std::chrono::milliseconds hold{0};
    {
        const std::scoped_lock lock(mutex_);
        posts_.push_back(nlohmann::json::parse(request.body, nullptr, false));
        last_authorization_ = request.get_header_value("Authorization");
        scenario = scenario_;
        hold = hold_;
    }
    if (scenario == Scenario::http_error) {
        response.status = http_payment_required;
        response.set_content(payment_error_body(), json_type);
        return;
    }
    if (scenario == Scenario::error_in_200) {
        response.status = http_ok;
        response.set_content(std::string(provider_error_body), json_type);
        return;
    }
    response.set_header("X-Generation-Id", generation_id);
    response.set_chunked_content_provider(
        event_stream_type, [this, scenario, hold](std::size_t /*offset*/, httplib::DataSink& sink) {
            return stream(scenario, hold, sink);
        });
}

// Writes the whole scenario in one provider call. Returning false drops the connection, which
// is how a client disconnect or server shutdown ends a held stream; sink.done() ends it cleanly
// so "EOF without [DONE]" scenarios reach the client as a normal end of body.
bool MockServer::stream(Scenario scenario, std::chrono::milliseconds hold,
                        httplib::DataSink& sink) const {
    if (scenario == Scenario::drip && !drip(hold, sink)) {
        return false;
    }
    for (const std::string_view event : events_for(scenario)) {
        if (!send_event(sink, event)) {
            return false;
        }
    }
    if (scenario == Scenario::slow && !wait_while_connected(hold, sink)) {
        return false;
    }
    sink.done();
    return true;
}

bool MockServer::drip(std::chrono::milliseconds hold, httplib::DataSink& sink) const {
    const auto deadline = Clock::now() + hold;
    while (Clock::now() < deadline) {
        if (!send_event(sink, processing) || !wait_while_connected(keepalive_interval, sink)) {
            return false;
        }
    }
    return true;
}

bool MockServer::wait_while_connected(std::chrono::milliseconds duration,
                                      httplib::DataSink& sink) const {
    const auto deadline = Clock::now() + duration;
    while (Clock::now() < deadline) {
        if (stopping_.load() || !sink.is_writable()) {
            return false;
        }
        std::this_thread::sleep_for(
            std::min<Clock::duration>(check_interval, deadline - Clock::now()));
    }
    return true;
}

} // namespace mock
