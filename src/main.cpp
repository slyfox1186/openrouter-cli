#include "auth.hpp"
#include "commands.hpp"
#include "common.hpp"
#include "config.hpp"
#include "context.hpp"
#include "costlog.hpp"
#include "credentials.hpp"
#include "fsutil.hpp"
#include "help.hpp"
#include "http.hpp"
#include "models.hpp"
#include "request.hpp"
#include "secrets.hpp"
#include "session.hpp"
#include "sse.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <iterator>
#include <numeric>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <linux/prctl.h>
#include <sys/prctl.h>
#include <unistd.h>

namespace orcli {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr std::string_view version_text = "openrouter-cli 0.1.0";
constexpr std::chrono::seconds heartbeat_interval{30};
constexpr std::chrono::seconds generation_lookup_window{30};
constexpr std::size_t max_display_chars = 400;
constexpr int internal_error_exit = 1;
constexpr long http_unauthorized = 401;
constexpr long http_not_found = 404;
constexpr long http_unavailable = 503;
constexpr int default_activity_days = 30;
constexpr int max_activity_days = 30;
constexpr int hours_per_day = 24;
constexpr int week_days = 7;
constexpr int month_days = 30;

int code_of(Exit exit) {
    return exit_code(exit);
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

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string joined;
    for (const auto& part : parts) {
        if (!joined.empty()) {
            joined += separator;
        }
        joined += part;
    }
    return joined;
}

std::string iso_days_ago(int days) {
    const auto then = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()) -
                      std::chrono::hours{hours_per_day * days};
    return std::format("{:%FT%TZ}", then);
}

FetchText catalog_fetcher(const Http& http) {
    return [&http] { return http.get("/models", GetOptions{.auth = Auth::none}); };
}

Result<Catalog> catalog_for(const Config& config, const Http& http, bool refresh) {
    auto catalog = load_catalog(config.state_dir, std::chrono::hours{config.catalog_ttl_hours},
                                catalog_fetcher(http), refresh);
    if (catalog && catalog->stale) {
        diagnostic("openrouter-cli: catalog refresh failed; using cached catalog from {}",
                   display_metadata(catalog->fetched_at));
    }
    return catalog;
}

// ----- models subcommands -----------------------------------------------------------------

int cmd_models_find(const Config& config, const Http& http, const std::string& name, bool refresh) {
    auto catalog = catalog_for(config, http, refresh);
    if (!catalog) {
        return report(catalog.error());
    }
    auto matches = search(name, catalog->models);
    if (matches.empty()) {
        return report(
            Error{.code = Exit::model, .message = std::format("no model matches \"{}\"", name)});
    }
    std::print("{}", format_candidates(matches));
    return code_of(Exit::ok);
}

// Resolves a name for display or saving; ambiguity and misses are reported with candidates.
Result<ModelInfo> resolve_or_report(const Config& config, const Catalog& catalog,
                                    const std::string& name) {
    auto aliases = load_aliases(config.state_dir);
    if (!aliases) {
        return std::unexpected(aliases.error());
    }
    auto resolution = resolve(name, catalog.models, *aliases);
    if (resolution.kind == Resolution::Kind::ambiguous) {
        write_stderr(format_candidates(resolution.candidates));
        return fail(Exit::model, std::format("\"{}\" matches {} models; pass one exact id from "
                                             "the list above",
                                             name, resolution.candidates.size()));
    }
    if (resolution.kind == Resolution::Kind::none || !resolution.model.has_value()) {
        return fail(Exit::model, std::format("no model matches \"{}\"; try `openrouter-cli "
                                             "models find <words>`",
                                             name));
    }
    return std::move(*resolution.model);
}

Result<std::vector<Endpoint>> endpoints_for(const Http& http, const std::string& model_id) {
    const auto body =
        http.get("/models/" + model_id + "/endpoints", GetOptions{.auth = Auth::none});
    if (!body) {
        return std::unexpected(body.error());
    }
    return parse_endpoints(*body);
}

int cmd_models_show(const Config& config, const Http& http, const std::string& name) {
    const auto catalog = catalog_for(config, http, false);
    if (!catalog) {
        return report(catalog.error());
    }
    const auto model = resolve_or_report(config, *catalog, name);
    if (!model) {
        return report(model.error());
    }
    std::print("{}", format_model(*model, catalog->fetched_at));
    const auto endpoints = endpoints_for(http, model->id);
    if (!endpoints) {
        return report(endpoints.error());
    }
    std::print("\n{}", format_endpoints(*endpoints));
    return code_of(Exit::ok);
}

struct AliasTarget {
    std::string alias;
    std::string model_id;
};

int cmd_models_save(const Config& config, const Http& http, const AliasTarget& target) {
    const auto catalog = catalog_for(config, http, false);
    if (!catalog) {
        return report(catalog.error());
    }
    const bool known = std::ranges::any_of(
        catalog->models, [&target](const ModelInfo& model) { return model.id == target.model_id; });
    if (!known) {
        return report(Error{
            .code = Exit::model,
            .message = std::format("\"{}\" is not a model id in the catalog; "
                                   "use `openrouter-cli models find`",
                                   target.model_id),
        });
    }
    const std::string name = normalize_alias(target.alias);
    if (name.empty()) {
        return report(Error{.code = Exit::usage, .message = "the alias has no letters or digits"});
    }
    if (auto saved = save_alias(config.state_dir, name, target.model_id); !saved) {
        return report(saved.error());
    }
    write_stdout(std::format("saved alias {} -> {}\n", display_metadata(name),
                             display_metadata(target.model_id)));
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
        std::println("{} -> {} (saved {})", display_metadata(alias),
                     display_metadata(entry.model_id), display_metadata(entry.saved));
    }
    return code_of(Exit::ok);
}

