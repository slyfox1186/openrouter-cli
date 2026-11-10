#include "http.hpp"
#include "sse.hpp"

#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;

class Socket {
public:
    explicit Socket(int descriptor) : descriptor_(descriptor) {}
    ~Socket() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&&) = delete;
    Socket& operator=(Socket&&) = delete;
    [[nodiscard]] int get() const { return descriptor_; }

private:
    int descriptor_;
};

struct Reply {
    std::string bytes;
    std::chrono::milliseconds delay{0};
};

bool send_all(int descriptor, std::string_view bytes) {
    while (!bytes.empty()) {
        const auto sent = ::send(descriptor, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent <= 0) {
            return false;
        }
        bytes.remove_prefix(static_cast<std::size_t>(sent));
    }
    return true;
}

// Raw responses make informational headers and trailers independent of a server library.
class Server {
public:
    explicit Server(std::vector<Reply> replies)
        : listener_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)),
          replies_(std::move(replies)) {
        REQUIRE(listener_.get() >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(::bind(listener_.get(), reinterpret_cast<const sockaddr*>(&address),
                       sizeof(address)) == 0);
        REQUIRE(::listen(listener_.get(), 4) == 0);
        socklen_t length = sizeof(address);
        REQUIRE(::getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&address), &length) ==
                0);
        port_ = ntohs(address.sin_port);
        worker_ = std::jthread([this](const std::stop_token& stop) { serve(stop); });
    }
    ~Server() {
        worker_.request_stop();
        worker_.join();
    }
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;
    [[nodiscard]] orcli::Config config() const {
        orcli::Config config;
        config.base_url = std::format("http://127.0.0.1:{}", port_);
        config.idle_timeout_seconds = 5;
        config.total_timeout_seconds = 5;
        return config;
    }
    [[nodiscard]] std::vector<std::string> requests() const {
        const std::lock_guard lock(mutex_);
        return requests_;
    }

private:
    static std::string receive_request(int descriptor) {
        std::array<char, 4096> buffer{};
        std::string request;
        std::size_t expected = std::string::npos;
        while (expected == std::string::npos || request.size() < expected) {
            pollfd ready{.fd = descriptor, .events = POLLIN, .revents = 0};
            if (::poll(&ready, 1, 200) <= 0) {
                break;
            }
            const auto count = ::recv(descriptor, buffer.data(), buffer.size(), 0);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                break;
            }
            request.append(buffer.data(), static_cast<std::size_t>(count));
            const auto end = request.find("\r\n\r\n");
            if (end != std::string::npos) {
                expected = end + 4;
                constexpr std::string_view field = "Content-Length: ";
                const auto length = request.find(field);
                if (length != std::string::npos && length < end) {
                    const auto text = std::string_view{request}.substr(length + field.size());
                    std::size_t body_size = 0;
                    const auto parsed =
                        std::from_chars(text.data(), std::to_address(text.end()), body_size);
                    if (parsed.ec != std::errc{}) {
                        break;
                    }
                    expected += body_size;
                }
            }
        }
        return request;
    }
    void serve(const std::stop_token& stop) {
        for (const auto& reply : replies_) {
            while (!stop.stop_requested()) {
                pollfd ready{.fd = listener_.get(), .events = POLLIN, .revents = 0};
                if (::poll(&ready, 1, 20) > 0) {
                    break;
                }
            }
            if (stop.stop_requested()) {
                return;
            }
            const Socket client(::accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC));
            if (client.get() < 0) {
                return;
            }
            const auto request = receive_request(client.get());
            {
                const std::lock_guard lock(mutex_);
                requests_.push_back(request);
            }
            const auto until = std::chrono::steady_clock::now() + reply.delay;
            while (!stop.stop_requested() && std::chrono::steady_clock::now() < until) {
                std::this_thread::sleep_for(10ms);
            }
            if (stop.stop_requested()) {
                return;
            }
            send_all(client.get(), reply.bytes);
        }
    }
    Socket listener_;
    std::vector<Reply> replies_;
    std::uint16_t port_ = 0;
    mutable std::mutex mutex_;
    std::vector<std::string> requests_;
    std::jthread worker_;
};

