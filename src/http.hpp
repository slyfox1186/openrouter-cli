#pragma once

#include "common.hpp"
#include "config.hpp"

#include <chrono>
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

struct GetOptions {
    bool auth = true;
    // When positive, a 404 is retried with backoff until this much time has passed (used only
    // for the generation lookup, whose record can appear a few seconds after completion).
    std::chrono::seconds retry_404_for{0};
};

struct StreamHandlers {
    std::function<void(std::string_view)> on_generation_id; // from X-Generation-Id
    std::function<void(std::string_view)> on_body_bytes;    // raw SSE bytes, any split
};

struct StreamResult {
    long status = 0;
    bool event_stream = false; // Content-Type was text/event-stream
    std::string error_body;    // body (bounded) when status != 200 or not an event stream
    bool interrupted = false;
    bool idle_timeout = false;
    bool total_timeout = false;
    std::string transport_error; // curl error text, empty when the transfer completed
};

class Http {
public:
    explicit Http(const Config& config);

    // GET base_url + path_and_query. Retries up to 3 times on connect errors, 408, 429 and
    // 5xx, honouring Retry-After. Returns the body for 2xx; Exit::api otherwise.
    Result<std::string> get(std::string_view path_and_query, const GetOptions& options) const;

    // POST base_url + path with a JSON body, streaming the response. Never retried. Applies the
    // idle timeout (time since the last received byte) and the total timeout.
    StreamResult post_stream(std::string_view path, const std::string& body,
                             const StreamHandlers& handlers) const;

private:
    const Config& config_;
};

} // namespace orcli
