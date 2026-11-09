#include "http.hpp"

#include "secrets.hpp"
#include "sse.hpp"

#include <curl/curl.h>
#include <signal.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace orcli {

namespace {

using Clock = std::chrono::steady_clock;

constexpr long connect_timeout_seconds = 30;
constexpr long get_timeout_seconds = 120;
constexpr int max_get_retries = 3;
constexpr std::chrono::seconds first_backoff{1};
constexpr std::chrono::seconds max_retry_after{60};
constexpr std::chrono::milliseconds sleep_slice{200};
constexpr std::size_t max_get_body_bytes = std::size_t{64} << 20U;
constexpr std::size_t max_error_body_bytes = std::size_t{1} << 20U;
// The PKCE exchange answers with a key and a user id; far below this.
constexpr std::size_t max_json_post_body_bytes = std::size_t{64} << 10U;
constexpr std::size_t max_display_chars = 400;
constexpr long http_ok_min = 200;
constexpr long http_ok_max = 299;
constexpr long http_ok = 200;
constexpr long http_not_found = 404;
constexpr long http_request_timeout = 408;
constexpr long http_too_many_requests = 429;
constexpr long http_server_error_min = 500;
constexpr long http_server_error_max = 599;
constexpr int progress_abort = 1;
constexpr int progress_continue = 0;
constexpr const char* user_agent = "openrouter-cli/0.1.0";

// The only mutable global: written from the signal handler, read by transfer callbacks.
std::atomic<bool> interrupt_flag{false};
static_assert(std::atomic<bool>::is_always_lock_free, "signal handlers need a lock-free flag");

void on_terminate_signal(int /*signal*/) {
    interrupt_flag.store(true);
}

struct EasyDeleter {
    void operator()(CURL* handle) const { curl_easy_cleanup(handle); }
};
using EasyHandle = std::unique_ptr<CURL, EasyDeleter>;

struct SlistDeleter {
    void operator()(curl_slist* list) const { curl_slist_free_all(list); }
};
using HeaderList = std::unique_ptr<curl_slist, SlistDeleter>;

template <class T> Result<void> set_option(CURL* handle, CURLoption option, T value) {
    if (curl_easy_setopt(handle, option, value) != CURLE_OK) {
        return fail(Exit::api, "libcurl rejected a transfer option");
    }
    return {};
}

Result<void> append_header(HeaderList& list, const std::string& header) {
    curl_slist* extended = curl_slist_append(list.get(), header.c_str());
    if (extended == nullptr) {
        return fail(Exit::api, "out of memory building request headers");
    }
    static_cast<void>(list.release());
    list.reset(extended);
    return {};
}

std::string lowercase(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string_view trim(std::string_view text) {
    constexpr std::string_view whitespace = " \t\r\n";
    const auto first = text.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(whitespace);
    return text.substr(first, last - first + 1);
}

// Splits "Name: value" and returns the lowercased name with the trimmed value.
std::optional<std::pair<std::string, std::string_view>> split_header(std::string_view line) {
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    return std::pair{lowercase(trim(line.substr(0, colon))), trim(line.substr(colon + 1))};
}

// An empty key sends no Authorization header (public metadata and the PKCE exchange).
Result<HeaderList> base_headers(const Config& config, std::string_view key) {
    HeaderList list;
    if (!key.empty()) {
        std::string header = "Authorization: Bearer ";
        header += key;
        // curl_slist_append copies the text, so this copy of the key is cleared at once.
        auto added = append_header(list, header);
        cleanse(header);
        if (!added) {
            return std::unexpected(added.error());
        }
    }
    if (!config.app_referer.empty()) {
        if (auto added = append_header(list, "HTTP-Referer: " + config.app_referer); !added) {
            return std::unexpected(added.error());
        }
    }
    for (const std::string& header : {"X-OpenRouter-Title: " + config.app_title,
                                      "X-OpenRouter-Categories: " + config.app_categories}) {
        if (auto added = append_header(list, header); !added) {
            return std::unexpected(added.error());
        }
    }
    return list;
}

Result<EasyHandle> new_handle(const std::string& url, long total_timeout_seconds) {
    EasyHandle handle(curl_easy_init());
    if (handle == nullptr) {
        return fail(Exit::api, "libcurl could not create a transfer handle");
    }
    CURL* raw = handle.get();
    const std::array results{
        set_option(raw, CURLOPT_URL, url.c_str()),
        set_option(raw, CURLOPT_NOSIGNAL, 1L),
        set_option(raw, CURLOPT_PROTOCOLS_STR, "http,https"),
        set_option(raw, CURLOPT_FOLLOWLOCATION, 0L),
        set_option(raw, CURLOPT_USERAGENT, user_agent),
        set_option(raw, CURLOPT_CONNECTTIMEOUT, connect_timeout_seconds),
        set_option(raw, CURLOPT_TIMEOUT, total_timeout_seconds),
        set_option(raw, CURLOPT_TCP_KEEPALIVE, 1L),
    };
    for (const auto& result : results) {
        if (!result) {
            return std::unexpected(result.error());
        }
    }
    return handle;
}

// Sleeps in short slices so Ctrl-C still stops a retry wait promptly.
bool interruptible_sleep(std::chrono::milliseconds duration) {
    const auto deadline = Clock::now() + duration;
    while (Clock::now() < deadline) {
        if (interrupt_flag.load()) {
            return false;
        }
        std::this_thread::sleep_for(std::min(
            sleep_slice,
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now())));
    }
    return !interrupt_flag.load();
}

bool retryable_transport_error(CURLcode code) {
    switch (code) {
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
        return true;
    default:
        return false;
    }
}

bool retryable_status(long status) {
    return status == http_request_timeout || status == http_too_many_requests ||
           (status >= http_server_error_min && status <= http_server_error_max);
}

std::string describe_error_body(long status, std::string_view body) {
    StreamOutcome outcome;
    if (parse_error_body(body, outcome) && outcome.error_message.has_value()) {
        const std::string type = outcome.error_type.value_or("error");
        return std::format("HTTP {} ({}): {}", status, type,
                           sanitize_for_display(*outcome.error_message, max_display_chars));
    }
    return std::format("HTTP {}", status);
}

struct GetBuffer {
    std::string body;
    std::size_t limit = max_get_body_bytes;
    bool too_large = false;
};

std::size_t get_write(char* data, std::size_t size, std::size_t count, void* user) {
    auto* buffer = static_cast<GetBuffer*>(user);
    const std::size_t bytes = size * count;
    if (buffer->body.size() + bytes > buffer->limit) {
        buffer->too_large = true;
        return 0;
    }
    try {
        buffer->body.append(std::string_view(data, bytes));
    } catch (const std::exception&) {
        return 0;
    }
    return bytes;
}

// Lets Ctrl-C stop a GET or JSON POST at once instead of after its whole timeout.
int abort_on_interrupt(void* /*user*/, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
                       curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    return interrupt_flag.load() ? progress_abort : progress_continue;
}

// One finished transfer: curl's result, the HTTP status, Retry-After and the body.
struct Attempt {
    CURLcode code = CURLE_OK;
    long status = 0;
    curl_off_t retry_after = 0;
    std::string body;
    bool too_large = false;
};

struct Request {
    const Config* config = nullptr;
    std::string url;
    std::string_view key;                      // empty: no Authorization header
    std::optional<std::string_view> json_body; // set: POST with this body
    std::size_t body_limit = max_get_body_bytes;
};

// Performs one transfer. When body_limit is small (the PKCE exchange) the buffer is reserved
// up front, so a response holding a key is never spread over reallocated, uncleared memory.
Result<Attempt> perform(const Request& request) {
    if (interrupt_flag.load()) {
        return fail(Exit::interrupted, "interrupted");
    }
    auto handle = new_handle(request.url, get_timeout_seconds);
    if (!handle) {
        return std::unexpected(handle.error());
    }
    auto headers = base_headers(*request.config, request.key);
    if (!headers) {
        return std::unexpected(headers.error());
    }
    if (request.json_body.has_value()) {
        for (const char* header : {"Content-Type: application/json", "Accept: application/json"}) {
            if (auto added = append_header(*headers, std::string(header)); !added) {
                return std::unexpected(added.error());
            }
        }
    }
    GetBuffer buffer;
    buffer.limit = request.body_limit;
    if (request.body_limit < max_get_body_bytes) {
        buffer.body.reserve(request.body_limit);
    }
    CURL* raw = handle->get();
    const std::array setup{
        set_option(raw, CURLOPT_HTTPHEADER, headers->get()),
        set_option(raw, CURLOPT_WRITEFUNCTION, &get_write),
        set_option(raw, CURLOPT_WRITEDATA, static_cast<void*>(&buffer)),
        set_option(raw, CURLOPT_XFERINFOFUNCTION, &abort_on_interrupt),
        set_option(raw, CURLOPT_NOPROGRESS, 0L),
    };
    for (const auto& result : setup) {
        if (!result) {
            return std::unexpected(result.error());
        }
    }
    if (request.json_body.has_value()) {
        const std::string_view body = *request.json_body;
        const std::array post_setup{
            set_option(raw, CURLOPT_POST, 1L),
            set_option(raw, CURLOPT_POSTFIELDS, body.data()),
            set_option(raw, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size())),
        };
        for (const auto& result : post_setup) {
            if (!result) {
                return std::unexpected(result.error());
            }
        }
    }
    Attempt attempt;
    attempt.code = curl_easy_perform(raw);
    curl_easy_getinfo(raw, CURLINFO_RESPONSE_CODE, &attempt.status);
    curl_easy_getinfo(raw, CURLINFO_RETRY_AFTER, &attempt.retry_after);
    attempt.too_large = buffer.too_large;
    attempt.body = std::move(buffer.body);
    if (interrupt_flag.load()) {
        return fail(Exit::interrupted, "interrupted");
    }
    return attempt;
}