// ----- spend ------------------------------------------------------------------------------

// The whole cost log, or an empty string when nothing has been logged yet.
Result<std::string> read_cost_log(const Config& config) {
    return CostLog(config.cost_log, config.cost_log.parent_path() / "costs.lock").read_snapshot();
}

int cmd_spend(const Config& config, const std::string& since) {
    std::string since_iso;
    if (since == "today") {
        since_iso = utc_now_iso().substr(0, std::string_view("YYYY-MM-DD").size());
    } else if (since == "7d") {
        since_iso = iso_days_ago(week_days);
    } else if (since == "30d") {
        since_iso = iso_days_ago(month_days);
    }
    const auto contents = read_cost_log(config);
    if (!contents) {
        return report(contents.error());
    }
    if (contents->empty()) {
        std::println("no calls logged yet ({})", config.cost_log.string());
        return code_of(Exit::ok);
    }
    std::print("{}", format_spend(fold_log({.contents = *contents, .since_iso = since_iso})));
    return code_of(Exit::ok);
}

struct ResolveCandidate {
    std::string call_id;
    std::string model;
    std::string generation_id;
};

std::vector<ResolveCandidate> resolve_candidates(const SpendReport& report) {
    std::vector<ResolveCandidate> candidates;
    for (const auto& call : report.unresolved) {
        if (!call.cost.has_value() && call.generation_id.has_value()) {
            candidates.push_back(ResolveCandidate{
                .call_id = call.call_id,
                .model = call.model,
                .generation_id = *call.generation_id,
            });
        }
    }
    return candidates;
}

enum class LookupOutcome : std::uint8_t { resolved, unknown, failed };

// One /generation lookup for spend --resolve; appends the charge when OpenRouter has one.
Result<LookupOutcome> resolve_one(const Http& http, const CostLog& cost_log,
                                  const ResolveCandidate& call) {
    const auto response = http.fetch("/generation?id=" + percent_encode(call.generation_id),
                                     GetOptions{.auth = Auth::inference});
    if (!response) {
        if (response.error().code == Exit::interrupted) {
            return std::unexpected(response.error());
        }
        diagnostic("openrouter-cli: {}: {}", call.call_id, response.error().message);
        return LookupOutcome::failed;
    }
    if (response->status == http_not_found) {
        return LookupOutcome::unknown;
    }
    if (!http_success(response->status)) {
        diagnostic("openrouter-cli: {}: HTTP {}", call.call_id, response->status);
        return LookupOutcome::failed;
    }
    const auto stats = parse_generation(response->body);
    if (!stats) {
        diagnostic("openrouter-cli: {}: {}", call.call_id, stats.error().message);
        return LookupOutcome::failed;
    }
    if (!stats->total_cost.has_value()) {
        return LookupOutcome::unknown;
    }
    if (auto logged = cost_log.append(resolved_record(call.call_id, *stats)); !logged) {
        return fail(Exit::persist, "cost log write failed: " + logged.error().message);
    }
    write_stdout(std::format("{}  {}  ${:g}\n", display_metadata(call.call_id),
                             display_metadata(call.model), *stats->total_cost));
    return LookupOutcome::resolved;
}

// Looks up, oldest first, every logged call whose cost is unknown but whose generation id is
// known. No cap: the lookups are free, and a fixed batch could starve older calls behind ids
// that never resolve. Each found charge is appended at once, so Ctrl-C loses nothing, and its
// line is written without throwing, so a closed stdout cannot stop the remaining lookups.
int cmd_spend_resolve(const Config& config) {
    const auto contents = read_cost_log(config);
    if (!contents) {
        return report(contents.error());
    }
    const auto folded = fold_log({.contents = *contents, .since_iso = ""});
    if (!folded.corrupt_lines.empty()) {
        diagnostic("openrouter-cli: ignored {} corrupt cost-log records",
                   folded.corrupt_lines.size());
    }
    const auto candidates = resolve_candidates(folded);
    if (candidates.empty()) {
        std::println("no unresolved calls with a generation id");
        return code_of(Exit::ok);
    }
    auto key = load_stored_key(config, KeyKind::inference);
    if (!key) {
        return report(key.error());
    }
    const Http http(config, HttpKeys{.inference = std::move(*key), .management = {}});
    const CostLog cost_log(config.cost_log, config.cost_log.parent_path() / "costs.lock");

    int resolved = 0;
    int failed = 0;
    std::vector<std::string> still_unknown;
    for (const auto& call : candidates) {
        const auto outcome = resolve_one(http, cost_log, call);
        if (!outcome) {
            report(outcome.error());
            return code_of(outcome.error().code);
        }
        switch (*outcome) {
        case LookupOutcome::resolved:
            ++resolved;
            break;
        case LookupOutcome::unknown:
            still_unknown.push_back(call.call_id);
            break;
        case LookupOutcome::failed:
            ++failed;
            break;
        }
    }
    if (!still_unknown.empty()) {
        write_stdout(std::format("no charge yet at OpenRouter: {}\n",
                                 display_metadata(join(still_unknown, ", "))));
    }
    write_stdout(std::format("resolved {}, still unknown {}, failed {}, of {} unresolved calls\n",
                             resolved, still_unknown.size(), failed, candidates.size()));
    return failed > 0 ? code_of(Exit::api) : code_of(Exit::ok);
}

// ----- ask --------------------------------------------------------------------------------

struct Prepared {
    ModelInfo model;
    std::optional<Session> session;
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
        auto snapshot = read_snapshot(fs::path(extra.substr(1)),
                                      std::min(config.max_file_bytes, config.max_input_bytes));
        if (!snapshot) {
            return std::unexpected(snapshot.error());
        }
        extra = std::move(snapshot->content);
    }
    return text.empty() ? extra : text + "\n\n" + extra;
}