struct Exchange {
    std::size_t connection = 0; // order of acceptance, starting at 0
    std::string request;
};

// Keeps every accepted connection open and answers each request on the connection it arrived
// on, so a test can tell which requests shared one. A request announcing "Expect: 100-continue"
// is answered when its headers end, before any body: the reply that makes libcurl send a POST
// a second time.
class PersistentServer {
public:
    explicit PersistentServer(std::string reply)
        : listener_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)), reply_(std::move(reply)) {
        REQUIRE(listener_.get() >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(::bind(listener_.get(), reinterpret_cast<const sockaddr*>(&address),
                       sizeof(address)) == 0);
        REQUIRE(::listen(listener_.get(), 4) == 0);
        socklen_t length = sizeof(address);
        REQUIRE(::getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&address), &length) ==
                0);
        port_ = ntohs(address.sin_port);
        worker_ = std::jthread([this](const std::stop_token& stop) { serve(stop); });
    }
    ~PersistentServer() {
        worker_.request_stop();
        worker_.join();
    }
    PersistentServer(const PersistentServer&) = delete;
    PersistentServer& operator=(const PersistentServer&) = delete;
    PersistentServer(PersistentServer&&) = delete;
    PersistentServer& operator=(PersistentServer&&) = delete;
    [[nodiscard]] orcli::Config config() const {
        orcli::Config config;
        config.base_url = std::format("http://127.0.0.1:{}", port_);
        config.idle_timeout_seconds = 5;
        config.total_timeout_seconds = 5;
        return config;
    }
    [[nodiscard]] std::vector<Exchange> exchanges() const {
        const std::lock_guard lock(mutex_);
        return exchanges_;
    }

private:
    struct Connection {
        std::unique_ptr<Socket> socket; // null once the client has closed it
        std::size_t number = 0;
        std::string pending;
    };

    // Bytes of the first complete request in pending, if one has arrived.
    static std::optional<std::size_t> request_length(std::string_view pending) {
        const auto end = pending.find("\r\n\r\n");
        if (end == std::string_view::npos) {
            return std::nullopt;
        }
        const std::size_t head = end + 4;
        const std::string_view headers = pending.substr(0, head);
        if (headers.find("Expect: 100-continue") != std::string_view::npos) {
            return head;
        }
        constexpr std::string_view field = "Content-Length: ";
        const auto at = headers.find(field);
        if (at == std::string_view::npos) {
            return head;
        }
        const std::string_view text = headers.substr(at + field.size());
        std::size_t body = 0;
        const auto parsed = std::from_chars(text.data(), std::to_address(text.end()), body);
        if (parsed.ec != std::errc{}) {
            return head;
        }
        return pending.size() >= head + body ? std::optional{head + body} : std::nullopt;
    }
    void answer(Connection& connection) {
        std::array<char, 65536> buffer{};
        const auto count = ::recv(connection.socket->get(), buffer.data(), buffer.size(), 0);
        if (count < 0 && errno == EINTR) {
            return;
        }
        if (count <= 0) {
            connection.socket.reset();
            return;
        }
        connection.pending.append(buffer.data(), static_cast<std::size_t>(count));
        while (const auto length = request_length(connection.pending)) {
            {
                const std::lock_guard lock(mutex_);
                exchanges_.push_back({.connection = connection.number,
                                      .request = connection.pending.substr(0, *length)});
            }
            connection.pending.erase(0, *length);
            if (!send_all(connection.socket->get(), reply_)) {
                connection.socket.reset();
                return;
            }
        }
    }
    void serve(const std::stop_token& stop) {
        std::vector<Connection> connections;
        std::size_t accepted = 0;
        while (!stop.stop_requested()) {
            std::vector<pollfd> ready{{.fd = listener_.get(), .events = POLLIN, .revents = 0}};
            for (const Connection& connection : connections) {
                ready.push_back({.fd = connection.socket->get(), .events = POLLIN, .revents = 0});
            }
            if (::poll(ready.data(), ready.size(), 20) <= 0) {
                continue;
            }
            for (std::size_t index = 0; index < connections.size(); ++index) {
                if (ready.at(index + 1).revents != 0) {
                    answer(connections.at(index));
                }
            }
            std::erase_if(connections, [](const Connection& connection) {
                return connection.socket == nullptr;
            });
            if ((ready.front().revents & POLLIN) != 0) {
                const int descriptor = ::accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC);
                if (descriptor >= 0) {
                    connections.push_back({.socket = std::make_unique<Socket>(descriptor),
                                           .number = accepted,
                                           .pending = {}});
                    ++accepted;
                }
            }
        }
    }
    Socket listener_;
    std::string reply_;
    std::uint16_t port_ = 0;
    mutable std::mutex mutex_;
    std::vector<Exchange> exchanges_;
    std::jthread worker_;
};

