#include "common.hpp"
#include "config.hpp"
#include "context.hpp"
#include "costlog.hpp"
#include "fsutil.hpp"
#include "http.hpp"
#include "models.hpp"
#include "request.hpp"
#include "secrets.hpp"
#include "session.hpp"
#include "sse.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <iterator>
#include <memory>
#include <numeric>
#include <optional>
#include <print>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

namespace orcli {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr std::string_view version_text = "openrouter-cli 0.1.0";
constexpr std::chrono::seconds heartbeat_interval{30};
constexpr std::chrono::milliseconds heartbeat_slice{500};
constexpr std::chrono::seconds generation_lookup_window{30};
constexpr std::size_t max_display_chars = 400;
constexpr std::size_t stdin_chunk_bytes = 65536;
constexpr int internal_error_exit = 1;
constexpr long http_unauthorized = 401;
constexpr int hours_per_day = 24;
constexpr int week_days = 7;
constexpr int month_days = 30;

int code_of(Exit exit) {
    return static_cast<int>(exit);
}

int report(const Error& error) {
    std::println(stderr, "openrouter-cli: {}", error.message);
    return code_of(error.code);
}

struct AskArgs {
    std::string model;
    std::vector<std::string> files;
    std::string role = "review";
    bool role_given = false;
    std::string system;
    std::string effort;
    long long max_output_tokens = 0;
    std::string privacy;
    std::vector<std::string> providers;
    std::string session;
    std::string query;
};

struct FileCloser {
    void operator()(std::FILE* file) const { std::fclose(file); }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

Result<std::string> read_stdin(std::size_t max_bytes) {
    std::string text;
    std::string chunk(stdin_chunk_bytes, '\0');
    while (true) {
        const std::size_t got = std::fread(chunk.data(), 1, chunk.size(), stdin);
        text.append(chunk, 0, got);
        if (text.size() > max_bytes) {
            return fail(Exit::refused, std::format("stdin is larger than {} bytes "
                                                   "(OPENROUTER_CLI_MAX_INPUT_BYTES)",
                                                   max_bytes));
        }
        if (got < chunk.size()) {
            break;
        }
    }
    if (std::ferror(stdin) != 0) {
        return fail(Exit::usage, "could not read the query from stdin");
    }
    return text;
}

std::string iso_days_ago(int days) {
    const auto then = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()) -
                      std::chrono::hours{hours_per_day * days};
    return std::format("{:%FT%TZ}", then);
}

FetchText catalog_fetcher(const Http& http) {
    return [&http]() { return http.get("/models", GetOptions{.auth = false}); };
}

Result<Catalog> catalog_for(const Config& config, const Http& http, bool refresh) {
    return load_catalog(config.state_dir, std::chrono::hours{config.catalog_ttl_hours},
                        catalog_fetcher(http), refresh);
}

// ----- models subcommands -----------------------------------------------------------------

int cmd_models_find(const Config& config, const Http& http, const std::string& name, bool refresh) {
    auto catalog = catalog_for(config, http, refresh);
    if (!catalog) {
        return report(catalog.error());
    }
    auto matches = search(name, catalog->models);
    if (matches.empty() && !refresh) {
        catalog = catalog_for(config, http, true);
        if (!catalog) {
            return report(catalog.error());
        }
        matches = search(name, catalog->models);
    }
    if (matches.empty()) {
        return report(
            Error{.code = Exit::model, .message = std::format("no model matches \"{}\"", name)});
    }
    std::print("{}", format_candidates(matches));
    return code_of(Exit::ok);
}

// Resolves a name for display or saving; ambiguity and misses are reported with candidates.
Result<ModelInfo> resolve_or_report(const Config& config, const Http& http,
                                    const std::string& name) {
    auto catalog = catalog_for(config, http, false);
    if (!catalog) {
        return std::unexpected(catalog.error());
    }
    auto aliases = load_aliases(config.state_dir);
    if (!aliases) {
        return std::unexpected(aliases.error());
    }
    auto resolution = resolve(name, catalog->models, *aliases);
    if (resolution.kind == Resolution::Kind::none) {
        catalog = catalog_for(config, http, true);
        if (!catalog) {
            return std::unexpected(catalog.error());
        }
        resolution = resolve(name, catalog->models, *aliases);
    }
    if (resolution.kind == Resolution::Kind::ambiguous) {
        std::print(stderr, "{}", format_candidates(resolution.candidates));
        return fail(Exit::model, std::format("\"{}\" matches {} models; pass one exact id from "
                                             "the list above",
                                             name, resolution.candidates.size()));
    }
    if (resolution.kind == Resolution::Kind::none || !resolution.model.has_value()) {
        return fail(Exit::model, std::format("no model matches \"{}\"; try `openrouter-cli "
                                             "models find <words>`",
                                             name));
    }
    if (resolution.new_alias.has_value()) {
        if (auto saved = save_alias(config.state_dir, *resolution.new_alias, resolution.model->id);
            saved) {
            std::println(stderr, "openrouter-cli: saved alias {} -> {}", *resolution.new_alias,
                         resolution.model->id);
        } else {
            std::println(stderr, "openrouter-cli: could not save alias: {}", saved.error().message);
        }
    }
    return std::move(*resolution.model);
}

Result<std::vector<Endpoint>> endpoints_for(const Http& http, const std::string& model_id) {
    const auto body = http.get("/models/" + model_id + "/endpoints", GetOptions{.auth = false});
    if (!body) {
        return std::unexpected(body.error());
    }
    return parse_endpoints(*body);
}

// A mistyped provider would otherwise surface as a routing error from a billed request.
Result<void> check_providers(const Http& http, const ModelInfo& model,
                             const std::vector<std::string>& providers) {
    if (providers.empty()) {
        return {};
    }
    const auto endpoints = endpoints_for(http, model.id);
    if (!endpoints) {
        return std::unexpected(endpoints.error());
    }
    for (const auto& slug : providers) {
        const bool served = std::ranges::any_of(*endpoints, [&slug](const Endpoint& endpoint) {
            return provider_matches(slug, endpoint.tag);
        });
        if (!served) {
            std::print(stderr, "{}", format_endpoints(*endpoints));
            return fail(Exit::model, std::format("no endpoint of {} matches provider \"{}\"; "
                                                 "use a tag from the list above",
                                                 model.id, slug));
        }
    }
    return {};
}

int cmd_models_show(const Config& config, const Http& http, const std::string& name) {
    const auto model = resolve_or_report(config, http, name);
    if (!model) {
        return report(model.error());
    }
    const auto catalog = catalog_for(config, http, false);
    if (!catalog) {
        return report(catalog.error());
    }
    std::print("{}", format_model(*model, catalog->fetched_at));
    const auto endpoints = endpoints_for(http, model->id);
    if (!endpoints) {
        return report(endpoints.error());
    }
    std::print("\n{}", format_endpoints(*endpoints));
    return code_of(Exit::ok);
}

int cmd_models_save(const Config& config, const Http& http, const std::string& alias,
                    const std::string& model_id) {
    const auto catalog = catalog_for(config, http, false);
    if (!catalog) {
        return report(catalog.error());
    }
    const bool known = std::ranges::any_of(
        catalog->models, [&model_id](const ModelInfo& model) { return model.id == model_id; });
    if (!known) {
        return report(Error{.code = Exit::model,
                            .message = std::format("\"{}\" is not a model id in the catalog; "
                                                   "use `openrouter-cli models find`",
                                                   model_id)});
    }
    const std::string name = normalize_alias(alias);
    if (name.empty()) {
        return report(Error{.code = Exit::usage, .message = "the alias has no letters or digits"});
    }
    if (auto saved = save_alias(config.state_dir, name, model_id); !saved) {
        return report(saved.error());
    }
    std::println("saved alias {} -> {}", name, model_id);
    return code_of(Exit::ok);
}

int cmd_models_list(const Config& config) {
    const auto aliases = load_aliases(config.state_dir);
    if (!aliases) {
        return report(aliases.error());
    }
    if (aliases->empty()) {
        std::println("no saved aliases yet");
        return code_of(Exit::ok);
    }
    for (const auto& [alias, entry] : *aliases) {
        std::println("{} -> {} (saved {})", alias, entry.model_id, entry.saved);
    }
    return code_of(Exit::ok);
}

// ----- spend and balance ------------------------------------------------------------------

int cmd_spend(const Config& config, const std::string& since) {
    std::string since_iso;
    if (since == "today") {
        since_iso = utc_now_iso().substr(0, std::string_view("YYYY-MM-DD").size());
    } else if (since == "7d") {
        since_iso = iso_days_ago(week_days);
    } else if (since == "30d") {
        since_iso = iso_days_ago(month_days);
    }
    std::error_code error;
    if (!fs::exists(config.cost_log, error)) {
        std::println("no calls logged yet ({})", config.cost_log.string());
        return code_of(Exit::ok);
    }
    if (auto checked = check_private_file(config.cost_log); !checked) {
        return report(checked.error());
    }
    FilePtr file(std::fopen(config.cost_log.c_str(), "rb"));
    if (file == nullptr) {
        return report(
            Error{.code = Exit::config, .message = "cannot read " + config.cost_log.string()});
    }
    std::string contents;
    std::string chunk(stdin_chunk_bytes, '\0');
    std::size_t got = 0;
    while ((got = std::fread(chunk.data(), 1, chunk.size(), file.get())) > 0) {
        contents.append(chunk, 0, got);
    }
    std::print("{}", format_spend(fold_log(contents, since_iso)));
    return code_of(Exit::ok);
}

std::string dollars(const nlohmann::json& value) {
    return value.is_number() ? std::format("${:.4f}", value.get<double>()) : "n/a";
}

int cmd_balance(const Http& http) {
    const auto body = http.get("/key", GetOptions{});
    if (!body) {
        return report(body.error());
    }
    const auto json = nlohmann::json::parse(*body, nullptr, false);
    if (json.is_discarded() || !json.contains("data") || !json["data"].is_object()) {
        return report(Error{.code = Exit::api, .message = "unexpected response from /key"});
    }
    const auto& data = json["data"];
    const auto field = [&data](const char* name) {
        return data.contains(name) ? data[name] : nlohmann::json();
    };
    std::println("usage today      {}", dollars(field("usage_daily")));
    std::println("usage this week  {}", dollars(field("usage_weekly")));
    std::println("usage this month {}", dollars(field("usage_monthly")));
    std::println("usage total      {}", dollars(field("usage")));
    std::println("key limit        {}", dollars(field("limit")));
    std::println("limit remaining  {}", dollars(field("limit_remaining")));
    const auto reset = field("limit_reset");
    std::println("limit resets     {}", reset.is_string() ? reset.get<std::string>() : "n/a");
    return code_of(Exit::ok);
}

// ----- ask --------------------------------------------------------------------------------

struct Prepared {
    ModelInfo model;
    std::optional<Session> session;
    std::vector<Message> messages;
    Message user_turn;
    BuiltRequest request;
};

Result<std::string> system_text(const AskArgs& args, const Config& config) {
    const auto role = args.role == "plain" ? Role::plain : Role::review;
    std::string text = system_prompt(role);
    if (args.system.empty()) {
        return text;
    }
    std::string extra = args.system;
    if (extra.starts_with('@')) {
        auto snapshot = read_snapshot(fs::path(extra.substr(1)), config.max_file_bytes);
        if (!snapshot) {
            return std::unexpected(snapshot.error());
        }
        extra = std::move(snapshot->content);
    }
    return text.empty() ? extra : text + "\n\n" + extra;
}

Result<std::vector<FileSnapshot>> read_files(const AskArgs& args, const Config& config) {
    std::vector<FileSnapshot> files;
    for (const auto& path : args.files) {
        auto snapshot = read_snapshot(fs::path(path), config.max_file_bytes);
        if (!snapshot) {
            return std::unexpected(snapshot.error());
        }
        files.push_back(std::move(*snapshot));
    }
    return files;
}

Result<void> refuse_secrets(const std::vector<Message>& messages,
                            const std::vector<FileSnapshot>& files, std::string_view query,
                            std::string_view system) {
    std::vector<SecretFinding> findings = scan_text(system, "system text");
    std::ranges::move(scan_text(query, "query"), std::back_inserter(findings));
    for (const auto& message : messages) {
        if (message.role != "system") {
            std::ranges::move(scan_text(message.content, "session history"),
                              std::back_inserter(findings));
        }
    }
    for (const auto& file : files) {
        std::ranges::move(scan_text(file.content, file.display_path), std::back_inserter(findings));
    }
    if (findings.empty()) {
        return {};
    }
    for (const auto& finding : findings) {
        std::println(stderr, "openrouter-cli: possible secret in {}:{} ({})", finding.source,
                     finding.line, finding.pattern);
    }
    return fail(Exit::refused, "nothing was sent; remove or replace the values above and retry");
}

Result<ModelInfo> session_model(const Config& config, const Http& http, const AskArgs& args,
                                const SessionData& data) {
    auto catalog = catalog_for(config, http, false);
    if (!catalog) {
        return std::unexpected(catalog.error());
    }
    const auto found = std::ranges::find(catalog->models, data.model_id, &ModelInfo::id);
    if (found == catalog->models.end()) {
        return fail(Exit::model, std::format("session model {} is no longer in the catalog; "
                                             "start a new session",
                                             data.model_id));
    }
    if (!args.model.empty()) {
        auto requested = resolve_or_report(config, http, args.model);
        if (!requested) {
            return std::unexpected(requested.error());
        }
        if (requested->id != data.model_id) {
            return fail(Exit::usage, std::format("session {} is pinned to {}; drop -m or use a "
                                                 "new session name",
                                                 args.session, data.model_id));
        }
    }
    return *found;
}

Result<Prepared> prepare(const AskArgs& args, const Config& config, const Http& http) {
    Prepared prepared;
    std::string system;
    std::vector<Message> history;

    if (!args.session.empty()) {
        auto session = Session::open(config.state_dir, args.session);
        if (!session) {
            return std::unexpected(session.error());
        }
        prepared.session.emplace(std::move(*session));
    }

    if (prepared.session.has_value() && prepared.session->exists()) {
        if (!args.system.empty() || args.role_given) {
            return fail(Exit::usage, "a session keeps the system text it was created with; "
                                     "drop -s and --role or use a new session name");
        }
        auto model = session_model(config, http, args, prepared.session->data());
        if (!model) {
            return std::unexpected(model.error());
        }
        prepared.model = std::move(*model);
        system = prepared.session->data().system_prompt;
        history = prepared.session->data().history;
    } else {
        if (args.model.empty()) {
            return fail(Exit::usage, "choose a model with -m (alias, id or plain name)");
        }
        auto model = resolve_or_report(config, http, args.model);
        if (!model) {
            return std::unexpected(model.error());
        }
        prepared.model = std::move(*model);
        auto text = system_text(args, config);
        if (!text) {
            return std::unexpected(text.error());
        }
        system = std::move(*text);
        if (prepared.session.has_value()) {
            if (auto created = prepared.session->create(prepared.model.id, system); !created) {
                return std::unexpected(created.error());
            }
        }
    }

    std::string query = args.query;
    if (query == "-") {
        auto text = read_stdin(config.max_input_bytes);
        if (!text) {
            return std::unexpected(text.error());
        }
        query = std::move(*text);
    }
    if (query.empty()) {
        return fail(Exit::usage, "the query is empty");
    }

    auto files = read_files(args, config);
    if (!files) {
        return std::unexpected(files.error());
    }
    if (auto clean = refuse_secrets(history, *files, query, system); !clean) {
        return std::unexpected(clean.error());
    }

    prepared.user_turn = Message{"user", user_message(query, pack_files(*files))};
    if (!system.empty()) {
        prepared.messages.push_back(Message{"system", system});
    }
    std::ranges::copy(history, std::back_inserter(prepared.messages));
    prepared.messages.push_back(prepared.user_turn);

    std::size_t total = 0;
    for (const auto& message : prepared.messages) {
        total = std::add_sat(total, message.content.size());
    }
    if (total > config.max_input_bytes) {
        return fail(Exit::refused, std::format("the request text is {} bytes, over the {} byte "
                                               "limit (OPENROUTER_CLI_MAX_INPUT_BYTES)",
                                               total, config.max_input_bytes));
    }

    RequestInput input;
    input.model = &prepared.model;
    input.messages = prepared.messages;
    input.effort = args.effort.empty() ? config.default_effort : args.effort;
    input.max_output_tokens =
        args.max_output_tokens > 0 ? args.max_output_tokens : config.max_output_tokens;
    input.privacy = args.privacy.empty()            ? config.privacy
                    : args.privacy == "no-training" ? Privacy::no_training
                                                    : Privacy::zdr;
    input.providers = args.providers;
    if (auto checked = check_providers(http, prepared.model, args.providers); !checked) {
        return std::unexpected(checked.error());
    }
    if (prepared.session.has_value()) {
        input.session_id = prepared.session->data().session_uuid;
    }
    auto built = build_request(input);
    if (!built) {
        return std::unexpected(built.error());
    }
    prepared.request = std::move(*built);
    return prepared;
}

// Streams answer text to the answer file and, while it accepts writes, to stdout.
class AnswerSink {
public:
    explicit AnswerSink(FilePtr file) : file_(std::move(file)) {}

