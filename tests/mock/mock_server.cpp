#include "mock_server.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <format>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace mock {
namespace {

using Clock = std::chrono::steady_clock;
using nlohmann::json;

constexpr int http_ok = 200;
constexpr int http_payment_required = 402;
constexpr int http_not_found = 404;
constexpr int hex_repeats = 4;
constexpr std::size_t key_hex_digits = 64;
constexpr std::size_t large_chunk_bytes = 512;
constexpr int keys_first_page_rows = 2;
constexpr time_t keep_alive_seconds = 1;
constexpr std::chrono::milliseconds keepalive_interval{200};
constexpr std::chrono::milliseconds check_interval{50};
constexpr const char* loopback = "127.0.0.1";
constexpr const char* json_type = "application/json";
constexpr const char* event_stream_type = "text/event-stream";
constexpr const char* generation_id = "gen-test-1";
constexpr const char* api_routes = R"(/api/v1/.*)";
constexpr std::string_view api_prefix = "/api/v1";
constexpr std::string_view chat_path = "/chat/completions";
constexpr std::string_view generation_path = "/generation";
constexpr std::string_view models_prefix = "/models/";
constexpr std::string_view endpoints_suffix = "/endpoints";

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

constexpr std::string_view provider_error_body =
    R"({"error":{"code":502,"message":"Upstream provider failed",)"
    R"("metadata":{"error_type":"provider_error"}}})";

constexpr std::string_view processing = ": OPENROUTER PROCESSING\n\n";

constexpr std::string_view hello_chunk =
    R"(data: {"id":"gen-test-1","model":"test/alpha-1","choices":[{"index":0,)"
    R"("delta":{"role":"assistant","content":"Hello"},"finish_reason":null}]})"
    "\n\n";

constexpr std::string_view routed_chunk =
    R"(data: {"id":"gen-test-1","model":"test/alpha-1","choices":[],)"
    R"("openrouter_metadata":{"endpoints":{"available":[)"
    R"({"provider":"MockProvider","selected":true}]}}})"
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
constexpr std::string_view credits_body = R"({"data":{"total_credits":20,"total_usage":7.5}})";
constexpr std::string_view empty_page = R"({"data":[]})";
constexpr std::string_view not_found_body = R"({"error":{"code":404,"message":"Not Found"}})";

std::string content_chunk(std::string_view text) {
    return R"(data: {"id":"gen-test-1","model":"test/alpha-1","choices":[{"index":0,)"
           R"("delta":{"content":")" +
           std::string(text) + R"("},"finish_reason":null}]})" + "\n\n";
}

std::vector<std::string> owned(std::initializer_list<std::string_view> events) {
    return std::vector<std::string>(events.begin(), events.end());
}

std::vector<std::string> large_events() {
    std::vector<std::string> events{std::string(processing)};
    const std::string chunk = content_chunk(std::string(large_chunk_bytes, 'x'));
    for (std::size_t sent = 0; sent < large_answer_bytes; sent += large_chunk_bytes) {
        events.push_back(chunk);
    }
    for (const std::string_view tail : {stop_chunk, usage_chunk, done_event}) {
        events.emplace_back(tail);
    }
    return events;
}

// The events sent after any drip or gate phase.
std::vector<std::string> events_for(Scenario scenario) {
    switch (scenario) {
    case Scenario::success:
    case Scenario::drip:
        return owned({processing, hello_chunk, world_chunk, stop_chunk, usage_chunk, done_event});
    case Scenario::provider_first:
        return owned({processing, routed_chunk, hello_chunk, world_chunk, stop_chunk, usage_chunk,
                      done_event});
    case Scenario::gated:
        return owned({world_chunk, stop_chunk, usage_chunk, done_event});
    case Scenario::success_no_cost:
        return owned(
            {processing, hello_chunk, world_chunk, stop_chunk, usage_no_cost_chunk, done_event});
    case Scenario::midstream_error:
        return owned({processing, partial_chunk, error_chunk, usage_after_error_chunk});
    case Scenario::early_eof:
    case Scenario::slow:
        return owned({processing, hello_chunk});
    case Scenario::length_empty:
        return owned({processing, length_chunk, usage_length_chunk, done_event});
    case Scenario::large:
        return large_events();
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

bool send_all(httplib::DataSink& sink, const std::vector<std::string>& events) {
    return std::ranges::all_of(
        events, [&sink](const std::string& event) { return send_event(sink, event); });
}

int offset_of(const RecordedRequest& request) {
    const std::string text = request.param("offset");
    int offset = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), offset);
    return parsed.ec == std::errc{} ? offset : 0;
}

json endpoint(std::string_view tag, std::string_view provider) {
    return {{"name", std::string(provider) + " endpoint"},
            {"tag", std::string(tag)},
            {"provider_name", std::string(provider)},
            {"pricing", {{"prompt", "0.0000025"}, {"completion", "0.00001"}}},
            {"context_length", 128000},
            {"max_completion_tokens", 8000},
            {"service_tier", "standard"},
            {"status", 0}};
}