// A complete response that leaves the connection open.
std::string persistent_response(int status, std::string_view type, std::string_view body) {
    return std::format("HTTP/1.1 {} Test\r\nContent-Type: {}\r\nContent-Length: {}\r\n\r\n{}",
                       status, type, body.size(), body);
}

orcli::Http client(const orcli::Config& config) {
    return orcli::Http(config, {.inference = orcli::SecretString{"test-key"}, .management = {}});
}

std::string response(int status, std::string_view type, std::string_view body,
                     std::string_view extra = {}) {
    return std::format("HTTP/1.1 {} Test\r\nContent-Type: {}\r\nContent-Length: {}\r\n{}"
                       "Connection: close\r\n\r\n{}",
                       status, type, body.size(), extra, body);
}

constexpr std::string_view event = "data: [DONE]\n\n";
} // namespace

TEST_CASE("only successful exact SSE media types reach the body callback") {
    const orcli::CurlGlobal global;
    REQUIRE(global.ok());
    for (const auto& [status, type] :
         std::vector<std::pair<int, std::string>>{{400, "text/event-stream"},
                                                  {200, "text/event-stream-evil"},
                                                  {200, "application/json"}}) {
        Server server({{response(status, type, event)}});
        const auto config = server.config();
        auto http = client(config);
        std::string received;
        orcli::StreamHandlers handlers;
        handlers.on_body_bytes = [&](std::string_view bytes) { received += bytes; };
        const auto result = http.post_stream("/chat", "{}", handlers);
        CHECK_FALSE(result.event_stream);
        CHECK(received.empty());
        CHECK(result.error_body == event);
    }
}

TEST_CASE("informational headers and trailers cannot replace final response metadata") {
    const orcli::CurlGlobal global;
    REQUIRE(global.ok());
    const auto wire =
        std::string{
            "HTTP/1.1 103 Early Hints\r\nX-Generation-Id: gen-early\r\n"
            "Content-Type: application/json\r\n\r\nHTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream; charset=utf-8\r\nX-Generation-Id: gen-final\r\n"
            "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"} +
        std::format("{:x}\r\n{}\r\n0\r\nContent-Type: application/json\r\n"
                    "X-Generation-Id: gen-trailer\r\n\r\n",
                    event.size(), event);
    Server server({{wire}});
    const auto config = server.config();
    auto http = client(config);
    std::string generation;
    std::string received;
    orcli::StreamHandlers handlers;
    handlers.on_generation_id = [&](std::string_view id) { generation = id; };
    handlers.on_body_bytes = [&](std::string_view bytes) {
        CHECK(generation == "gen-final");
        received += bytes;
    };
    const auto result = http.post_stream("/chat", "{}", handlers);
    CHECK(result.transport_error.empty());
    CHECK(result.event_stream);
    CHECK(generation == "gen-final");
    CHECK(received == event);
}

