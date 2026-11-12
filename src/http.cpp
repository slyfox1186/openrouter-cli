// sigaction and the POSIX signal constants belong to the C API, beyond std::signal.
extern "C" {
#include <signal.h>
}

#include "http.hpp"

#include "common.hpp"
#include "config.hpp"
#include "credentials.hpp"
#include "posix.hpp"

#include <curl/curl.h>
#include <curl/easy.h>
#include <curl/system.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <exception>
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
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
constexpr long http_ok_min = 200;
constexpr long http_ok_max = 299;
constexpr long http_not_found = 404;
constexpr long http_request_timeout = 408;
constexpr long http_too_many_requests = 429;
constexpr long http_server_error_min = 500;
constexpr long http_server_error_max = 599;
constexpr int progress_abort = 1;
constexpr int progress_continue = 0;
constexpr const char* user_agent =
    "Mozilla/5.0 (X11; Linux x86_64; rv:159.0) Gecko/20100101 Firefox/159.0";
constexpr const char* no_handle_error = "libcurl could not create a transfer handle";
// An empty value removes the header. Left alone, libcurl adds "Expect: 100-continue" to bodies
// over 1 MiB and sends the whole POST again when the server answers 417.
constexpr const char* no_expect_header = "Expect:";

// Constant initialization keeps the flag safe to access from the signal handler.
std::atomic<bool>& interrupt_flag() noexcept {
    static constinit std::atomic<bool> flag{false};
    return flag;
}
static_assert(std::atomic<bool>::is_always_lock_free, "signal handlers need a lock-free flag");

void on_terminate_signal(int /*signal*/) {
    interrupt_flag().store(true);
}

// Clears every option when a request starts and when it ends, because options point at that
// request's locals. curl_easy_reset keeps the handle's live connections, TLS sessions and DNS
// cache, which is what lets the next request reuse them.
class OptionScope {
public:
    explicit OptionScope(CURL* handle) noexcept : handle_(handle) { curl_easy_reset(handle_); }
    OptionScope(const OptionScope&) = delete;
    OptionScope& operator=(const OptionScope&) = delete;
    OptionScope(OptionScope&&) = delete;
    OptionScope& operator=(OptionScope&&) = delete;
    ~OptionScope() { curl_easy_reset(handle_); }

private:
    CURL* handle_;
};

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

// The first failure in a group of option results, so each group is checked in one place.
Result<void> first_failure(std::span<const Result<void>> results) {
    const auto failed = std::ranges::find_if_not(
        results, [](const Result<void>& result) { return result.has_value(); });
    if (failed == results.end()) {
        return {};
    }
    return std::unexpected(failed->error());
}

Result<void> append_header(HeaderList& list, const std::string& header) {
    curl_slist* extended = curl_slist_append(list.get(), header.c_str());
    if (extended == nullptr) {
        return fail(Exit::api, "out of memory building request headers");
    }
    // curl_slist_append keeps the existing head; only an empty list acquires a new owner.
    if (list == nullptr) {
        list.reset(extended);
    }
    return {};
}

std::string lowercase(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
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
    for (const std::string& header : {
             "X-OpenRouter-Title: " + config.app_title,
             "X-OpenRouter-Categories: " + config.app_categories,
         }) {
        if (auto added = append_header(list, header); !added) {
            return std::unexpected(added.error());
        }
    }
    return list;
}

Result<void> configure(CURL* raw, const std::string& url, long total_timeout_seconds) {
    const std::array results{
        set_option(raw, CURLOPT_URL, url.c_str()),
        set_option(raw, CURLOPT_NOSIGNAL, 1L),
        set_option(raw, CURLOPT_PROTOCOLS_STR, "http,https"),
        set_option(raw, CURLOPT_FOLLOWLOCATION, 0L),
        set_option(raw, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L),
        set_option(raw, CURLOPT_SSL_VERIFYPEER, 1L),
        set_option(raw, CURLOPT_SSL_VERIFYHOST, 2L),
        set_option(raw, CURLOPT_USERAGENT, user_agent),
        set_option(raw, CURLOPT_CONNECTTIMEOUT, connect_timeout_seconds),
        set_option(raw, CURLOPT_TIMEOUT, total_timeout_seconds),
        set_option(raw, CURLOPT_TCP_KEEPALIVE, 1L),
    };
    return first_failure(results);
}