struct FileLimits {
    std::size_t per_file = 0;
    std::size_t total = 0;
};

Result<std::vector<FileSnapshot>> read_files(const AskArgs& args, const FileLimits& limits) {
    std::vector<FileSnapshot> files;
    std::size_t remaining = limits.total;
    for (const auto& path : args.files) {
        auto snapshot = read_snapshot(fs::path(path), std::min(limits.per_file, remaining));
        if (!snapshot) {
            return std::unexpected(snapshot.error());
        }
        remaining -= snapshot->content.size();
        files.push_back(std::move(*snapshot));
    }
    return files;
}

struct PromptText {
    std::string_view query;
    std::string_view system;
};

Result<void> refuse_secrets(const std::vector<Message>& messages,
                            const std::vector<FileSnapshot>& files, const PromptText& prompt) {
    std::vector<SecretFinding> findings =
        scan_text({.text = prompt.system, .source = "system text"});
    std::ranges::move(scan_text({.text = prompt.query, .source = "query"}),
                      std::back_inserter(findings));
    for (const auto& message : messages) {
        std::ranges::move(scan_text({.text = message.content, .source = "session history"}),
                          std::back_inserter(findings));
    }
    for (const auto& file : files) {
        std::ranges::move(scan_text({.text = file.content, .source = file.display_path}),
                          std::back_inserter(findings));
    }
    if (findings.empty()) {
        return {};
    }
    for (const auto& finding : findings) {
        diagnostic("openrouter-cli: possible secret in {}:{} ({})", finding.source, finding.line,
                   finding.pattern);
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
        auto requested = resolve_or_report(config, *catalog, args.model);
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

struct Conversation {
    std::string system;
    std::vector<Message> history;
};

Result<Conversation> prepare_conversation(Prepared& prepared, const AskArgs& args,
                                          const Config& config, const Http& http) {
    Conversation conversation;
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
        conversation.system = prepared.session->data().system_prompt;
        conversation.history = prepared.session->data().history;
    } else {
        if (args.model.empty()) {
            return fail(Exit::usage, "choose a model with -m (alias, id or plain name)");
        }
        const auto catalog = catalog_for(config, http, false);
        if (!catalog) {
            return std::unexpected(catalog.error());
        }
        auto model = resolve_or_report(config, *catalog, args.model);
        if (!model) {
            return std::unexpected(model.error());
        }
        prepared.model = std::move(*model);
        auto text = system_text(args, config);
        if (!text) {
            return std::unexpected(text.error());
        }
        conversation.system = std::move(*text);
        if (prepared.session.has_value()) {
            if (auto created = prepared.session->create(prepared.model.id, conversation.system);
                !created) {
                return std::unexpected(created.error());
            }
        }
    }

    return conversation;
}

// The query as given, or standard input for "-", read within what is left of the input budget.
Result<std::string> query_text(const AskArgs& args, std::size_t remaining) {
    if (args.query != "-") {
        return args.query;
    }
    auto text = read_input(remaining, false);
    if (!text) {
        return fail(text.error().code == Exit::usage ? Exit::refused : text.error().code,
                    text.error().message);
    }
    return std::move(*text);
}

Privacy selected_privacy(const AskArgs& args, const Config& config) {
    if (args.privacy.empty()) {
        return config.privacy;
    }
    return args.privacy == "no-training" ? Privacy::no_training : Privacy::zdr;
}

Result<Prepared> prepare(const AskArgs& args, const Config& config, const Http& http) {
    Prepared prepared;
    auto conversation = prepare_conversation(prepared, args, config, http);
    if (!conversation) {
        return std::unexpected(conversation.error());
    }
    auto& [system, history] = *conversation;

    std::size_t remaining = config.max_input_bytes;
    const auto consume = [&](std::size_t bytes) {
        if (bytes > remaining) {
            return false;
        }
        remaining -= bytes;
        return true;
    };
    if (!consume(system.size())) {
        return fail(Exit::refused, "request text exceeds OPENROUTER_CLI_MAX_INPUT_BYTES");
    }
    for (const auto& message : history) {
        if (!consume(message.content.size())) {
            return fail(Exit::refused, "session history exceeds OPENROUTER_CLI_MAX_INPUT_BYTES");
        }
    }
    const auto query_read = query_text(args, remaining);
    if (!query_read) {
        return std::unexpected(query_read.error());
    }
    const std::string& query = *query_read;
    if (query.empty()) {
        return fail(Exit::usage, "the query is empty");
    }

    if (!consume(query.size())) {
        return fail(Exit::refused, "request text exceeds OPENROUTER_CLI_MAX_INPUT_BYTES");
    }
    auto files = read_files(args, {.per_file = config.max_file_bytes, .total = remaining});
    if (!files) {
        return std::unexpected(files.error());
    }
    if (auto clean = refuse_secrets(history, *files, {.query = query, .system = system}); !clean) {
        return std::unexpected(clean.error());
    }

    constexpr std::size_t separator_bytes = 2;
    if (!files->empty() && !consume(separator_bytes)) {
        return fail(Exit::refused, "request formatting exceeds OPENROUTER_CLI_MAX_INPUT_BYTES");
    }
    const auto packed = pack_files(*files, remaining);
    if (!packed) {
        return std::unexpected(packed.error());
    }
    prepared.user_turn = Message{.role = "user", .content = user_message(query, *packed)};

    // The checks above were the last readers of the system text and the history, so both move
    // into the request; only the new turn is copied, because the session commit needs it later.
    RequestInput input;
    input.model = &prepared.model;
    input.messages.reserve(history.size() + 2);
    if (!system.empty()) {
        input.messages.push_back(Message{.role = "system", .content = std::move(system)});
    }
    std::ranges::move(history, std::back_inserter(input.messages));
    input.messages.push_back(prepared.user_turn);

    std::size_t total = 0;
    for (const auto& message : input.messages) {
        total = std::add_sat(total, message.content.size());
    }
    if (total > config.max_input_bytes) {
        return fail(Exit::refused, std::format("the request text is {} bytes, over the {} byte "
                                               "limit (OPENROUTER_CLI_MAX_INPUT_BYTES)",
                                               total, config.max_input_bytes));
    }

    input.effort = args.effort.empty() ? config.default_effort : args.effort;
    input.max_output_tokens =
        args.max_output_tokens > 0 ? args.max_output_tokens : config.max_output_tokens;
    input.privacy = selected_privacy(args, config);
    input.providers = args.providers;
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

// Owns the exclusively created descriptor; stdout failure never prevents saving the answer.
class AnswerSink {
public:
    explicit AnswerSink(int descriptor) noexcept : descriptor_(descriptor) {}
    AnswerSink(const AnswerSink&) = delete;
    AnswerSink& operator=(const AnswerSink&) = delete;
    AnswerSink(AnswerSink&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)), file_ok_(other.file_ok_),
          stdout_ok_(other.stdout_ok_), ends_with_newline_(other.ends_with_newline_),
          wrote_any_(other.wrote_any_) {}
    AnswerSink& operator=(AnswerSink&&) = delete;
    ~AnswerSink() { close_file(); }

    void write(std::string_view text) {
        if (text.empty()) {
            return;
        }
        if (file_ok_) {
            file_ok_ = write_file(text);
        }
        if (stdout_ok_ && (std::fwrite(text.data(), 1, text.size(), stdout) != text.size() ||
                           std::fflush(stdout) != 0)) {
            stdout_ok_ = false;
        }
        wrote_any_ = true;
        ends_with_newline_ = text.ends_with('\n');
    }

    void finish(const fs::path& path) {
        if (file_ok_ && !sync_file(descriptor_, path)) {
            file_ok_ = false;
        }
        if (stdout_ok_ && wrote_any_ && !ends_with_newline_ && std::fputc('\n', stdout) == EOF) {
            stdout_ok_ = false;
        }
        if (stdout_ok_ && std::fflush(stdout) != 0) {
            stdout_ok_ = false;
        }
        close_file();
    }

    [[nodiscard]] bool file_ok() const { return file_ok_; }

private:
    [[nodiscard]] bool write_file(std::string_view pending) const {
        while (!pending.empty()) {
            const auto written = ::write(descriptor_, pending.data(), pending.size());
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                return false;
            }
            pending.remove_prefix(static_cast<std::size_t>(written));
        }
        return true;
    }

    void close_file() noexcept {
        if (descriptor_ >= 0) {
            // Linux releases the descriptor even if close reports EINTR; never retry it.
            if (::close(descriptor_) != 0) {
                file_ok_ = false;
            }
            descriptor_ = -1;
        }
    }

    int descriptor_;
    bool file_ok_ = true;
    bool stdout_ok_ = true;
    bool ends_with_newline_ = true;
    bool wrote_any_ = false;
};