bool should_retry(const Attempt& attempt, int retries, bool within_404_window) {
    if (attempt.code != CURLE_OK) {
        return retryable_transport_error(attempt.code) && retries < max_get_retries;
    }
    if (attempt.status == http_not_found) {
        return within_404_window;
    }
    return retryable_status(attempt.status) && retries < max_get_retries;
}

std::chrono::milliseconds retry_wait(const Attempt& attempt, std::chrono::milliseconds backoff) {
    if (attempt.retry_after > 0) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::min(std::chrono::seconds{attempt.retry_after}, max_retry_after));
    }
    return backoff;
}

struct StreamState {
    const StreamHandlers* handlers = nullptr;
    StreamResult* result = nullptr;
    Clock::time_point last_byte;
    Clock::duration idle_limit{};
    bool generation_id_seen = false;
    std::string callback_error;
};

std::size_t stream_header(char* data, std::size_t size, std::size_t count, void* user) {
    auto* state = static_cast<StreamState*>(user);
    const std::size_t bytes = size * count;
    state->last_byte = Clock::now();
    try {
        const std::string_view line(data, bytes);
        if (line.starts_with("HTTP/")) {
            state->result->event_stream = false;
            return bytes;
        }
        const auto header = split_header(line);
        if (!header.has_value()) {
            return bytes;
        }
        const auto& [name, value] = *header;
        if (name == "content-type") {
            state->result->event_stream = lowercase(value).starts_with("text/event-stream");
        } else if (name == "x-generation-id" && !value.empty() && !state->generation_id_seen &&
                   state->handlers->on_generation_id) {
            state->generation_id_seen = true;
            state->handlers->on_generation_id(value);
        }
    } catch (const std::exception& error) {
        state->callback_error = error.what();
        return 0;
    }
    return bytes;
}