std::string endpoints_body(std::string_view path) {
    const std::string model_id(path.substr(
        models_prefix.size(), path.size() - models_prefix.size() - endpoints_suffix.size()));
    const json endpoints =
        json::array({endpoint("mockcloud/fp8", "MockCloud"), endpoint("othercloud", "OtherCloud")});
    return json{{"data", {{"id", model_id}, {"name", model_id}, {"endpoints", endpoints}}}}.dump();
}

json key_entry(std::string_view name, bool disabled) {
    return {{"hash", "hash-" + std::string(name)},
            {"name", std::string(name)},
            {"label", "sk-or-v1-" + std::string(name) + "...0000"},
            {"disabled", disabled},
            {"limit", nullptr},
            {"limit_remaining", nullptr},
            {"limit_reset", nullptr},
            {"usage", 1.25},
            {"usage_daily", 0.25},
            {"usage_weekly", 0.5},
            {"usage_monthly", 1.0},
            {"created_at", "2026-10-01T12:00:00Z"},
            {"updated_at", nullptr},
            {"expires_at", nullptr}};
}

// Two pages, so a client that stops after the first page misses "old-key".
std::string keys_page(int offset) {
    if (offset == 0) {
        return json{
            {"data", json::array({key_entry("ci-runner", false), key_entry("laptop", false)})}}
            .dump();
    }
    if (offset == keys_first_page_rows) {
        return json{{"data", json::array({key_entry("old-key", true)})}}.dump();
    }
    return std::string(empty_page);
}

// The live API's row shape (2026-11-11): the date carries a time, and cached_tokens and
// byok_requests are present.
json activity_row(const std::string& date, std::string_view model, std::string_view endpoint_id,
                  long long requests, long long prompt_tokens, double usage) {
    return {{"date", date + " 00:00:00"},
            {"model", std::string(model)},
            {"model_permaslug", std::string(model)},
            {"endpoint_id", std::string(endpoint_id)},
            {"provider_name", "MockCloud"},
            {"usage", usage},
            {"byok_usage_inference", 0},
            {"requests", requests},
            {"prompt_tokens", prompt_tokens},
            {"completion_tokens", 50},
            {"reasoning_tokens", 10},
            {"cached_tokens", 0},
            {"byok_requests", 0}};
}

// Yesterday has two endpoint rows for one model and provider (requests 30 + 12 = 42, prompt
// tokens 1,000 + 234 = 1,234); five days ago has one test/beta-2 row.
std::string activity_body() {
    const auto today = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
    const std::string yesterday = std::format("{:%F}", today - std::chrono::days{1});
    const std::string five_days_ago = std::format("{:%F}", today - std::chrono::days{5});
    const json rows = json::array({
        activity_row(yesterday, "test/alpha-1", "ep-1", 30, 1000, 0.01),
        activity_row(yesterday, "test/alpha-1", "ep-2", 12, 234, 0.02),
        activity_row(five_days_ago, "test/beta-2", "ep-3", 2, 50, 0.005),
    });
    return json{{"data", rows}}.dump();
}

} // namespace

std::string RecordedRequest::param(const std::string& name) const {
    const auto found = params.find(name);
    return found == params.end() ? std::string() : found->second;
}

std::string fake_api_key() {
    std::string key = "sk-or-v1-";
    for (int repeat = 0; repeat < hex_repeats; ++repeat) {
        key += "0123456789abcdef";
    }
    return key;
}

std::string fake_key(char digit) {
    return "sk-or-v1-" + std::string(key_hex_digits, digit);
}

std::string login_key() {
    return fake_key('3');
}

std::string management_key() {
    return fake_key('2');
}

json key_data() {
    return {{"label", std::string(masked_key_label)},
            {"usage", 1.5},
            {"usage_daily", 0.25},
            {"usage_weekly", 0.5},
            {"usage_monthly", 1.0},
            {"limit", nullptr},
            {"limit_remaining", nullptr},
            {"limit_reset", nullptr},
            {"expires_at", nullptr},
            {"is_free_tier", false},
            {"is_provisioning_key", false},
            {"creator_user_id", std::string(creator_user_id)},
            {"organization_id", "org_test_1"},
            {"workspace_id", "ws_test_1"},
            {"is_management_key", false}};
}

json member(std::string_view id, std::string_view email) {
    return {{"id", std::string(id)},
            {"email", std::string(email)},
            {"first_name", "Pat"},
            {"last_name", "Lee"},
            {"role", "org:admin"}};
}