    void write(std::string_view text) {
        if (text.empty()) {
            return;
        }
        if (file_ok_ && (std::fwrite(text.data(), 1, text.size(), file_.get()) != text.size() ||
                         std::fflush(file_.get()) != 0)) {
            file_ok_ = false;
        }
        if (stdout_ok_ && (std::fwrite(text.data(), 1, text.size(), stdout) != text.size() ||
                           std::fflush(stdout) != 0)) {
            stdout_ok_ = false;
        }
        ends_with_newline_ = text.ends_with('\n');
    }

    void finish() {
        if (stdout_ok_ && wrote_any() && !ends_with_newline_) {
            std::fputc('\n', stdout);
        }
        std::fflush(stdout);
    }

    [[nodiscard]] bool file_ok() const { return file_ok_; }
    [[nodiscard]] bool wrote_any() const { return std::ftell(file_.get()) > 0; }

private:
    FilePtr file_;
    bool file_ok_ = true;
    bool stdout_ok_ = true;
    bool ends_with_newline_ = true;
};

std::string privacy_name(const nlohmann::json& body) {
    const auto& provider = body["provider"];
    return provider.contains("zdr") ? "zdr" : "no-training";
}

struct StreamRun {
    StreamResult transfer;
    StreamOutcome outcome;
    std::optional<Error> parse_error;
    long long latency_ms = 0;
};

StreamRun run_stream(const Http& http, const Prepared& prepared, AnswerSink& sink,
                     const CostLog& cost_log, const std::string& call_id) {
    StreamRun run;
    SseParser parser;
    ChatStream chat;
    std::atomic<std::size_t> received{0};
    bool generation_logged = false;

    const auto log_generation = [&](std::string_view id) {
        if (generation_logged || id.empty()) {
            return;
        }
        generation_logged = true;
        if (auto logged = cost_log.append(generation_record(call_id, std::string(id))); !logged) {
            std::println(stderr, "openrouter-cli: cost log write failed: {}",
                         logged.error().message);
        }
    };

    StreamHandlers handlers;
    handlers.on_generation_id = log_generation;
    handlers.on_body_bytes = [&](std::string_view bytes) {
        received.fetch_add(bytes.size());
        if (run.parse_error.has_value()) {
            return;
        }
        auto payloads = parser.feed(bytes);
        if (!payloads) {
            run.parse_error = payloads.error();
            return;
        }
        for (const auto& payload : *payloads) {
            auto text = chat.on_data(payload);
            if (!text) {
                run.parse_error = text.error();
                return;
            }
            sink.write(*text);
            if (chat.outcome().generation_id.has_value()) {
                log_generation(*chat.outcome().generation_id);
            }
        }
    };

    const auto started = Clock::now();
    std::jthread heartbeat([&received, started](const std::stop_token& stop) {
        auto next = started + heartbeat_interval;
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(heartbeat_slice);
            if (Clock::now() >= next && !stop.stop_requested()) {
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started);
                std::println(stderr, "openrouter-cli: waiting, {}s elapsed, {} bytes received",
                             elapsed.count(), received.load());
                next += heartbeat_interval;
            }
        }
    });

    // Replacement keeps a file with invalid UTF-8 from aborting the call; dump() throws otherwise.
    const std::string body =
        prepared.request.body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    run.transfer = http.post_stream("/chat/completions", body, handlers);
    heartbeat.request_stop();
    heartbeat.join();
    run.latency_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();

    if (!run.transfer.event_stream) {
        parse_error_body(run.transfer.error_body, run.outcome);
    } else {
        run.outcome = chat.outcome();
    }
    return run;
}