TEST_CASE("final headers publish recovery identifiers even when the body never arrives") {
    const orcli::CurlGlobal global;
    REQUIRE(global.ok());
    Server server({{"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                    "Content-Length: 100\r\nX-Generation-Id: gen-recover\r\n\r\n"}});
    const auto config = server.config();
    auto http = client(config);
    std::string generation;
    orcli::StreamHandlers handlers;
    handlers.on_generation_id = [&](std::string_view id) { generation = id; };
    const auto result = http.post_stream("/chat", "{}", handlers);
    CHECK(generation == "gen-recover");
    CHECK_FALSE(result.transport_error.empty());
    CHECK(server.requests().size() == 1);
}

TEST_CASE("invalid recovery identifiers are not published") {
    const orcli::CurlGlobal global;
    REQUIRE(global.ok());
    Server server({{response(200, "text/event-stream", event,
                             "X-Generation-Id: private text with spaces\r\n")}});
    const auto config = server.config();
    auto http = client(config);
    bool published = false;
    std::size_t received = 0;
    orcli::StreamHandlers handlers;
    handlers.on_generation_id = [&](std::string_view) { published = true; };
    handlers.on_body_bytes = [&](std::string_view bytes) { received += bytes.size(); };
    const auto result = http.post_stream("/chat", "{}", handlers);
    CHECK(result.transport_error.empty());
    CHECK_FALSE(published);
    CHECK(received == event.size());
}

TEST_CASE("malformed JSON aborts a stream through its continuation callback") {
    const orcli::CurlGlobal global;
    REQUIRE(global.ok());
    Server server({{response(200, "text/event-stream", "data: {bad json}\n\n")}});
    const auto config = server.config();
    auto http = client(config);
    orcli::SseParser parser;
    orcli::ChatStream chat;
    bool failed = false;
    orcli::StreamHandlers handlers;
    handlers.on_body_bytes = [&](std::string_view bytes) {
        const auto payloads = parser.feed(bytes);
        if (!payloads) {
            failed = true;
            return;
        }
        for (const auto& payload : *payloads) {
            if (!chat.on_data(payload)) {
                failed = true;
                return;
            }
        }
    };
    handlers.should_continue = [&] { return !failed; };
    const auto result = http.post_stream("/chat", "{}", handlers);
    CHECK(failed);
    CHECK_FALSE(result.transport_error.empty());
    CHECK(chat.outcome().text.empty());
}

TEST_CASE("progress diagnostics may fail without aborting a response") {
    const orcli::CurlGlobal global;
    REQUIRE(global.ok());
    Server server({{response(200, "text/event-stream", event)}});
    const auto config = server.config();
    auto http = client(config);
    std::string received;
    orcli::StreamHandlers handlers;
    handlers.on_progress = [] { throw std::runtime_error("private diagnostic"); };
    handlers.on_body_bytes = [&](std::string_view bytes) { received += bytes; };
    const auto result = http.post_stream("/chat", "{}", handlers);
    CHECK(result.transport_error.empty());
    CHECK(received == event);
}

TEST_CASE("processing exceptions are local errors and continuation may stop a transfer") {
    const orcli::CurlGlobal global;
    REQUIRE(global.ok());
    Server server({{response(200, "text/event-stream", event)}});
    const auto config = server.config();
    auto http = client(config);
    orcli::StreamHandlers handlers;
    bool processed = false;
    SUBCASE("exception") {
        handlers.on_body_bytes = [](std::string_view) {
            throw std::runtime_error("private prompt");
        };
    }
    SUBCASE("continuation") {
        handlers.on_body_bytes = [&](std::string_view) { processed = true; };
        handlers.should_continue = [&] { return !processed; };
    }
    const auto result = http.post_stream("/chat", "{}", handlers);
    CHECK_FALSE(result.transport_error.empty());
    CHECK(result.transport_error.find("private") == std::string::npos);
}

TEST_CASE("non SSE bodies and total streamed bytes have enforced limits") {
    const orcli::CurlGlobal global;
    REQUIRE(global.ok());
    std::size_t limit = 0;
    std::string type;
    SUBCASE("error body") {
        limit = std::size_t{1} << 20U;
        type = "application/json";
    }
    SUBCASE("stream total") {
        limit = std::size_t{64} << 20U;
        type = "text/event-stream";
    }
    Server server({{response(200, type, std::string(limit + 1, 'a'))}});
    const auto config = server.config();
    auto http = client(config);
    std::size_t received = 0;
    orcli::StreamHandlers handlers;
    handlers.on_body_bytes = [&](std::string_view bytes) { received += bytes.size(); };
    const auto result = http.post_stream("/chat", "{}", handlers);
    CHECK_FALSE(result.transport_error.empty());
    CHECK(received <= limit);
    CHECK(result.error_body.size() <= limit);
}

TEST_CASE("lookup window bounds the first request and retries including retry-after") {
    const orcli::CurlGlobal global;
    REQUIRE(global.ok());
    std::vector<Reply> replies;
    bool first_timeout = false;
    SUBCASE("first transfer") {
        first_timeout = true;
        replies = {{response(200, "application/json", "{}"), 2500ms}};
    }
    SUBCASE("retry-after wait preserves completed response") {
        replies = {{response(404, "application/json", "last", "Retry-After: 60\r\n")}};
    }
    SUBCASE("timed out retry preserves completed response") {
        replies = {{response(404, "application/json", "last")},
                   {response(200, "application/json", "{}"), 2500ms}};
    }
    Server server(std::move(replies));
    const auto config = server.config();
    auto http = client(config);
    const auto started = std::chrono::steady_clock::now();
    const auto result = http.fetch("/generation", {.auth = orcli::Auth::none, .retry_404_for = 2s});
    CHECK(std::chrono::steady_clock::now() - started < 4s);
    if (first_timeout) {
        CHECK_FALSE(result.has_value());
    } else {
        REQUIRE(result.has_value());
        CHECK(result->status == 404);
        CHECK(result->body == "last");
    }
}

TEST_CASE("both POST forms are never retried and use the exact user agent") {
    const orcli::CurlGlobal global;
    REQUIRE(global.ok());
    Server server(
        {{response(503, "application/json", R"({"error":{"message":"private prompt"}})")}});
    const auto config = server.config();
    auto http = client(config);
    SUBCASE("stream") {
        const auto result = http.post_stream("/chat", "{}", {});
        CHECK(result.status == 503);
    }
    SUBCASE("JSON") {
        const auto result = http.post_json("/keys", "{}", orcli::Auth::none);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message.find("private") == std::string::npos);
        CHECK(result.error().message.find("routing") != std::string::npos);
    }
    const auto requests = server.requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests.front().find("User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:159.0) "
                                "Gecko/20100101 Firefox/159.0\r\n") != std::string::npos);
}

TEST_CASE("GET error bodies are bounded and never copied into diagnostic text") {
    const orcli::CurlGlobal global;
    REQUIRE(global.ok());
    const std::string body((std::size_t{1} << 20U) + 1, 'x');
    Server server({{response(400, "application/json", body)}});
    const auto config = server.config();
    auto http = client(config);
    const auto result = http.get("/models", {.auth = orcli::Auth::none});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("1048576") != std::string::npos);
    const auto requests = server.requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests.front().find("User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:159.0) "
                                "Gecko/20100101 Firefox/159.0\r\n") != std::string::npos);
}