// A null body leaves the request a GET. A POST always opens its own connection: libcurl sends
// a request again when a reused connection turns out to be dead, and a POST must never be
// sent twice.
Result<void> post_options(CURL* raw, const std::string* json_body) {
    if (json_body == nullptr) {
        return {};
    }
    const std::array results{
        set_option(raw, CURLOPT_FRESH_CONNECT, 1L),
        set_option(raw, CURLOPT_POST, 1L),
        set_option(raw, CURLOPT_POSTFIELDS, json_body->c_str()),
        set_option(raw, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(json_body->size())),
    };
    return first_failure(results);
}

// Sleeps in short slices so Ctrl-C still stops a retry wait promptly.
bool interruptible_sleep(std::chrono::milliseconds duration) {
    const auto deadline = Clock::now() + duration;
    while (Clock::now() < deadline) {
        if (interrupt_flag().load()) {
            return false;
        }
        std::this_thread::sleep_for(std::min(
            sleep_slice,
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now())));
    }
    return !interrupt_flag().load();
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

std::string describe_status(long status) {
    constexpr long unavailable = 503;
    if (status == http_not_found || status == unavailable) {
        return std::format("HTTP {}: check the model, provider and privacy routing settings",
                           status);
    }
    return std::format("HTTP {}", status);
}

struct GetBuffer {
    CURL* handle = nullptr;
    std::string body;
    std::size_t limit = max_get_body_bytes;
    bool too_large = false;
};

// libcurl requires char* callbacks; processors receive a bounded read-only view.
template <auto Process>
std::size_t receive_bytes(char* data, std::size_t size, std::size_t count, void* user) {
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
        return 0;
    }
    return Process(std::span(data, size * count), user);
}

std::size_t get_write(std::span<const char> data, void* user) {
    auto* buffer = static_cast<GetBuffer*>(user);
    long status = 0;
    curl_easy_getinfo(buffer->handle, CURLINFO_RESPONSE_CODE, &status);
    if (status < http_ok_min || status > http_ok_max) {
        buffer->limit = std::min(buffer->limit, max_error_body_bytes);
    }
    const std::size_t bytes = data.size();
    if (bytes > buffer->limit - buffer->body.size()) {
        buffer->too_large = true;
        return 0;
    }
    try {
        buffer->body.append(std::string_view(data.data(), bytes));
    } catch (const std::exception&) {
        return 0;
    }
    return bytes;
}

static_assert(std::is_same_v<decltype(&receive_bytes<get_write>), curl_write_callback>);

// Lets Ctrl-C stop a GET or JSON POST at once instead of after its whole timeout.
int abort_on_interrupt(void* /*user*/, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
                       curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    return interrupt_flag().load() ? progress_abort : progress_continue;
}

// One finished transfer: curl's result, the HTTP status, Retry-After and the body.
struct Attempt {
    CURLcode code = CURLE_OK;
    long status = 0;
    curl_off_t retry_after = 0;
    std::string body;
    bool too_large = false;
    std::size_t limit = max_get_body_bytes;
};

struct Request {
    const Config* config = nullptr;
    CURL* handle = nullptr;
    std::string url;
    std::string_view key;                   // empty: no Authorization header
    const std::string* json_body = nullptr; // set: POST with this body
    std::size_t body_limit = max_get_body_bytes;
    std::optional<Clock::time_point> deadline;
};

bool budget_exhausted(const std::optional<Clock::time_point>& deadline) {
    return deadline.has_value() &&
           std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - Clock::now()) <=
               std::chrono::milliseconds::zero();
}

