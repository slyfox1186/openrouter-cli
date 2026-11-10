#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace httplib {
class DataSink;
class Request;
class Response;
class Server;
} // namespace httplib

namespace mock {

// What POST /api/v1/chat/completions answers with when no route override is set.
enum class Scenario {
    success,         // "Hello", " world", stop, usage with cost 0.0042, [DONE]
    success_no_cost, // as success, but usage.cost is null
    http_error,      // HTTP 402 with a JSON error body that echoes a key-shaped string
    error_in_200,    // HTTP 200, Content-Type application/json, JSON error body
    midstream_error, // "Partial", an error chunk, usage with cost 0.0031, then EOF without [DONE]
    early_eof,       // "Hello", then EOF without a finish reason or [DONE]
    length_empty,    // finish_reason "length", no text, all 8000 output tokens spent on reasoning
    slow,            // "Hello", then silence for the hold time
    drip,            // keepalive comments for the hold time, then as success
    gated,           // "Hello", keepalives until release_gate(), then as success
    large,           // large_answer_bytes of "x" in many chunks, then stop, usage, [DONE]
    provider_first,  // as success, with the provider named before any answer text
};

inline constexpr std::size_t large_answer_bytes = 16384;
inline constexpr std::string_view creator_user_id = "user_test_1";
inline constexpr std::string_view masked_key_label = "sk-or-v1-0e6...1c96";

// A well-formed but fake OpenRouter key ("sk-or-v1-" and 64 hex digits), assembled at run time
// so no key-shaped literal sits in the source for secret scanners to flag.
std::string fake_api_key();
// The same shape with every hex digit equal to `digit`, so tests can tell keys apart.
std::string fake_key(char digit);
// The key POST /auth/keys hands out by default.
std::string login_key();

// The `data` object GET /key returns by default: a valid inference key owned by an
// organization. Tests edit a copy to build other replies.
nlohmann::json key_data();
// One row of GET /organization/members.
nlohmann::json member(std::string_view id, std::string_view email);

struct Reply {
    int status = 0;
    std::string body{}; // served as application/json
};

struct RecordedRequest {
    std::string method;
    std::string path; // after /api/v1, e.g. "/key"
    std::multimap<std::string, std::string> params;
    std::string authorization; // empty when the header was absent
    std::string body;

    [[nodiscard]] std::string param(const std::string& name) const; // first value or ""
};

using Responder = std::function<Reply(const RecordedRequest&)>;

// Loopback stand-in for the OpenRouter API under /api/v1. Every request is recorded. Built-in
// replies: GET models, models/{id}/endpoints, key, generation, keys (two pages), credits,
// activity (dates relative to today, UTC), organization/members (empty), POST auth/keys, and
// POST chat/completions per Scenario. set_route replaces any of them. Serves until destroyed.
class MockServer {
public:
    MockServer();
    MockServer(const MockServer&) = delete;
    MockServer& operator=(const MockServer&) = delete;
    MockServer(MockServer&&) = delete;
    MockServer& operator=(MockServer&&) = delete;
    ~MockServer();

    [[nodiscard]] int port() const { return port_; }

    void set_scenario(Scenario scenario);
    // How long `slow` stays silent after its first chunk and how long `drip` sends keepalives.
    void set_hold(std::chrono::milliseconds hold);
    // HTTP statuses for successive built-in GET /generation replies; `otherwise` answers every
    // later call.
    void set_generation_statuses(std::vector<int> leading, int otherwise);
    // Answers every request to `path` (after /api/v1) with the responder instead.
    void set_route(const std::string& path, Responder responder);
    void release_gate();

    [[nodiscard]] std::vector<RecordedRequest> requests() const;
    [[nodiscard]] std::vector<RecordedRequest> requests_to(std::string_view path) const;
    [[nodiscard]] int post_count() const; // POST /chat/completions
    [[nodiscard]] int generation_count() const;
    [[nodiscard]] std::vector<nlohmann::json> posts() const; // parsed completion bodies
    [[nodiscard]] nlohmann::json last_post() const;          // null before the first POST
    [[nodiscard]] std::string last_authorization() const;    // of the last completion POST
    [[nodiscard]] std::string last_generation_id() const;

private:
    void handle(const httplib::Request& request, httplib::Response& response);
    Reply built_in(const RecordedRequest& request);
    void handle_chat(httplib::Response& response);
    bool stream(Scenario scenario, std::chrono::milliseconds hold, httplib::DataSink& sink) const;
    bool drip(std::chrono::milliseconds hold, httplib::DataSink& sink) const;
    bool wait_for_gate(httplib::DataSink& sink) const;
    bool wait_while_connected(std::chrono::milliseconds duration, httplib::DataSink& sink) const;

    mutable std::mutex mutex_;
    Scenario scenario_ = Scenario::success;
    std::chrono::milliseconds hold_{0};
    std::vector<int> generation_leading_;
    int generation_otherwise_;
    int generation_served_ = 0;
    std::map<std::string, Responder> routes_;
    std::vector<RecordedRequest> requests_;
    // Let a provider waiting out a hold or a gate return as soon as it can.
    std::atomic<bool> gate_open_{false};
    std::atomic<bool> stopping_{false};
    std::unique_ptr<httplib::Server> server_;
    int port_ = 0;
    std::jthread listener_;
};

} // namespace mock