struct Verdict {
    std::string outcome;
    Exit exit = Exit::ok;
    std::string message;
};

std::string incomplete_reason(const StreamOutcome& outcome, long long max_tokens) {
    const auto& reason = outcome.finish_reason;
    if (reason == "length") {
        const auto reasoning = outcome.usage.reasoning_tokens;
        return std::format("the {} token output budget ran out{}; retry with a lower --effort "
                           "or a higher --max-output-tokens",
                           max_tokens,
                           reasoning.has_value()
                               ? std::format(" after {} reasoning tokens", *reasoning)
                               : std::string());
    }
    if (reason == "content_filter") {
        return "the provider's content filter stopped the answer";
    }
    if (outcome.text.empty()) {
        return "the model returned no answer text";
    }
    if (!outcome.done) {
        return "the stream ended before the model finished";
    }
    return std::format("the model stopped with finish reason \"{}\"", reason.value_or("unknown"));
}

Verdict judge(const StreamRun& run, long long max_tokens) {
    if (run.transfer.interrupted) {
        return {"interrupted", Exit::interrupted, "interrupted; the partial answer was kept"};
    }
    if (run.parse_error.has_value()) {
        return {"error", Exit::api, run.parse_error->message};
    }
    if (run.transfer.idle_timeout) {
        return {"error", Exit::api,
                "no data from OpenRouter within the idle timeout "
                "(OPENROUTER_CLI_IDLE_TIMEOUT_SECONDS)"};
    }
    if (run.transfer.total_timeout) {
        return {"error", Exit::api, "the call exceeded OPENROUTER_CLI_TOTAL_TIMEOUT_SECONDS"};
    }
    if (!run.transfer.transport_error.empty()) {
        return {"error", Exit::api, "network error: " + run.transfer.transport_error};
    }
    const auto& outcome = run.outcome;
    if (outcome.error_code.has_value() || outcome.error_type.has_value() ||
        outcome.error_message.has_value() || !run.transfer.event_stream) {
        std::string text = std::format("OpenRouter error (HTTP {}", run.transfer.status);
        if (outcome.error_type.has_value()) {
            text += ", " + *outcome.error_type;
        }
        text += ")";
        if (outcome.error_message.has_value()) {
            text += ": " + sanitize_for_display(*outcome.error_message, max_display_chars);
        }
        if (run.transfer.status == http_unauthorized) {
            text += "; check OPENROUTER_API_KEY in the project .env";
        }
        return {"error", Exit::api, text};
    }
    if (!outcome.complete()) {
        return {"incomplete", Exit::incomplete, incomplete_reason(outcome, max_tokens)};
    }
    return {"complete", Exit::ok, ""};
}