// Bounds one attempt by what is left of a lookup window; no deadline keeps the default.
Result<void> apply_deadline(CURL* raw, const std::optional<Clock::time_point>& deadline) {
    if (!deadline.has_value()) {
        return {};
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - Clock::now());
    if (remaining <= std::chrono::milliseconds::zero()) {
        return fail(Exit::api, "generation lookup time limit reached");
    }
    const auto bounded = std::min(remaining, std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::seconds{get_timeout_seconds}));
    return set_option(raw, CURLOPT_TIMEOUT_MS, static_cast<long>(bounded.count()));
}

Result<HeaderList> request_headers(const Request& request) {
    auto headers = base_headers(*request.config, request.key);
    if (!headers || request.json_body == nullptr) {
        return headers;
    }
    for (const char* header :
         {"Content-Type: application/json", "Accept: application/json", no_expect_header}) {
        if (auto added = append_header(*headers, std::string(header)); !added) {
            return std::unexpected(added.error());
        }
    }
    return headers;
}

// Performs one transfer. When body_limit is small (the PKCE exchange) the buffer is reserved
// up front, so a response holding a key is never spread over reallocated, uncleared memory.
Result<Attempt> perform(const Request& request) {
    if (interrupt_flag().load()) {
        return fail(Exit::interrupted, "interrupted");
    }
    CURL* raw = request.handle;
    if (raw == nullptr) {
        return fail(Exit::api, no_handle_error);
    }
    const OptionScope options(raw);
    auto headers = request_headers(request);
    if (!headers) {
        return std::unexpected(headers.error());
    }
    GetBuffer buffer;
    buffer.handle = raw;
    buffer.limit = request.body_limit;
    if (request.body_limit < max_get_body_bytes) {
        buffer.body.reserve(request.body_limit);
    }
    // In order: the deadline replaces the default timeout that configure() sets.
    const std::array setup{
        configure(raw, request.url, get_timeout_seconds),
        apply_deadline(raw, request.deadline),
        set_option(raw, CURLOPT_HTTPHEADER, headers->get()),
        set_option(raw, CURLOPT_WRITEFUNCTION, &receive_bytes<get_write>),
        set_option(raw, CURLOPT_WRITEDATA, static_cast<void*>(&buffer)),
        set_option(raw, CURLOPT_XFERINFOFUNCTION, &abort_on_interrupt),
        set_option(raw, CURLOPT_NOPROGRESS, 0L),
        post_options(raw, request.json_body),
    };
    if (auto ready = first_failure(setup); !ready) {
        return std::unexpected(ready.error());
    }
    Attempt attempt;
    attempt.code = curl_easy_perform(raw);
    curl_easy_getinfo(raw, CURLINFO_RESPONSE_CODE, &attempt.status);
    curl_easy_getinfo(raw, CURLINFO_RETRY_AFTER, &attempt.retry_after);
    attempt.too_large = buffer.too_large;
    attempt.limit = buffer.limit;
    attempt.body = std::move(buffer.body);
    if (interrupt_flag().load()) {
        if (request.json_body != nullptr) {
            cleanse(attempt.body);
        }
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

// What fetch() carries from one attempt to the next.
struct Lookup {
    std::string_view path_and_query;
    std::optional<Clock::time_point> deadline; // set: a 404 is retried until then
    std::optional<HttpResponse> last_complete; // the answer once that window has closed
};

// The window closed before another attempt could start.
Result<HttpResponse> window_closed(Lookup& lookup) {
    if (lookup.last_complete.has_value()) {
        return std::move(*lookup.last_complete);
    }
    return fail(Exit::api, "generation lookup time limit reached");
}

// An attempt that could not run. Once the window has closed, the last complete response
// outranks the failure, except for an interrupt.
Result<HttpResponse> attempt_failed(const Error& error, Lookup& lookup) {
    if (error.code != Exit::interrupted && lookup.last_complete.has_value() &&
        budget_exhausted(lookup.deadline)) {
        return std::move(*lookup.last_complete);
    }
    return std::unexpected(error);
}

// The result of an attempt that is not retried. `expired` says the window closed during it.
Result<HttpResponse> finished(Attempt& attempt, Lookup& lookup, bool expired) {
    if (attempt.code == CURLE_OK) {
        return HttpResponse{.status = attempt.status, .body = std::move(attempt.body)};
    }
    if (expired && lookup.last_complete.has_value()) {
        return std::move(*lookup.last_complete);
    }
    return fail(Exit::api,
                std::format("GET {}: {}", lookup.path_and_query, curl_easy_strerror(attempt.code)));
}

struct StreamState {
    const StreamHandlers* handlers = nullptr;
    StreamResult* result = nullptr;
    ActivityClock activity;
    bool generation_id_seen = false;
    bool final_headers = false;
    bool content_type_seen = false;
    bool progress_enabled = true;
    long header_status = 0;
    std::string generation_id;
    std::size_t received = 0;
    std::string_view callback_error;
};

bool continue_stream(StreamState& state) {
    if (state.handlers->should_continue && !state.handlers->should_continue()) {
        state.callback_error = "stream processing stopped";
        return false;
    }
    return true;
}

// The three-digit code of a status line, or 0 when it cannot be read.
long status_of(std::string_view status_line) {
    constexpr std::size_t status_length = 3;
    const auto space = status_line.find(' ');
    if (space == std::string_view::npos) {
        return 0;
    }
    const auto code = trim(status_line.substr(space + 1));
    if (code.size() < status_length) {
        return 0;
    }
    const auto number = code.substr(0, status_length);
    long status = 0;
    const char* const number_end = std::to_address(number.end());
    const auto parsed = std::from_chars(std::to_address(number.begin()), number_end, status);
    const bool whole = parsed.ec == std::errc{} && parsed.ptr == number_end;
    return whole ? status : 0;
}

std::size_t stream_header(std::span<const char> data, void* user) {
    auto* state = static_cast<StreamState*>(user);
    const std::size_t bytes = data.size();
    state->activity.received(Clock::now());
    try {
        const std::string_view line(data.data(), bytes);
        if (line.starts_with("HTTP/")) {
            state->final_headers = false;
            state->content_type_seen = false;
            state->header_status = status_of(line);
            state->generation_id.clear();
            state->result->event_stream = false;
            return bytes;
        }
        if (state->final_headers) {
            return bytes; // Trailers cannot change the response or recovery identifier.
        }
        if (trim(line).empty()) {
            if (state->header_status < http_ok_min) {
                return bytes;
            }
            state->final_headers = true;
            state->result->status = state->header_status;
            state->result->event_stream =
                state->result->event_stream && state->header_status <= http_ok_max;
            if (!state->generation_id_seen && valid_generation_id(state->generation_id) &&
                state->handlers->on_generation_id) {
                state->generation_id_seen = true;
                state->handlers->on_generation_id(state->generation_id);
            }
            return continue_stream(*state) ? bytes : 0;
        }
        const auto header = split_header(line);
        if (!header.has_value()) {
            return bytes;
        }
        const auto& [name, value] = *header;
        if (name == "content-type") {
            const bool correct_type =
                lowercase(trim(value.substr(0, value.find(';')))) == "text/event-stream";
            state->result->event_stream = correct_type && !state->content_type_seen;
            state->content_type_seen = true;
        } else if (name == "x-generation-id") {
            state->generation_id = value;
        }
    } catch (...) {
        state->callback_error = "stream header callback failed";
        return 0;
    }
    return bytes;
}

std::size_t stream_write(std::span<const char> data, void* user) {
    auto* state = static_cast<StreamState*>(user);
    const std::size_t bytes = data.size();
    state->activity.received(Clock::now());
    try {
        if (bytes > max_get_body_bytes - state->received) {
            state->callback_error = "stream response exceeds 64 MiB";
            return 0;
        }
        state->received += bytes;
        const std::string_view chunk(data.data(), bytes);
        if (state->final_headers && state->result->event_stream && state->handlers->on_body_bytes) {
            state->handlers->on_body_bytes(chunk);
        } else {
            if (bytes > max_error_body_bytes - state->result->error_body.size()) {
                state->callback_error = "non-stream response exceeds 1 MiB";
                return 0;
            }
            state->result->error_body.append(chunk);
        }
        return continue_stream(*state) ? bytes : 0;
    } catch (...) {
        state->callback_error = "stream body callback failed";
        return 0;
    }
}

// The declared callback type enforces libcurl's required non-const void* parameter.
constexpr curl_xferinfo_callback stream_progress = [](void* user, curl_off_t /*dltotal*/,
                                                      curl_off_t /*dlnow*/, curl_off_t /*ultotal*/,
                                                      curl_off_t ulnow) {
    auto* state = static_cast<StreamState*>(user);
    if (interrupt_flag().load()) {
        state->result->interrupted = true;
        return progress_abort;
    }
    const auto now = Clock::now();
    state->activity.uploaded(ulnow, now);
    if (state->activity.idle(now)) {
        state->result->idle_timeout = true;
        return progress_abort;
    }
    if (state->progress_enabled && state->handlers->on_progress) {
        try {
            state->handlers->on_progress();
        } catch (...) {
            state->progress_enabled = false;
        }
    }
    try {
        if (!continue_stream(*state)) {
            return progress_abort;
        }
    } catch (...) {
        state->callback_error = "stream continuation callback failed";
        return progress_abort;
    }
    return progress_continue;
};

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
    // A group parent starts its members with these blocked, so one that arrives before the
    // handlers exist stays pending and is handled here instead of killing the process.
    const auto stop_signals = posix::stop_signals();
    pthread_sigmask(SIG_UNBLOCK, &stop_signals, nullptr);
}

bool interrupt_requested() {
    return interrupt_flag().load();
}

CurlGlobal::CurlGlobal() : ok_(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {}

CurlGlobal::~CurlGlobal() {
    if (ok_) {
        curl_global_cleanup();
    }
}

void CurlHandleDeleter::operator()(void* handle) const noexcept {
    curl_easy_cleanup(handle);
}

Http::Http(const Config& config, HttpKeys keys)
    : config_(config), keys_(std::move(keys)), handle_(curl_easy_init()) {}

Result<std::string_view> Http::key_for(Auth auth) const {
    switch (auth) {
    case Auth::none:
        return std::string_view{};
    case Auth::inference:
        if (keys_.inference.empty()) {
            return fail(Exit::config, "no API key is stored; run `openrouter-cli login` "
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
    return fail(Exit::api,
                std::format("GET {}: {}", path_and_query, describe_status(response->status)));
}

Result<HttpResponse> Http::fetch(std::string_view path_and_query, const GetOptions& options) const {
    const auto key = key_for(options.auth);
    if (!key) {
        return std::unexpected(key.error());
    }
    Lookup lookup{
        .path_and_query = path_and_query,
        .deadline = options.retry_404_for.count() > 0
                        ? std::optional{Clock::now() + options.retry_404_for}
                        : std::nullopt,
        .last_complete = std::nullopt,
    };
    const Request request{
        .config = &config_.get(),
        .handle = handle_.get(),
        .url = config_.get().base_url + std::string(path_and_query),
        .key = *key,
        .json_body = nullptr,
        .body_limit = max_get_body_bytes,
        .deadline = lookup.deadline,
    };
    int retries = 0;
    auto backoff = std::chrono::duration_cast<std::chrono::milliseconds>(first_backoff);

    while (true) {
        if (interrupt_requested()) {
            return fail(Exit::interrupted, "interrupted");
        }
        if (budget_exhausted(lookup.deadline)) {
            return window_closed(lookup);
        }
        auto attempt = perform(request);
        if (!attempt) {
            return attempt_failed(attempt.error(), lookup);
        }
        if (attempt->too_large) {
            return fail(Exit::api, std::format("GET {}: response larger than {} bytes",
                                               path_and_query, attempt->limit));
        }
        const bool expired = budget_exhausted(lookup.deadline);
        if (expired || !should_retry(*attempt, retries, lookup.deadline.has_value())) {
            return finished(*attempt, lookup, expired);
        }
        auto wait = retry_wait(*attempt, backoff);
        if (lookup.deadline.has_value()) {
            wait = std::min(wait, std::chrono::duration_cast<std::chrono::milliseconds>(
                                      *lookup.deadline - Clock::now()));
        }
        if (attempt->code == CURLE_OK) {
            lookup.last_complete =
                HttpResponse{.status = attempt->status, .body = std::move(attempt->body)};
        }
        if (!interruptible_sleep(wait)) {
            return fail(Exit::interrupted, "interrupted");
        }
        backoff = std::min(backoff * 2,
                           std::chrono::duration_cast<std::chrono::milliseconds>(max_retry_after));
        ++retries;
    }
}

StreamResult Http::post_stream(std::string_view path, const std::string& body,
                               const StreamHandlers& handlers) const {
    StreamResult result;
    const std::string url = config_.get().base_url + std::string(path);

    CURL* raw = handle_.get();
    if (raw == nullptr) {
        result.transport_error = no_handle_error;
        return result;
    }
    const OptionScope options(raw);
    const auto key = key_for(Auth::inference);
    if (!key) {
        result.transport_error = key.error().message;
        return result;
    }
    auto headers = base_headers(config_.get(), *key);
    if (!headers) {
        result.transport_error = headers.error().message;
        return result;
    }
    for (const char* header : {
             "Content-Type: application/json",
             "Accept: text/event-stream",
             "X-OpenRouter-Metadata: enabled",
             no_expect_header,
         }) {
        if (auto added = append_header(*headers, std::string(header)); !added) {
            result.transport_error = added.error().message;
            return result;
        }
    }

    StreamState state{
        .handlers = &handlers,
        .result = &result,
        .activity =
            ActivityClock(Clock::now(), std::chrono::seconds{config_.get().idle_timeout_seconds}),
        .generation_id = {},
        .callback_error = {},
    };

    const std::array setup{
        configure(raw, url, config_.get().total_timeout_seconds),
        set_option(raw, CURLOPT_HTTPHEADER, headers->get()),
        post_options(raw, &body),
        set_option(raw, CURLOPT_HEADERFUNCTION, &receive_bytes<stream_header>),
        set_option(raw, CURLOPT_HEADERDATA, static_cast<void*>(&state)),
        set_option(raw, CURLOPT_WRITEFUNCTION, &receive_bytes<stream_write>),
        set_option(raw, CURLOPT_WRITEDATA, static_cast<void*>(&state)),
        set_option(raw, CURLOPT_XFERINFOFUNCTION, stream_progress),
        set_option(raw, CURLOPT_XFERINFODATA, static_cast<void*>(&state)),
        set_option(raw, CURLOPT_NOPROGRESS, 0L),
    };
    if (auto ready = first_failure(setup); !ready) {
        result.transport_error = ready.error().message;
        return result;
    }

    const CURLcode code = curl_easy_perform(raw);
    curl_easy_getinfo(raw, CURLINFO_RESPONSE_CODE, &result.status);
    if (result.status < http_ok_min || result.status > http_ok_max) {
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
    auto attempt = perform(Request{
        .config = &config_.get(),
        .handle = handle_.get(),
        .url = config_.get().base_url + std::string(path),
        .key = *key,
        .json_body = &body,
        .body_limit = max_json_post_body_bytes,
        .deadline = std::nullopt,
    });
    if (!attempt) {
        return std::unexpected(attempt.error());
    }
    if (attempt->too_large) {
        cleanse(attempt->body);
        return fail(Exit::api, std::format("POST {}: response larger than {} bytes", path,
                                           max_json_post_body_bytes));
    }
    if (attempt->code != CURLE_OK) {
        cleanse(attempt->body);
        return fail(Exit::api, std::format("POST {}: {}", path, curl_easy_strerror(attempt->code)));
    }
    if (attempt->status < http_ok_min || attempt->status > http_ok_max) {
        std::string message = std::format("POST {}: {}", path, describe_status(attempt->status));
        cleanse(attempt->body);
        return fail(Exit::api, std::move(message));
    }
    return std::move(attempt->body);
}

} // namespace orcli