TEST_CASE("GET requests on one client share a connection") {
    const orcli::CurlGlobal global;
    REQUIRE(global.ok());
    PersistentServer server(persistent_response(200, "application/json", "{}"));
    const auto config = server.config();
    auto http = client(config);
    REQUIRE(http.fetch("/first", {.auth = orcli::Auth::none}).has_value());
    REQUIRE(http.fetch("/second", {.auth = orcli::Auth::none}).has_value());
    const auto exchanges = server.exchanges();
    REQUIRE(exchanges.size() == 2);
    CHECK(exchanges.at(0).connection == exchanges.at(1).connection);
}

TEST_CASE("a POST opens its own connection and a later GET opens no third one") {
    const orcli::CurlGlobal global;
    REQUIRE(global.ok());
    PersistentServer server(persistent_response(200, "application/json", "{}"));
    const auto config = server.config();
    auto http = client(config);
    REQUIRE(http.fetch("/before", {.auth = orcli::Auth::none}).has_value());
    SUBCASE("stream") {
        const auto result = http.post_stream("/chat", "{}", {});
        CHECK(result.status == 200);
        CHECK(result.transport_error.empty());
    }
    SUBCASE("JSON") {
        CHECK(http.post_json("/keys", "{}", orcli::Auth::none).has_value());
    }
    REQUIRE(http.fetch("/after", {.auth = orcli::Auth::none}).has_value());
    const auto exchanges = server.exchanges();
    REQUIRE(exchanges.size() == 3);
    CHECK(exchanges.at(1).request.starts_with("POST "));
    CHECK(exchanges.at(1).connection != exchanges.at(0).connection);
    CHECK(exchanges.at(2).request.starts_with("GET /after "));
    CHECK(exchanges.at(2).request.find("Content-Length") == std::string::npos);
    CHECK(exchanges.at(2).connection < 2);
}