MockServer::MockServer()
    : generation_otherwise_(http_ok), server_(std::make_unique<httplib::Server>()) {
    server_->set_keep_alive_timeout(keep_alive_seconds);
    const auto handler = [this](const httplib::Request& request, httplib::Response& response) {
        handle(request, response);
    };
    server_->Get(api_routes, handler);
    server_->Post(api_routes, handler);

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

void MockServer::set_route(const std::string& path, Responder responder) {
    const std::scoped_lock lock(mutex_);
    routes_.insert_or_assign(path, std::move(responder));
}

void MockServer::release_gate() {
    gate_open_.store(true);
}

std::vector<RecordedRequest> MockServer::requests() const {
    const std::scoped_lock lock(mutex_);
    return requests_;
}

std::vector<RecordedRequest> MockServer::requests_to(std::string_view path) const {
    const std::scoped_lock lock(mutex_);
    std::vector<RecordedRequest> matching;
    std::ranges::copy_if(requests_, std::back_inserter(matching),
                         [path](const RecordedRequest& request) { return request.path == path; });
    return matching;
}

int MockServer::post_count() const {
    return static_cast<int>(requests_to(chat_path).size());
}

int MockServer::generation_count() const {
    return static_cast<int>(requests_to(generation_path).size());
}

std::vector<json> MockServer::posts() const {
    std::vector<json> bodies;
    for (const RecordedRequest& request : requests_to(chat_path)) {
        bodies.push_back(json::parse(request.body, nullptr, false));
    }
    return bodies;
}

json MockServer::last_post() const {
    const std::vector<json> bodies = posts();
    return bodies.empty() ? json() : bodies.back();
}

std::string MockServer::last_authorization() const {
    const std::vector<RecordedRequest> chats = requests_to(chat_path);
    return chats.empty() ? std::string() : chats.back().authorization;
}

std::string MockServer::last_generation_id() const {
    const std::vector<RecordedRequest> lookups = requests_to(generation_path);
    return lookups.empty() ? std::string() : lookups.back().param("id");
}

void MockServer::handle(const httplib::Request& request, httplib::Response& response) {
    RecordedRequest recorded{.method = request.method,
                             .path = request.path.substr(api_prefix.size()),
                             .params = {},
                             .authorization = request.get_header_value("Authorization"),
                             .body = request.body};
    for (const auto& [name, value] : request.params) {
        recorded.params.emplace(name, value);
    }
    Responder responder;
    {
        const std::scoped_lock lock(mutex_);
        requests_.push_back(recorded);
        if (const auto found = routes_.find(recorded.path); found != routes_.end()) {
            responder = found->second;
        }
    }
    if (responder) {
        const Reply reply = responder(recorded);
        response.status = reply.status;
        response.set_content(reply.body, json_type);
        return;
    }
    if (recorded.method == "POST" && recorded.path == chat_path) {
        handle_chat(response);
        return;
    }
    const Reply reply = built_in(recorded);
    response.status = reply.status;
    response.set_content(reply.body, json_type);
}

Reply MockServer::built_in(const RecordedRequest& request) {
    const std::string& path = request.path;
    if (path == "/models") {
        return {.status = http_ok, .body = std::string(catalog_body)};
    }
    if (path.starts_with(models_prefix) && path.ends_with(endpoints_suffix)) {
        return {.status = http_ok, .body = endpoints_body(path)};
    }
    if (path == "/key") {
        json data = key_data();
        data["is_management_key"] = request.authorization == "Bearer " + management_key();
        return {.status = http_ok, .body = json{{"data", data}}.dump()};
    }
    if (path == generation_path) {
        int status = http_ok;
        {
            const std::scoped_lock lock(mutex_);
            const auto index = static_cast<std::size_t>(generation_served_);
            status = index < generation_leading_.size() ? generation_leading_.at(index)
                                                        : generation_otherwise_;
            ++generation_served_;
        }
        return {.status = status,
                .body = std::string(status == http_ok ? generation_body : generation_missing_body)};
    }
    if (path == "/keys") {
        return {.status = http_ok, .body = keys_page(offset_of(request))};
    }
    if (path == "/credits") {
        return {.status = http_ok, .body = std::string(credits_body)};
    }
    if (path == "/activity") {
        return {.status = http_ok, .body = activity_body()};
    }
    if (path == "/organization/members") {
        return {.status = http_ok, .body = std::string(empty_page)};
    }
    if (path == "/auth/keys") {
        return {.status = http_ok,
                .body =
                    json{{"key", login_key()}, {"user_id", std::string(creator_user_id)}}.dump()};
    }
    return {.status = http_not_found, .body = std::string(not_found_body)};
}

void MockServer::handle_chat(httplib::Response& response) {
    Scenario scenario = Scenario::success;
    std::chrono::milliseconds hold{0};
    {
        const std::scoped_lock lock(mutex_);
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
    if (scenario == Scenario::gated &&
        !(send_all(sink, owned({processing, hello_chunk})) && wait_for_gate(sink))) {
        return false;
    }
    if (!send_all(sink, events_for(scenario))) {
        return false;
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

// Keepalives while waiting keep the client's idle timeout from firing, however long the test
// takes to act on the partial answer.
bool MockServer::wait_for_gate(httplib::DataSink& sink) const {
    while (!gate_open_.load()) {
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
