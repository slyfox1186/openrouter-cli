#ifndef ORCLI_HTTP_HPP
#define ORCLI_HTTP_HPP

#include "common.hpp"
#include "config.hpp"
#include "credentials.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace orcli {

// SIGPIPE is ignored; SIGINT, SIGTERM and SIGHUP set a flag that aborts transfers, and are
// unblocked in case a parent started this process with them blocked.
void install_signal_handlers();
bool interrupt_requested();

// curl_global_init/curl_global_cleanup; construct once in main before any transfer.
class CurlGlobal {
public:
    CurlGlobal();
    CurlGlobal(const CurlGlobal&) = delete;
    CurlGlobal& operator=(const CurlGlobal&) = delete;
    CurlGlobal(CurlGlobal&&) = delete;
    CurlGlobal& operator=(CurlGlobal&&) = delete;
    ~CurlGlobal();
    [[nodiscard]] bool ok() const { return ok_; }

private:
    bool ok_ = false;
};

// Which stored key a request carries. The mapping lives at each call site in main.cpp:
// none for public metadata and the PKCE exchange, inference for completions, /generation and
// /key, management for /keys, /credits, /activity and the /key check of a Management key. A
// management key is never sent to a completion or /generation request because each request
// names its kind and Http holds both separately.
enum class Auth : std::uint8_t {
    none,
    inference,
    management,
};

// Keys loaded by this process; an empty key means that kind was not loaded, and a request
// needing it fails with Exit::config naming the command that stores one.
struct HttpKeys {
    SecretString inference;
    SecretString management;
};

struct GetOptions {
    Auth auth = Auth::inference;
    // Positive values bound the whole lookup, including its first attempt, retries and waits.
    // Within this window 404 is retryable; the last complete response survives timeout.
    std::chrono::seconds retry_404_for{0};
};

// A completed HTTP exchange, whatever its status; callers that must tell 401 from 403/404 from
// 5xx (status, auth, account) branch on `status`, never on message text.
struct HttpResponse {
    long status = 0;
    std::string body;
};

struct StreamHandlers {
    std::function<void(std::string_view)> on_generation_id; // from X-Generation-Id
    std::function<void(std::string_view)> on_body_bytes;    // raw SSE bytes, any split
    std::function<bool()> should_continue; // false after a parsing or persistence failure
    std::function<void()> on_progress;     // diagnostics only; exceptions do not abort the response
};

struct StreamResult {
    long status = 0;
    bool event_stream = false; // Content-Type was text/event-stream
    std::string error_body;    // body (bounded) when status is not 2xx or not an event stream
    bool interrupted = false;
    bool idle_timeout = false;
    bool total_timeout = false;
    std::string transport_error; // curl error text, empty when the transfer completed
};

// Idle detection for a streamed call. Activity is a received byte or an advance of the upload
// total, so a large request on a slow link is not mistaken for a silent server; an upload that
// stops advancing still becomes idle.
class ActivityClock {
public:
    using Clock = std::chrono::steady_clock;

    ActivityClock(Clock::time_point start, Clock::duration limit) : last_(start), limit_(limit) {}

    void received(Clock::time_point now) { last_ = now; }

    // `total` is the count of request bytes sent so far.
    void uploaded(std::int64_t total, Clock::time_point now) {
        if (total > uploaded_) {
            uploaded_ = total;
            last_ = now;
        }
    }

    [[nodiscard]] bool idle(Clock::time_point now) const { return now - last_ > limit_; }

private:
    Clock::time_point last_;
    Clock::duration limit_;
    std::int64_t uploaded_ = 0;
};

// Closes a libcurl easy handle. Defined in http.cpp, so this header needs no libcurl include.
struct CurlHandleDeleter {
    void operator()(void* handle) const noexcept;
};

// One client keeps one libcurl handle, so its GET requests share a connection and TLS session
// instead of paying a handshake each. It serves one request at a time.
class Http {
public:
    Http(const Config& config, HttpKeys keys);

    // GET base_url + path_and_query. Retries up to 3 times on connect errors, 408, 429 and
    // 5xx, honouring Retry-After. Returns the last response whatever its status; errors are
    // only transport failures, interruption (Exit::interrupted), an oversize body, or a missing
    // key for options.auth (Exit::config).
    [[nodiscard]] Result<HttpResponse> fetch(std::string_view path_and_query,
                                             const GetOptions& options) const;

    // fetch, with any non-2xx status turned into Exit::api (sanitized message only).
    [[nodiscard]] Result<std::string> get(std::string_view path_and_query,
                                          const GetOptions& options) const;

    // POST base_url + path with a JSON body and the inference key, streaming the response.
    // Never retried, by this class or by libcurl: it always opens its own connection and sends
    // no Expect header. Applies the idle timeout (time since the last byte sent or received)
    // and the total timeout.
    [[nodiscard]] StreamResult post_stream(std::string_view path, const std::string& body,
                                           const StreamHandlers& handlers) const;

    // POST base_url + path with a JSON body, reading a bounded JSON response. Never retried,
    // under the same rules as post_stream: the PKCE exchange code is single-use. Returns the
    // body for 2xx; Exit::api otherwise, with only a sanitized, length-bounded error message.
    [[nodiscard]] Result<std::string> post_json(std::string_view path, const std::string& body,
                                                Auth auth) const;

private:
    [[nodiscard]] Result<std::string_view> key_for(Auth auth) const;

    std::reference_wrapper<const Config> config_;
    HttpKeys keys_;
    std::unique_ptr<void, CurlHandleDeleter> handle_;
};

} // namespace orcli

#endif // ORCLI_HTTP_HPP