std::size_t stream_write(char* data, std::size_t size, std::size_t count, void* user) {
    auto* state = static_cast<StreamState*>(user);
    const std::size_t bytes = size * count;
    state->last_byte = Clock::now();
    try {
        const std::string_view chunk(data, bytes);
        if (state->result->event_stream && state->handlers->on_body_bytes) {
            state->handlers->on_body_bytes(chunk);
        } else if (state->result->error_body.size() < max_error_body_bytes) {
            const std::size_t room = max_error_body_bytes - state->result->error_body.size();
            state->result->error_body.append(chunk.substr(0, std::min(room, chunk.size())));
        }
    } catch (const std::exception& error) {
        state->callback_error = error.what();
        return 0;
    }
    return bytes;
}

int stream_progress(void* user, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
                    curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* state = static_cast<StreamState*>(user);
    if (interrupt_flag.load()) {
        state->result->interrupted = true;
        return progress_abort;
    }
    if (Clock::now() - state->last_byte > state->idle_limit) {
        state->result->idle_timeout = true;
        return progress_abort;
    }
    return progress_continue;
}

} // namespace

void install_signal_handlers() {
    struct sigaction ignore_action{};
    ignore_action.sa_handler = SIG_IGN;
    sigemptyset(&ignore_action.sa_mask);
    sigaction(SIGPIPE, &ignore_action, nullptr);

    struct sigaction stop_action{};
    stop_action.sa_handler = on_terminate_signal;
    sigemptyset(&stop_action.sa_mask);
    for (const int signal_number : {SIGINT, SIGTERM, SIGHUP}) {
        sigaction(signal_number, &stop_action, nullptr);
    }
}

