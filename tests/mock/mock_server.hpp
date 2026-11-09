#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace httplib {
class DataSink;
class Request;
class Response;
class Server;
} // namespace httplib

namespace mock {

// What POST /api/v1/chat/completions answers with.
enum class Scenario {
    success,         // "Hello", " world", stop, usage with cost 0.0042, [DONE]
    success_no_cost, // as success, but usage.cost is null
    http_error,      // HTTP 402 with a JSON error body
    error_in_200,    // HTTP 200, Content-Type application/json, JSON error body
    midstream_error, // "Partial", an error chunk, usage with cost 0.0031, then EOF without [DONE]
    early_eof,       // "Hello", then EOF without a finish reason or [DONE]
    length_empty,    // finish_reason "length", no text, all 8000 output tokens spent on reasoning
    slow,            // "Hello", then silence for the hold time
    drip,            // keepalive comments for the hold time, then as success
};

// A well-formed but fake OpenRouter key, assembled at run time so no key-shaped literal sits in
// the source for secret scanners to flag.
std::string fake_api_key();

// Loopback stand-in for the OpenRouter API under /api/v1: GET models, generation and key, and
// POST chat/completions. Serves until destroyed.
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
    // HTTP statuses for successive GET /generation calls; `otherwise` answers every later call.
    void set_generation_statuses(std::vector<int> leading, int otherwise);

    [[nodiscard]] int post_count() const;
    [[nodiscard]] int generation_count() const;
    [[nodiscard]] std::vector<nlohmann::json> posts() const; // parsed bodies, in arrival order
    [[nodiscard]] nlohmann::json last_post() const;          // null before the first POST
    [[nodiscard]] std::string last_authorization() const;
    [[nodiscard]] std::string last_generation_id() const;

private:
    void handle_chat(const httplib::Request& request, httplib::Response& response);
    void handle_generation(const httplib::Request& request, httplib::Response& response);
    bool stream(Scenario scenario, std::chrono::milliseconds hold, httplib::DataSink& sink) const;
    bool drip(std::chrono::milliseconds hold, httplib::DataSink& sink) const;
    bool wait_while_connected(std::chrono::milliseconds duration, httplib::DataSink& sink) const;

    mutable std::mutex mutex_;
    Scenario scenario_ = Scenario::success;
    std::chrono::milliseconds hold_{0};
    std::vector<int> generation_leading_;
    int generation_otherwise_;
    int generation_count_ = 0;
    std::vector<nlohmann::json> posts_;
    std::string last_authorization_;
    std::string last_generation_id_;
    // Lets a provider that is waiting out a hold return as soon as the server shuts down.
    std::atomic<bool> stopping_{false};
    std::unique_ptr<httplib::Server> server_;
    int port_ = 0;
    std::jthread listener_;
};

} // namespace mock