int cmd_ask(const AskArgs& args, const Config& config, const Http& http) {
    auto prepared = prepare(args, config, http);
    if (!prepared) {
        return report(prepared.error());
    }

    const fs::path answers = config.state_dir / "answers";
    for (const auto& dir : {config.state_dir, answers}) {
        if (auto ready = ensure_private_dir(dir); !ready) {
            return report(ready.error());
        }
    }
    const std::string call_id = new_call_id();
    const fs::path answer_path = answers / (call_id + ".md");
    auto fd = create_exclusive(answer_path);
    if (!fd) {
        return report(fd.error());
    }
    FilePtr answer_file(fdopen(*fd, "wb"));
    if (answer_file == nullptr) {
        ::close(*fd);
        return report(
            Error{.code = Exit::refused, .message = "cannot open " + answer_path.string()});
    }
    AnswerSink sink(std::move(answer_file));

    const CostLog cost_log(config.cost_log, config.cost_log.parent_path() / "costs.lock");
    std::error_code cwd_error;
    PendingInfo pending;
    pending.call_id = call_id;
    pending.time = utc_now_iso();
    pending.cwd = fs::current_path(cwd_error).string();
    pending.requested_model = args.model.empty() ? prepared->model.id : args.model;
    pending.model_id = prepared->model.id;
    pending.privacy = privacy_name(prepared->request.body);
    pending.effort = prepared->request.effort_sent;
    if (!args.session.empty()) {
        pending.session = args.session;
    }
    pending.answer_path = answer_path.string();
    if (auto logged = cost_log.append(pending_record(pending)); !logged) {
        return report(Error{.code = Exit::refused,
                            .message = "nothing was sent because the cost log could not be "
                                       "written: " +
                                       logged.error().message});
    }

    std::println(stderr, "openrouter-cli: asking {} (effort {}, up to {} output tokens, {})",
                 prepared->model.id, prepared->request.effort_sent.value_or("default"),
                 prepared->request.max_tokens, pending.privacy);

    const StreamRun run = run_stream(http, *prepared, sink, cost_log, call_id);
    sink.finish();
    const Verdict verdict = judge(run, prepared->request.max_tokens);

    ResultInfo result;
    result.call_id = call_id;
    result.outcome = verdict.outcome;
    result.served_model = run.outcome.served_model;
    result.provider = run.outcome.provider;
    result.usage = run.outcome.usage;
    result.finish_reason = run.outcome.finish_reason;
    result.error_type = run.outcome.error_type;
    result.latency_ms = run.latency_ms;
    if (auto logged = cost_log.append(result_record(result)); !logged) {
        std::println(stderr, "openrouter-cli: cost log write failed: {}", logged.error().message);
    }

    ResultInfo shown = result;
    if (!result.usage.cost.has_value() && run.outcome.generation_id.has_value() &&
        !interrupt_requested()) {
        const auto body =
            http.get("/generation?id=" + *run.outcome.generation_id,
                     GetOptions{.auth = true, .retry_404_for = generation_lookup_window});
        if (body) {
            if (auto stats = parse_generation(*body); stats) {
                if (auto logged = cost_log.append(resolved_record(call_id, *stats)); !logged) {
                    std::println(stderr, "openrouter-cli: cost log write failed: {}",
                                 logged.error().message);
                }
                shown.usage.cost = stats->total_cost;
                if (!shown.provider.has_value()) {
                    shown.provider = stats->provider;
                }
            }
        }
    }

    if (verdict.exit == Exit::ok && prepared->session.has_value()) {
        if (auto saved = prepared->session->commit(prepared->user_turn,
                                                   Message{"assistant", run.outcome.text});
            !saved) {
            std::println(stderr, "openrouter-cli: session not updated: {}", saved.error().message);
        }
    }
    if (!sink.file_ok()) {
        std::println(stderr, "openrouter-cli: writing {} failed; the answer is incomplete on disk",
                     answer_path.string());
    }
    if (!verdict.message.empty()) {
        std::println(stderr, "openrouter-cli: {}", verdict.message);
    }
    std::println(stderr, "openrouter-cli: {}",
                 format_cost_line(shown, prepared->model.id, answer_path.string()));
    return code_of(verdict.exit);
}