bool interrupt_requested() {
    return interrupt_flag.load();
}

CurlGlobal::CurlGlobal() : ok_(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {}

CurlGlobal::~CurlGlobal() {
    if (ok_) {
        curl_global_cleanup();
    }
}

Http::Http(const Config& config, HttpKeys keys) : config_(config), keys_(std::move(keys)) {}

Result<std::string_view> Http::key_for(Auth auth) const {
    switch (auth) {
    case Auth::none:
        return std::string_view{};
    case Auth::inference:
        if (keys_.inference.empty()) {
            return fail(Exit::config, "no API key is stored; run `openrouter-cli auth login` "
                                      "(or `auth set`)");
        }
        return keys_.inference.view();
    case Auth::management:
        if (keys_.management.empty()) {
            return fail(Exit::config, "no Management key is stored; create one at "
                                      "https://openrouter.ai/settings/management-keys and run "
                                      "`openrouter-cli auth set --management`");
        }
        return keys_.management.view();
    }
    return fail(Exit::config, "unknown credential kind");
}

Result<std::string> Http::get(std::string_view path_and_query, const GetOptions& options) const {
    auto response = fetch(path_and_query, options);
    if (!response) {
        return std::unexpected(response.error());
    }
    if (response->status >= http_ok_min && response->status <= http_ok_max) {
        return std::move(response->body);
    }
    return fail(Exit::api, std::format("GET {}: {}", path_and_query,
                                       describe_error_body(response->status, response->body)));
}

Result<HttpResponse> Http::fetch(std::string_view path_and_query, const GetOptions& options) const {
    const auto key = key_for(options.auth);
    if (!key) {
        return std::unexpected(key.error());
    }
    const Request request{.config = &config_,
                          .url = config_.base_url + std::string(path_and_query),
                          .key = *key,
                          .json_body = std::nullopt,
                          .body_limit = max_get_body_bytes};
    const auto started = Clock::now();
    int retries = 0;
    auto backoff = std::chrono::duration_cast<std::chrono::milliseconds>(first_backoff);

    while (true) {
        auto attempt = perform(request);
        if (!attempt) {
            return std::unexpected(attempt.error());
        }
        if (attempt->too_large) {
            return fail(Exit::api, std::format("GET {}: response larger than {} bytes",
                                               path_and_query, max_get_body_bytes));
        }
        const bool success = attempt->code == CURLE_OK && attempt->status >= http_ok_min &&
                             attempt->status <= http_ok_max;
        if (success ||
            !should_retry(*attempt, retries, Clock::now() - started < options.retry_404_for)) {
            if (attempt->code != CURLE_OK) {
                return fail(Exit::api, std::format("GET {}: {}", path_and_query,
                                                   curl_easy_strerror(attempt->code)));
            }
            return HttpResponse{.status = attempt->status, .body = std::move(attempt->body)};
        }
        if (!interruptible_sleep(retry_wait(*attempt, backoff))) {
            return fail(Exit::interrupted, "interrupted");
        }
        backoff *= 2;
        ++retries;
    }
}

StreamResult Http::post_stream(std::string_view path, const std::string& body,
                               const StreamHandlers& handlers) const {
    StreamResult result;
    const std::string url = config_.base_url + std::string(path);

    auto handle = new_handle(url, config_.total_timeout_seconds);
    if (!handle) {
        result.transport_error = handle.error().message;
        return result;
    }
    const auto key = key_for(Auth::inference);
    if (!key) {
        result.transport_error = key.error().message;
        return result;
    }
    auto headers = base_headers(config_, *key);
    if (!headers) {
        result.transport_error = headers.error().message;
        return result;
    }
    for (const char* header : {"Content-Type: application/json", "Accept: text/event-stream",
                               "X-OpenRouter-Metadata: enabled"}) {
        if (auto added = append_header(*headers, std::string(header)); !added) {
            result.transport_error = added.error().message;
            return result;
        }
    }

    StreamState state;
    state.handlers = &handlers;
    state.result = &result;
    state.last_byte = Clock::now();
    state.idle_limit = std::chrono::seconds{config_.idle_timeout_seconds};

    CURL* raw = handle->get();
    const std::array setup{
        set_option(raw, CURLOPT_HTTPHEADER, headers->get()),
        set_option(raw, CURLOPT_POST, 1L),
        set_option(raw, CURLOPT_POSTFIELDS, body.c_str()),
        set_option(raw, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size())),
        set_option(raw, CURLOPT_HEADERFUNCTION, &stream_header),
        set_option(raw, CURLOPT_HEADERDATA, static_cast<void*>(&state)),
        set_option(raw, CURLOPT_WRITEFUNCTION, &stream_write),
        set_option(raw, CURLOPT_WRITEDATA, static_cast<void*>(&state)),
        set_option(raw, CURLOPT_XFERINFOFUNCTION, &stream_progress),
        set_option(raw, CURLOPT_XFERINFODATA, static_cast<void*>(&state)),
        set_option(raw, CURLOPT_NOPROGRESS, 0L),
    };
    for (const auto& outcome : setup) {
        if (!outcome) {
            result.transport_error = outcome.error().message;
            return result;
        }
    }

    const CURLcode code = curl_easy_perform(raw);
    curl_easy_getinfo(raw, CURLINFO_RESPONSE_CODE, &result.status);
    if (result.status != http_ok) {
        result.event_stream = false;
    }
    if (code == CURLE_OPERATION_TIMEDOUT && !result.idle_timeout) {
        result.total_timeout = true;
    }
    if (code != CURLE_OK && !result.interrupted && !result.idle_timeout && !result.total_timeout) {
        result.transport_error =
            state.callback_error.empty() ? curl_easy_strerror(code) : state.callback_error;
    }
    return result;
}