std::string_view privacy_name(const nlohmann::json& body) {
    const auto& provider = body.at("provider");
    return provider.contains("zdr") ? "zdr" : "no-training";
}

struct StreamRun {
    StreamResult transfer;
    StreamOutcome outcome;
    std::optional<Error> parse_error;
    // From X-Generation-Id or the first chunk, whichever arrived first.
    std::optional<std::string> generation_id;
    // The id reached the cost log; when false, cmd_ask retries once so `spend --resolve` can
    // still recover the charge later.
    bool generation_saved = false;
    long long latency_ms = 0;
};

// A timeout or interrupt may leave a generation running; only ended streams can be priced.
bool stream_ended(const StreamRun& run) {
    return !run.transfer.interrupted && !run.transfer.idle_timeout && !run.transfer.total_timeout &&
           run.transfer.transport_error.empty() && !run.parse_error.has_value();
}

StreamRun run_stream(const Http& http, const Prepared& prepared, AnswerSink& sink,
                     const CostLog& cost_log, const std::string& call_id) {
    StreamRun run;
    SseParser parser;
    ChatStream chat;
    std::size_t received = 0;
    bool generation_logged = false;

    const auto log_generation = [&](std::string_view generation_id) {
        if (generation_logged || !valid_generation_id(generation_id)) {
            return;
        }
        generation_logged = true;
        run.generation_id = generation_id;
        if (auto logged = cost_log.append(
                generation_record({.call_id = call_id, .generation_id = generation_id}));
            logged) {
            run.generation_saved = true;
        } else {
            diagnostic("openrouter-cli: cost log write failed: {}", logged.error().message);
        }
    };

    StreamHandlers handlers;
    handlers.on_generation_id = log_generation;
    handlers.on_body_bytes = [&](std::string_view bytes) {
        received += bytes.size();
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
    auto next = started + heartbeat_interval;
    handlers.should_continue = [&] { return !run.parse_error.has_value(); };
    handlers.on_progress = [&] {
        if (Clock::now() >= next) {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started);
            diagnostic("openrouter-cli: waiting, {}s elapsed, {} bytes received", elapsed.count(),
                       received);
            next = Clock::now() + heartbeat_interval;
        }
    };

    // Replacement keeps a file with invalid UTF-8 from aborting the call; dump() throws otherwise.
    const std::string body =
        prepared.request.body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    run.transfer = http.post_stream("/chat/completions", body, handlers);
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
    return "the provider stopped the answer with an unsupported finish reason";
}

