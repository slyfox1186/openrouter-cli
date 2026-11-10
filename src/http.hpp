#ifndef ORCLI_HTTP_HPP
#define ORCLI_HTTP_HPP

#include "common.hpp"
#include "config.hpp"
#include "credentials.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace orcli {

// SIGPIPE is ignored; SIGINT, SIGTERM and SIGHUP set a flag that aborts transfers.
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
// /key, management for /keys, /credits and /activity. A management key is never sent to an
// inference endpoint because each request names its kind and Http holds both separately.
enum class Auth : std::uint8_t {
    none,
    inference,
    management,
};

// Keys loaded by this process; an empty key means that kind was not loaded, and a request
// needing it fails with Exit::config naming the `auth` command that stores one.
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
    // Never retried. Applies the idle timeout (time since the last received byte) and the total
    // timeout.
    [[nodiscard]] StreamResult post_stream(std::string_view path, const std::string& body,
                                           const StreamHandlers& handlers) const;

    // POST base_url + path with a JSON body, reading a bounded JSON response. Never retried:
    // the PKCE exchange code is single-use. Returns the body for 2xx; Exit::api otherwise, with
    // only a sanitized, length-bounded error message.
    [[nodiscard]] Result<std::string> post_json(std::string_view path, const std::string& body,
                                                Auth auth) const;

private:
    [[nodiscard]] Result<std::string_view> key_for(Auth auth) const;

    std::reference_wrapper<const Config> config_;
    HttpKeys keys_;
};

} // namespace orcli

#endif // ORCLI_HTTP_HPP