Result<std::string> Http::post_json(std::string_view path, const std::string& body,
                                    Auth auth) const {
    const auto key = key_for(auth);
    if (!key) {
        return std::unexpected(key.error());
    }
    // One attempt only: the PKCE exchange code is single-use, so a retry after an ambiguous
    // failure could never succeed and would hide the first error.
    auto attempt = perform(Request{.config = &config_,
                                   .url = config_.base_url + std::string(path),
                                   .key = *key,
                                   .json_body = body,
                                   .body_limit = max_json_post_body_bytes});
    if (!attempt) {
        return std::unexpected(attempt.error());
    }
    if (attempt->too_large) {
        cleanse(attempt->body);
        return fail(Exit::api, std::format("POST {}: response larger than {} bytes", path,
                                           max_json_post_body_bytes));
    }
    if (attempt->code != CURLE_OK) {
        return fail(Exit::api, std::format("POST {}: {}", path, curl_easy_strerror(attempt->code)));
    }
    if (attempt->status < http_ok_min || attempt->status > http_ok_max) {
        std::string message =
            std::format("POST {}: {}", path, describe_error_body(attempt->status, attempt->body));
        cleanse(attempt->body);
        return fail(Exit::api, std::move(message));
    }
    return std::move(attempt->body);
}

} // namespace orcli