Verdict judge(const StreamRun& run, long long max_tokens) {
    if (run.transfer.interrupted) {
        return {
            .outcome = "interrupted",
            .exit = Exit::interrupted,
            .message = "interrupted; the partial answer was kept",
        };
    }
    if (run.parse_error.has_value()) {
        return {.outcome = "error", .exit = Exit::api, .message = run.parse_error->message};
    }
    if (run.transfer.idle_timeout) {
        return {
            .outcome = "error",
            .exit = Exit::api,
            .message = "no data sent or received within the idle timeout "
                       "(OPENROUTER_CLI_IDLE_TIMEOUT_SECONDS)",
        };
    }
    if (run.transfer.total_timeout) {
        return {
            .outcome = "error",
            .exit = Exit::api,
            .message = "the call exceeded OPENROUTER_CLI_TOTAL_TIMEOUT_SECONDS",
        };
    }
    if (!run.transfer.transport_error.empty()) {
        return {
            .outcome = "error",
            .exit = Exit::api,
            .message = "network error: " + run.transfer.transport_error,
        };
    }
    const auto& outcome = run.outcome;
    if (outcome.error_code.has_value() || outcome.error_type.has_value() ||
        outcome.error_message.has_value() || !run.transfer.event_stream) {
        std::string text = std::format("OpenRouter error (HTTP {}", run.transfer.status);
        if (outcome.error_code.has_value()) {
            text += ", code " + *outcome.error_code;
        }
        if (outcome.error_type.has_value()) {
            text += ", " + *outcome.error_type;
        }
        text += ')';
        if (outcome.error_message.has_value()) {
            text += ": " + sanitize_for_display(*outcome.error_message, max_display_chars);
        }
        if (run.transfer.status == http_unauthorized) {
            text += "; the stored API key was rejected: run `openrouter-cli auth login --replace`";
        } else if (run.transfer.status == http_not_found ||
                   run.transfer.status == http_unavailable) {
            text += "; check the -p tags with `openrouter-cli models show MODEL`, and under "
                    "--privacy zdr try --privacy no-training";
        }
        return {.outcome = "error", .exit = Exit::api, .message = text};
    }
    if (!stream_complete(outcome)) {
        return {
            .outcome = "incomplete",
            .exit = Exit::incomplete,
            .message = incomplete_reason(outcome, max_tokens),
        };
    }
    return {.outcome = "complete", .exit = Exit::ok, .message = ""};
}

// Looks up the charge of a generation whose stream ended without one. Returns the stats, or
// nullopt when OpenRouter has none yet; Exit::interrupted when Ctrl-C stopped the wait.
Result<std::optional<GenerationStats>> look_up_cost(const Http& http,
                                                    const std::string& generation_id) {
    const auto body =
        http.get("/generation?id=" + percent_encode(generation_id),
                 GetOptions{.auth = Auth::inference, .retry_404_for = generation_lookup_window});
    if (!body) {
        if (body.error().code == Exit::interrupted) {
            return std::unexpected(body.error());
        }
        return std::optional<GenerationStats>();
    }
    auto stats = parse_generation(*body);
    if (!stats) {
        return std::optional<GenerationStats>();
    }
    return std::optional<GenerationStats>(std::move(*stats));
}

// Creates answers/<call_id>.md exclusively; the path is never reused or overwritten.
Result<AnswerSink> open_answer(const fs::path& answer_path) {
    const auto descriptor = create_exclusive(answer_path);
    if (!descriptor) {
        return std::unexpected(descriptor.error());
    }
    return AnswerSink(*descriptor);
}

PendingInfo pending_for(const AskArgs& args, const Prepared& prepared, const std::string& call_id,
                        const fs::path& answer_path) {
    std::error_code cwd_error;
    PendingInfo pending;
    pending.call_id = call_id;
    pending.time = utc_now_iso();
    pending.cwd = fs::current_path(cwd_error).string();
    pending.requested_model = args.model.empty() ? prepared.model.id : args.model;
    pending.model_id = prepared.model.id;
    pending.privacy = privacy_name(prepared.request.body);
    pending.effort = prepared.request.effort_sent;
    if (!args.session.empty()) {
        pending.session = args.session;
    }
    pending.answer_path = answer_path.string();
    return pending;
}

ResultInfo result_for(const std::string& call_id, const Verdict& verdict, const StreamRun& run) {
    ResultInfo result;
    result.call_id = call_id;
    result.outcome = verdict.outcome;
    result.served_model = run.outcome.served_model;
    result.provider = run.outcome.provider;
    result.usage = run.outcome.usage;
    result.finish_reason = run.outcome.finish_reason;
    result.error_type = run.outcome.error_type;
    result.latency_ms = run.latency_ms;
    return result;
}

// What happened after the stream: the cost line to show, whether Ctrl-C stopped the cost
// lookup, and every record that could not be saved.
struct Aftermath {
    ResultInfo shown;
    bool lookup_interrupted = false;
    std::vector<std::string> unsaved;
};

void note_unsaved(Aftermath& after, const Result<void>& saved, std::string_view what) {
    if (!saved) {
        diagnostic("openrouter-cli: saving {} failed: {}", what, saved.error().message);
        after.unsaved.emplace_back(what);
    }
}