// ----- command line -----------------------------------------------------------------------

int run(int argc, char** argv) {
    CLI::App app{"Ask any OpenRouter model to review a plan or code. Options come first; the "
                 "query is last (use - to read it from stdin).",
                 "openrouter-cli"};
    app.set_version_flag("--version", std::string(version_text));
    app.require_subcommand(0, 1);
    app.fallthrough(false);

    AskArgs ask;
    app.add_option("-m,--model", ask.model, "Model alias, id, ~author/family-latest, or name");
    app.add_option("-f,--file", ask.files, "File to include, line-numbered (repeatable)");
    app.add_option("--role", ask.role, "System prompt: review (default) or plain")
        ->check(CLI::IsMember({"review", "plain"}));
    app.add_option("-s,--system", ask.system, "Extra system text, or @FILE");
    app.add_option("-e,--effort", ask.effort,
                   "Reasoning effort: max, xhigh, high, medium, low, minimal, none");
    app.add_option("--max-output-tokens", ask.max_output_tokens,
                   "Output budget including reasoning")
        ->check(CLI::PositiveNumber);
    app.add_option("-p,--provider", ask.providers,
                   "Use only this provider (slug or endpoint tag from `models show`; repeatable)");
    app.add_option("--privacy", ask.privacy, "zdr (default) or no-training")
        ->check(CLI::IsMember({"zdr", "no-training"}));
    app.add_option("--session", ask.session, "Keep history under this name for follow-ups");
    app.add_option("query", ask.query, "The question; - reads it from stdin");

    auto* models = app.add_subcommand("models", "Find, inspect and save model ids");
    models->require_subcommand(1);
    models->fallthrough(false);
    std::string find_name;
    bool refresh = false;
    auto* find = models->add_subcommand("find", "List models matching words");
    find->add_flag("--refresh", refresh, "Refetch the catalog first");
    find->add_option("name", find_name, "Words to match")->required();
    std::string show_name;
    auto* show = models->add_subcommand("show", "Prices, limits and efforts of one model");
    show->add_option("model", show_name, "Alias, id or name")->required();
    std::string save_alias_name;
    std::string save_model_id;
    auto* save = models->add_subcommand("save", "Save an alias for a model id");
    save->add_option("alias", save_alias_name, "Alias to save")->required();
    save->add_option("model_id", save_model_id, "Exact model id")->required();
    auto* list = models->add_subcommand("list", "List saved aliases");

    std::string since = "all";
    auto* spend = app.add_subcommand("spend", "Totals from the global cost log");
    spend->add_option("--since", since, "today, 7d, 30d or all")
        ->check(CLI::IsMember({"today", "7d", "30d", "all"}));
    auto* balance = app.add_subcommand("balance", "Usage and limit of the API key");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        const int code = app.exit(error);
        return code == 0 ? code_of(Exit::ok) : code_of(Exit::usage);
    }

    ask.role_given = app.count("--role") > 0;
    const bool subcommand_used = !app.get_subcommands().empty();
    const bool ask_options_used = std::ranges::any_of(
        std::vector<std::string>{"-m", "-f", "--role", "-s", "-e", "--max-output-tokens",
                                 "--privacy", "--provider", "--session", "query"},
        [&app](const std::string& name) { return app.count(name) > 0; });
    if (subcommand_used && ask_options_used) {
        return report(Error{.code = Exit::usage,
                            .message = "question options cannot be combined with a "
                                       "subcommand; put a query that is a subcommand name "
                                       "after --"});
    }
    if (!subcommand_used && ask.query.empty()) {
        return report(Error{.code = Exit::usage, .message = "missing query; see --help"});
    }

    install_signal_handlers();
    const CurlGlobal curl;
    if (!curl.ok()) {
        return report(Error{.code = Exit::api, .message = "libcurl failed to initialize"});
    }
    const char* home_env = std::getenv("HOME");
    const char* xdg_env = std::getenv("XDG_STATE_HOME");
    const auto config = load_config(default_home(), home_env == nullptr ? "" : home_env,
                                    xdg_env == nullptr ? "" : xdg_env);
    if (!config) {
        return report(config.error());
    }
    const Http http(*config);

    if (find->parsed()) {
        return cmd_models_find(*config, http, find_name, refresh);
    }
    if (show->parsed()) {
        return cmd_models_show(*config, http, show_name);
    }
    if (save->parsed()) {
        return cmd_models_save(*config, http, save_alias_name, save_model_id);
    }
    if (list->parsed()) {
        return cmd_models_list(*config);
    }
    if (spend->parsed()) {
        return cmd_spend(*config, since);
    }
    if (balance->parsed()) {
        return cmd_balance(http);
    }
    return cmd_ask(ask, *config, http);
}

} // namespace
} // namespace orcli

int main(int argc, char** argv) {
    try {
        return orcli::run(argc, argv);
    } catch (const std::exception& error) {
        std::println(stderr, "openrouter-cli: internal error: {}", error.what());
    } catch (...) {
        std::println(stderr, "openrouter-cli: internal error");
    }
    return orcli::internal_error_exit;
}