TEST_CASE("a large POST is sent once, without an Expect header, when the server answers 417") {
    const orcli::CurlGlobal global;
    REQUIRE(global.ok());
    PersistentServer server(persistent_response(417, "application/json", "{}"));
    const auto config = server.config();
    auto http = client(config);
    const std::string body((std::size_t{1} << 20U) + 1, 'x');
    SUBCASE("stream") {
        const auto result = http.post_stream("/chat", body, {});
        CHECK(result.status == 417);
        CHECK(result.transport_error.empty());
    }
    SUBCASE("JSON") {
        const auto result = http.post_json("/keys", body, orcli::Auth::none);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message.find("417") != std::string::npos);
    }
    const auto exchanges = server.exchanges();
    REQUIRE(exchanges.size() == 1);
    CHECK(exchanges.front().request.find("Expect") == std::string::npos);
    CHECK(exchanges.front().request.size() > body.size());
}

TEST_CASE("each request on the shared handle starts from clean options") {
    const orcli::CurlGlobal global;
    REQUIRE(global.ok());
    PersistentServer server(persistent_response(200, "text/event-stream", event));
    const auto config = server.config();
    auto http = client(config);
    REQUIRE(http.fetch("/keyed", {.auth = orcli::Auth::inference}).has_value());
    REQUIRE(http.fetch("/public", {.auth = orcli::Auth::none}).has_value());
    // The stream stops itself once its body has arrived, so the server has seen the request.
    bool received = false;
    orcli::StreamHandlers handlers;
    handlers.on_body_bytes = [&](std::string_view) { received = true; };
    handlers.should_continue = [&] { return !received; };
    const auto aborted = http.post_stream("/chat", "{}", handlers);
    CHECK(received);
    CHECK_FALSE(aborted.transport_error.empty());
    REQUIRE(http.fetch("/after", {.auth = orcli::Auth::none}).has_value());
    const auto exchanges = server.exchanges();
    REQUIRE(exchanges.size() == 4);
    CHECK(exchanges.at(0).request.find("Authorization: Bearer test-key\r\n") != std::string::npos);
    CHECK(exchanges.at(1).request.find("Authorization") == std::string::npos);
    CHECK(exchanges.at(2).request.starts_with("POST /chat "));
    CHECK(exchanges.at(3).request.starts_with("GET /after "));
    CHECK(exchanges.at(3).request.find("Authorization") == std::string::npos);
    CHECK(exchanges.at(3).request.find("text/event-stream") == std::string::npos);
    CHECK(exchanges.at(3).request.find("Content-Length") == std::string::npos);
}

TEST_CASE("the idle clock counts received bytes and upload progress, not a repeated total") {
    using Clock = orcli::ActivityClock::Clock;
    const Clock::time_point start{};
    orcli::ActivityClock clock(start, 10s);
    CHECK_FALSE(clock.idle(start + 10s));
    CHECK(clock.idle(start + 11s));
    clock.uploaded(100, start + 8s);
    CHECK_FALSE(clock.idle(start + 18s));
    clock.uploaded(100, start + 17s);
    CHECK(clock.idle(start + 19s));
    clock.uploaded(101, start + 19s);
    CHECK_FALSE(clock.idle(start + 29s));
    clock.received(start + 25s);
    CHECK_FALSE(clock.idle(start + 35s));
    CHECK(clock.idle(start + 36s));
}