Aftermath record_outcome(const Http& http, const CostLog& cost_log, const std::string& call_id,
                         const StreamRun& run, const Verdict& verdict) {
    Aftermath after;
    after.shown = result_for(call_id, verdict, run);
    constexpr std::chrono::seconds cleanup_window{2};
    LockDeadline cleanup_deadline;
    const auto append = [&](const nlohmann::json& record) {
        if (interrupt_requested() && !cleanup_deadline.has_value()) {
            cleanup_deadline = Clock::now() + cleanup_window;
        }
        auto saved = cost_log.append(record, cleanup_deadline);
        if (!saved && saved.error().code == Exit::interrupted && !cleanup_deadline.has_value()) {
            cleanup_deadline = Clock::now() + cleanup_window;
            saved = cost_log.append(record, cleanup_deadline);
        }
        return saved;
    };
    // A first append that failed mid-stream is retried here; without the id `spend --resolve`
    // could never recover this call's charge.
    if (run.generation_id.has_value() && !run.generation_saved) {
        note_unsaved(
            after,
            append(generation_record({.call_id = call_id, .generation_id = *run.generation_id})),
            "the generation id");
    }
    note_unsaved(after, append(result_record(after.shown)), "the cost record");
    if (after.shown.usage.cost.has_value() || !run.generation_id.has_value() ||
        !stream_ended(run)) {
        return after;
    }
    const auto stats = look_up_cost(http, *run.generation_id);
    if (!stats) {
        after.lookup_interrupted = true;
        return after;
    }
    if (stats->has_value() && (*stats)->total_cost.has_value()) {
        note_unsaved(after, append(resolved_record(call_id, **stats)), "the looked-up cost");
        after.shown.usage.cost = (*stats)->total_cost;
        if (!after.shown.provider.has_value()) {
            after.shown.provider = (*stats)->provider;
        }
    }
    return after;
}

// Precedence: Ctrl-C (also during the cost lookup), then API/incomplete outcomes, then a
// failed save of a call that may have been billed.
int final_exit(const Verdict& verdict, const Aftermath& after) {
    if (verdict.exit == Exit::interrupted || after.lookup_interrupted || interrupt_requested()) {
        if (after.lookup_interrupted) {
            diagnostic("openrouter-cli: interrupted while looking up the cost; run "
                       "`openrouter-cli spend --resolve` later");
        }
        return code_of(Exit::interrupted);
    }
    if (verdict.exit != Exit::ok) {
        return code_of(verdict.exit);
    }
    if (!after.unsaved.empty()) {
        diagnostic("openrouter-cli: the answer arrived (the call may have been billed) but "
                   "saving failed: {}; do not retry automatically",
                   join(after.unsaved, ", "));
        return code_of(Exit::persist);
    }
    return code_of(Exit::ok);
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
    auto sink = open_answer(answer_path);
    if (!sink) {
        return report(sink.error());
    }

    const CostLog cost_log(config.cost_log, config.cost_log.parent_path() / "costs.lock");
    const PendingInfo pending = pending_for(args, *prepared, call_id, answer_path);
    if (auto logged = cost_log.append(pending_record(pending)); !logged) {
        // Nothing was sent, so the empty answer file would only mislead.
        std::error_code ignored;
        fs::remove(answer_path, ignored);
        if (logged.error().code == Exit::interrupted) {
            return report(logged.error());
        }
        return report(Error{
            .code = Exit::refused,
            .message = "nothing was sent because the cost log could not be "
                       "written: " +
                       logged.error().message,
        });
    }
    diagnostic("openrouter-cli: asking {} (effort {}, up to {} output tokens, {}{})",
               display_metadata(prepared->model.id),
               display_metadata(prepared->request.effort_sent.value_or("default")),
               prepared->request.max_tokens, pending.privacy,
               args.providers.empty()
                   ? std::string()
                   : ", providers " + display_metadata(join(args.providers, ",")));

    const StreamRun run = run_stream(http, *prepared, *sink, cost_log, call_id);
    sink->finish(answer_path);
    const Verdict verdict = judge(run, prepared->request.max_tokens);
    Aftermath after = record_outcome(http, cost_log, call_id, run, verdict);

    if (verdict.exit == Exit::ok && prepared->session.has_value()) {
        note_unsaved(
            after,
            prepared->session->commit(prepared->user_turn,
                                      Message{.role = "assistant", .content = run.outcome.text}),
            "the session");
    }
    if (!sink->file_ok()) {
        diagnostic("openrouter-cli: writing {} failed; the answer is incomplete on disk",
                   answer_path.string());
        after.unsaved.emplace_back("the answer file");
    }
    if (!verdict.message.empty()) {
        diagnostic("openrouter-cli: {}", verdict.message);
    }
    diagnostic("openrouter-cli: {}",
               format_cost_line(after.shown, prepared->model.id, answer_path.string()));
    return final_exit(verdict, after);
}

// ----- command line -----------------------------------------------------------------------

// Every parsed option and subcommand of the command line.
struct Cli {
    AskArgs ask;
    std::string find_name;
    bool refresh = false;
    std::string show_name;
    std::string save_alias_name;
    std::string save_model_id;
    std::string since = "all";
    bool resolve = false;
    bool login_replace = false;
    bool set_management = false;
    bool set_replace = false;
    bool logout_management = false;
    int activity_days = default_activity_days;

    CLI::App* find = nullptr;
    CLI::App* show = nullptr;
    CLI::App* save = nullptr;
    CLI::App* list = nullptr;
    CLI::App* spend = nullptr;
    CLI::App* login = nullptr;
    CLI::App* set_key = nullptr;
    CLI::App* logout = nullptr;
    CLI::App* status = nullptr;
    CLI::App* balance = nullptr;
    CLI::App* keys = nullptr;
    CLI::App* credits = nullptr;
    CLI::App* activity = nullptr;
};

void add_ask_options(CLI::App& app, AskArgs& ask) {
    app.add_option("-m,--model", ask.model, "Model alias, id, ~author/family-latest, or name");
    // One value per occurrence: CLI11 lets a vector option take every following argument by
    // default, which would swallow the query (CLI11 book, "Options": allow_extra_args).
    app.add_option("-f,--file", ask.files, "File to include, line-numbered (repeatable)")
        ->allow_extra_args(false);
    app.add_option("--role", ask.role, "System prompt: review (default) or plain")
        ->check(CLI::IsMember({"review", "plain"}));
    app.add_option("-s,--system", ask.system, "Extra system text, or @FILE")->option_text("TEXT");
    app.add_option("-e,--effort", ask.effort,
                   "Reasoning effort: max, xhigh, high, medium, low, minimal, none")
        ->option_text("LEVEL");
    app.add_option("--max-output-tokens", ask.max_output_tokens,
                   "Output budget including reasoning")
        ->check(CLI::PositiveNumber)
        ->option_text("N");
    app.add_option("-p,--provider", ask.providers,
                   "Use only this provider (endpoint tag from `models show`; repeatable)")
        ->allow_extra_args(false)
        ->option_text("TAG");
    app.add_option("--privacy", ask.privacy, "zdr (default) or no-training")
        ->check(CLI::IsMember({"zdr", "no-training"}))
        ->option_text("MODE");
    app.add_option("--session", ask.session, "Keep history under this name for follow-ups")
        ->option_text("NAME");
    app.add_option("query", ask.query, "The question; - reads it from stdin");
}

void add_models_commands(CLI::App& app, Cli& cli) {
    auto* models = app.add_subcommand("models", "Find, inspect and save model ids");
    models->require_subcommand(1);
    models->fallthrough(false);
    cli.find = models->add_subcommand("find", "List models matching words");
    cli.find->add_flag("--refresh", cli.refresh, "Refetch the catalog first");
    cli.find->add_option("name", cli.find_name, "Words to match")->required();
    cli.show = models->add_subcommand("show", "Prices, limits, efforts and provider tags");
    cli.show->add_option("model", cli.show_name, "Alias, id or name")->required();
    cli.save = models->add_subcommand("save", "Save an alias for a model id");
    cli.save->add_option("alias", cli.save_alias_name, "Alias to save")->required();
    cli.save->add_option("model_id", cli.save_model_id, "Exact model id")->required();
    cli.list = models->add_subcommand("list", "List saved aliases");
}

void add_account_commands(CLI::App& app, Cli& cli) {
    cli.spend = app.add_subcommand("spend", "Totals from the global cost log");
    cli.spend->add_option("--since", cli.since, "today, 7d, 30d or all")
        ->check(CLI::IsMember({"today", "7d", "30d", "all"}))
        ->option_text("PERIOD");
    cli.spend->add_flag("--resolve", cli.resolve,
                        "Look up the charge of earlier calls whose cost is still unknown (free)");

    auto* auth = app.add_subcommand("auth", "Log in to OpenRouter and manage the stored keys");
    auth->require_subcommand(1);
    auth->fallthrough(false);
    cli.login = auth->add_subcommand(
        "login", "Sign in with your OpenRouter account in a browser; stores a new API key");
    cli.login->add_flag("--replace", cli.login_replace, "Replace the stored API key");
    cli.set_key = auth->add_subcommand(
        "set", "Store an existing key, read from a hidden prompt or from stdin");
    cli.set_key->add_flag("--management", cli.set_management, "The key is a Management key");
    cli.set_key->add_flag("--replace", cli.set_replace, "Replace a stored key of the same kind");
    cli.logout = auth->add_subcommand("logout", "Delete the stored keys from this PC");
    cli.logout->add_flag("--management", cli.logout_management, "Delete only the Management key");

    cli.status =
        app.add_subcommand("status", "Who is logged in: account identity, key usage and limits");

    auto* account = app.add_subcommand("account", "OpenRouter account information");
    account->require_subcommand(1);
    account->fallthrough(false);
    cli.balance = account->add_subcommand("balance", "Usage and limit of the stored API key");
    cli.keys = account->add_subcommand("keys", "Every API key (needs a Management key)");
    cli.credits =
        account->add_subcommand("credits", "Credits bought and used (needs a Management key)");
    cli.activity = account->add_subcommand(
        "activity", "Usage per day, model and provider (needs a Management key)");
    cli.activity
        ->add_option("--days", cli.activity_days,
                     "Completed UTC days to show, 1 to 30 (default 30)")
        ->check(CLI::Range(1, max_activity_days))
        ->option_text("N");
}

// Commands that never touch a decrypted key or the plaintext guard come first: set and login
// are how keys move out of .env, logout is how stored keys are removed.
std::optional<int> dispatch_auth(const Cli& cli, const Config& config) {
    if (cli.login->parsed()) {
        return cmd_auth_login(config, cli.login_replace);
    }
    if (cli.set_key->parsed()) {
        return cmd_auth_set(config, cli.set_management, cli.set_replace);
    }
    if (cli.logout->parsed()) {
        return cmd_auth_logout(config, cli.logout_management);
    }
    return std::nullopt;
}

std::optional<int> dispatch_account(const Cli& cli, const Config& config) {
    if (cli.status->parsed()) {
        return cmd_status(config);
    }
    if (cli.balance->parsed()) {
        return cmd_account_balance(config);
    }
    if (cli.keys->parsed()) {
        return cmd_account_keys(config);
    }
    if (cli.credits->parsed()) {
        return cmd_account_credits(config);
    }
    if (cli.activity->parsed()) {
        return cmd_account_activity(config, cli.activity_days);
    }
    if (cli.spend->parsed()) {
        return cli.resolve ? cmd_spend_resolve(config) : cmd_spend(config, cli.since);
    }
    return std::nullopt;
}

std::optional<int> dispatch_models(const Cli& cli, const Config& config) {
    const Http public_http(config, HttpKeys{});
    if (cli.find->parsed()) {
        return cmd_models_find(config, public_http, cli.find_name, cli.refresh);
    }
    if (cli.show->parsed()) {
        return cmd_models_show(config, public_http, cli.show_name);
    }
    if (cli.save->parsed()) {
        return cmd_models_save(config, public_http,
                               {.alias = cli.save_alias_name, .model_id = cli.save_model_id});
    }
    if (cli.list->parsed()) {
        return cmd_models_list(config);
    }
    return std::nullopt;
}

int dispatch(const Cli& cli, const Config& config) {
    if (auto code = dispatch_auth(cli, config)) {
        return *code;
    }
    if (warn_plaintext_keys(config)) {
        return report(Error{
            .code = Exit::config,
            .message = "refusing to run while .env holds a plaintext key; "
                       "keys are stored only encrypted",
        });
    }
    if (auto code = dispatch_account(cli, config)) {
        return *code;
    }
    if (auto code = dispatch_models(cli, config)) {
        return *code;
    }
    auto key = load_stored_key(config, KeyKind::inference);
    if (!key) {
        return report(key.error());
    }
    const Http http(config, HttpKeys{.inference = std::move(*key), .management = {}});
    return cmd_ask(cli.ask, config, http);
}

// Question options and subcommands are separate grammars; mixing them is almost always a
// query that happens to be a subcommand name.
std::optional<Error> check_grammar(const CLI::App& app, const AskArgs& ask) {
    const bool subcommand_used = !app.get_subcommands().empty();
    const bool ask_options_used = std::ranges::any_of(
        std::vector<std::string>{
            "-m",
            "-f",
            "--role",
            "-s",
            "-e",
            "--max-output-tokens",
            "--privacy",
            "--provider",
            "--session",
            "query",
        },
        [&app](const std::string& name) { return app.count(name) > 0; });
    if (subcommand_used && ask_options_used) {
        return Error{
            .code = Exit::usage,
            .message = "question options cannot be combined with a subcommand; put a "
                       "query that is a subcommand name after --",
        };
    }
    if (!subcommand_used && ask.query.empty()) {
        return Error{.code = Exit::usage, .message = "missing query; see --help"};
    }
    return std::nullopt;
}

// Examples use a placeholder alias and model id so the binary names no vendor. A session
// follow-up drops -m because a session refuses any model but the one it was created with.
HelpContent root_help() {
    return HelpContent{
        .title = std::string(version_text),
        .examples =
            {
                HelpExample{
                    .caption = "Save a short alias for a model id",
                    .commands = {"openrouter-cli models save reviewer AUTHOR/MODEL"},
                },
                HelpExample{
                    .caption = "Review a plan file",
                    .commands = {R"(openrouter-cli -m reviewer -f plan.md "Review this plan")"},
                },
                HelpExample{
                    .caption = "Send a diff as the question, read from stdin",
                    .commands = {"git diff | openrouter-cli -m reviewer -"},
                },
                HelpExample{
                    .caption =
                        "Keep a named conversation, then follow up (the session keeps its model)",
                    .commands =
                        {
                            R"(openrouter-cli --session plan -m reviewer "Review it")",
                            R"(openrouter-cli --session plan "Is step 3 sound now?")",
                        },
                },
            },
        .notes =
            {
                "Options come first and the query comes last. Put -- before a query that starts "
                "with - or matches a command name. Question options cannot be combined with a "
                "command.",
            },
    };
}

int run(std::span<char* const> argv) {
    CLI::App app{"Ask any OpenRouter model to review a plan or code.", "openrouter-cli"};
    // Subcommands copy the formatter, the help flag and the option defaults when they are
    // created, so all three are set before any subcommand is added.
    app.formatter(std::make_shared<HelpFormatter>(root_help()));
    app.set_help_flag("-h,--help", "Show this help and exit")->group("GENERAL");
    app.set_version_flag("--version", std::string(version_text), "Show the version and exit")
        ->group("GENERAL");
    app.require_subcommand(0, 1);
    app.fallthrough(false);
    Cli cli;
    app.option_defaults()->group("QUESTION OPTIONS");
    add_ask_options(app, cli.ask);
    app.option_defaults()->group("OPTIONS");
    add_models_commands(app, cli);
    add_account_commands(app, cli);

    try {
        app.parse(static_cast<int>(argv.size()), argv.data());
    } catch (const CLI::ParseError& error) {
        const int code = app.exit(error);
        return code == 0 ? code_of(Exit::ok) : code_of(Exit::usage);
    }
    cli.ask.role_given = app.count("--role") > 0;
    if (auto invalid = check_grammar(app, cli.ask)) {
        return report(*invalid);
    }

    // A crash must not leave a core dump holding a decrypted key, so a failure here stops the
    // command before any key is read.
    if (::prctl(PR_SET_DUMPABLE, 0) != 0) {
        return report(Error{
            .code = Exit::config,
            .message = "could not disable core dumps (prctl PR_SET_DUMPABLE); "
                       "refusing to handle keys",
        });
    }
    install_signal_handlers();
    const CurlGlobal curl;
    if (!curl.ok()) {
        return report(Error{.code = Exit::api, .message = "libcurl failed to initialize"});
    }
    const char* home_env = std::getenv("HOME");
    const char* xdg_env = std::getenv("XDG_STATE_HOME");
    const char* config_env = std::getenv("XDG_CONFIG_HOME");
    const auto config =
        load_config(default_home(), home_env == nullptr ? "" : home_env,
                    xdg_env == nullptr ? "" : xdg_env, config_env == nullptr ? "" : config_env);
    if (!config) {
        return report(config.error());
    }
    return dispatch(cli, *config);
}

} // namespace
} // namespace orcli

int main(int argc, char** argv) {
    try {
        return orcli::run(std::span(argv, static_cast<std::size_t>(argc)));
    } catch (...) {
        // The final error path must not allocate, throw, or expose provider/key material.
        constexpr std::string_view message = "openrouter-cli: internal error\n";
        std::string_view pending = message;
        while (!pending.empty()) {
            const auto written = ::write(STDERR_FILENO, pending.data(), pending.size());
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                break;
            }
            pending.remove_prefix(static_cast<std::size_t>(written));
        }
        return orcli::internal_error_exit;
    }
}
