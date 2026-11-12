#include "auth.hpp"
#include "commands.hpp"
#include "common.hpp"
#include "config.hpp"
#include "context.hpp"
#include "costlog.hpp"
#include "credentials.hpp"
#include "display.hpp"
#include "fsutil.hpp"
#include "group.hpp"
#include "help.hpp"
#include "http.hpp"
#include "models.hpp"
#include "progress.hpp"
#include "registry.hpp"
#include "request.hpp"
#include "session.hpp"
#include "sse.hpp"
#include "terminal.hpp"

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
#include <memory>
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
    std::string role = "plain";
    bool role_given = false;
    std::string system;
    std::string effort;
    long long max_output_tokens = 0;
    std::string privacy;
    std::vector<std::string> providers;
    bool any_provider = false;
    std::string group;
    // Internal: a group member refuses to send unless -m resolves to exactly this id.
    std::string expect_model_id;
    std::string session;
    std::string query;
    // The query is the text itself even when it is "-" (a group member's buffered stdin).
    bool query_is_text = false;
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
    const auto matches = search(name, catalog->models);
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
                                             "find <words>`",
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
    const auto saved = load_providers(config.state_dir);
    if (!saved) {
        return report(saved.error());
    }
    const auto pinned = saved->find(model->id);
    std::print("{}", format_model(*model, catalog->fetched_at, config.max_output_tokens,
                                  pinned == saved->end() ? std::vector<std::string>{}
                                                         : pinned->second.tags));
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
                                   "use `openrouter-cli find`",
                                   target.model_id),
        });
    }
    const std::string name = normalize_alias(target.alias);
    if (name.empty()) {
        return report(Error{.code = Exit::usage, .message = "the alias has no letters or digits"});
    }
    const auto replaced = save_alias(config.state_dir, name, target.model_id);
    if (!replaced) {
        return report(replaced.error());
    }
    std::string text = std::format("saved alias {} -> {}\n", display_metadata(name),
                                   display_metadata(target.model_id));
    for (const std::string& old_alias : *replaced) {
        text += std::format("removed alias {} (a model keeps one alias)\n",
                            display_metadata(old_alias));
    }
    write_stdout(text);
    return code_of(Exit::ok);
}

int cmd_models_remove(const Config& config, const std::string& alias) {
    const std::string name = normalize_alias(alias);
    if (name.empty()) {
        return report(Error{.code = Exit::usage, .message = "the alias has no letters or digits"});
    }
    const auto removed = remove_alias(config.state_dir, name);
    if (!removed) {
        return report(removed.error());
    }
    const std::optional<AliasEntry>& entry = *removed;
    if (!entry.has_value()) {
        return report(Error{
            .code = Exit::model,
            .message =
                std::format("no saved alias is named \"{}\"; see `openrouter-cli list`", name),
        });
    }
    write_stdout(std::format("removed alias {} (was {})\n", display_metadata(name),
                             display_metadata(entry->model_id)));
    return code_of(Exit::ok);
}

int cmd_models_list(const Config& config) {
    const auto aliases = load_aliases(config.state_dir);
    if (!aliases) {
        return report(aliases.error());
    }
    const auto providers = load_providers(config.state_dir);
    if (!providers) {
        return report(providers.error());
    }
    const auto groups = load_groups(config.state_dir);
    if (!groups) {
        return report(groups.error());
    }
    std::string text;
    if (aliases->empty() && providers->empty()) {
        text = "no saved aliases or providers yet\n";
    }
    for (const auto& [alias, entry] : *aliases) {
        text += std::format("{} -> {} (saved {})\n", display_metadata(alias),
                            display_metadata(entry.model_id), display_metadata(entry.saved));
    }
    if (!providers->empty()) {
        text += aliases->empty() ? "Saved providers:\n" : "\nSaved providers:\n";
        for (const auto& [model_id, entry] : *providers) {
            text += std::format("{}: {} (saved {})\n", display_metadata(model_id),
                                display_metadata(join(entry.tags, ", ")),
                                display_metadata(entry.saved));
        }
    }
    if (!groups->empty()) {
        text += "\nGroups: see openrouter-cli group list\n";
    }
    write_stdout(text);
    return code_of(Exit::ok);
}

std::string tag_phrase(const std::vector<std::string>& tags) {
    return std::format("{} {}", tags.size() == 1 ? "provider" : "providers",
                       display_metadata(join(tags, ", ")));
}

// pin MODEL TAG...: saved for every later question to that model, replacing earlier tags.
int cmd_models_pin(const Config& config, const Http& http, const std::string& name,
                   const std::vector<std::string>& tags) {
    std::vector<std::string> unique;
    for (const std::string& tag : tags) {
        if (auto checked = check_provider_tag(tag); !checked) {
            return report(checked.error());
        }
        if (!std::ranges::contains(unique, tag)) {
            unique.push_back(tag);
        }
    }
    const auto catalog = catalog_for(config, http, false);
    if (!catalog) {
        return report(catalog.error());
    }
    const auto model = resolve_or_report(config, *catalog, name);
    if (!model) {
        return report(model.error());
    }
    const auto replaced = save_provider(config.state_dir, model->id, unique);
    if (!replaced) {
        return report(replaced.error());
    }
    std::string text =
        std::format("saved {} for {}\n", tag_phrase(unique), display_metadata(model->id));
    if (!replaced->empty()) {
        text += std::format("replaced {}\n", display_metadata(join(*replaced, ", ")));
    }
    write_stdout(text);
    return code_of(Exit::ok);
}

// unpin MODEL: an exact saved id is cleared without the catalog, so a retired model can be;
// anything else resolves the way pin does, so pin X and unpin X reach the same model.
int cmd_models_unpin(const Config& config, const Http& http, const std::string& name) {
    const auto saved = load_providers(config.state_dir);
    if (!saved) {
        return report(saved.error());
    }
    std::string model_id = name;
    if (!saved->contains(name)) {
        const auto catalog = catalog_for(config, http, false);
        if (!catalog) {
            return report(catalog.error());
        }
        const auto model = resolve_or_report(config, *catalog, name);
        if (!model) {
            return report(model.error());
        }
        model_id = model->id;
    }
    const auto removed = remove_provider(config.state_dir, model_id);
    if (!removed) {
        return report(removed.error());
    }
    if (!removed->has_value()) {
        return report(Error{
            .code = Exit::model,
            .message = std::format("no saved provider for {}; see `openrouter-cli list`",
                                   display_metadata(name)),
        });
    }
    write_stdout(std::format("removed saved {} for {}\n", tag_phrase((*removed)->tags),
                             display_metadata(model_id)));
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
    std::optional<std::string> tag; // from -m MODEL:TAG
    std::optional<Session> session;
    Message user_turn;
    BuiltRequest request;
    std::vector<std::string> providers; // sent as provider.only
    std::string provider_source;        // "from -m", "from -p", "saved", "any" or empty
};

// A -m value resolved to a catalog model, with the provider tag that followed its last colon.
struct Choice {
    ModelInfo model;
    std::optional<std::string> tag;
};

Result<Choice> resolve_choice(const Config& config, const Catalog& catalog,
                              const std::string& value) {
    auto split = split_model_choice(value, catalog.models);
    if (!split) {
        return std::unexpected(split.error());
    }
    auto model = resolve_or_report(config, catalog, split->model);
    if (!model) {
        return std::unexpected(model.error());
    }
    return Choice{.model = std::move(*model), .tag = std::move(split->provider)};
}

Result<std::string> system_text(const AskArgs& args, const Config& config) {
    const auto role = args.role == "review" ? Role::review : Role::plain;
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

Result<Choice> session_model(const Config& config, const Http& http, const AskArgs& args,
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
    std::optional<std::string> tag;
    if (!args.model.empty()) {
        auto requested = resolve_choice(config, *catalog, args.model);
        if (!requested) {
            return std::unexpected(requested.error());
        }
        if (requested->model.id != data.model_id) {
            return fail(Exit::usage, std::format("session {} keeps model {}; drop -m or use a "
                                                 "new session name",
                                                 args.session, data.model_id));
        }
        tag = std::move(requested->tag);
    }
    return Choice{.model = *found, .tag = std::move(tag)};
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
        auto choice = session_model(config, http, args, prepared.session->data());
        if (!choice) {
            return std::unexpected(choice.error());
        }
        prepared.model = std::move(choice->model);
        prepared.tag = std::move(choice->tag);
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
        auto choice = resolve_choice(config, *catalog, args.model);
        if (!choice) {
            return std::unexpected(choice.error());
        }
        prepared.model = std::move(choice->model);
        prepared.tag = std::move(choice->tag);
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
    if (args.query_is_text || args.query != "-") {
        return args.query;
    }
    auto text = read_input(remaining, false);
    if (!text) {
        return fail(text.error().code == Exit::usage ? Exit::refused : text.error().code,
                    text.error().message);
    }
    return std::move(*text);
}

// A group member refuses to send when -m no longer resolves to the model the group checked.
Result<void> check_expected_model(const Prepared& prepared, const AskArgs& args) {
    if (!args.expect_model_id.empty() && prepared.model.id != args.expect_model_id) {
        return fail(Exit::model, std::format("-m now resolves to {}, not {}; the catalog changed "
                                             "since the group checked its members, so nothing "
                                             "was sent",
                                             prepared.model.id, args.expect_model_id));
    }
    return {};
}

// Provider precedence: --any-provider, then an explicit -m tag or -p list, then the provider
// saved for this model. providers.json is read only in the last case, so a broken file never
// blocks a question that names its own provider.
Result<void> choose_providers(Prepared& prepared, const AskArgs& args, const Config& config) {
    if (args.any_provider) {
        if (!args.providers.empty() || prepared.tag.has_value()) {
            return fail(Exit::usage, "--any-provider ignores a saved provider; it cannot be "
                                     "combined with -p or -m MODEL:TAG");
        }
        prepared.provider_source = "any";
        return {};
    }
    if (prepared.tag.has_value()) {
        if (!args.providers.empty()) {
            return fail(Exit::usage, "-m MODEL:TAG and -p cannot be combined; repeat -p for "
                                     "several providers");
        }
        prepared.providers = {*prepared.tag};
        prepared.provider_source = "from -m";
        return {};
    }
    if (!args.providers.empty()) {
        prepared.providers = args.providers;
        prepared.provider_source = "from -p";
        return {};
    }
    const auto saved = load_providers(config.state_dir);
    if (!saved) {
        return std::unexpected(saved.error());
    }
    if (const auto found = saved->find(prepared.model.id); found != saved->end()) {
        prepared.providers = found->second.tags;
        prepared.provider_source = "saved";
    }
    return {};
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
    if (auto expected = check_expected_model(prepared, args); !expected) {
        return std::unexpected(expected.error());
    }
    if (auto chosen = choose_providers(prepared, args, config); !chosen) {
        return std::unexpected(chosen.error());
    }

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
        total = orcli::saturating_add(total, message.content.size());
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
    input.providers = prepared.providers;
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

// SGR faint: progress recedes on a color terminal so the answer stands out from it.
constexpr std::string_view sgr_progress = "2";

// The stderr side of one call while its answer streams to stdout. The methods are called from
// transfer callbacks, so none of them throws.
class StatusLines {
public:
    StatusLines(bool shared_destination, bool color) noexcept
        : spacing_(shared_destination), color_(color) {}

    // Call before any stderr line that is not written through progress().
    void separate() noexcept { write_stderr(spacing_.before_status()); }

    // One "openrouter-cli: <line>" progress line; false when it could not be written.
    bool progress(std::string_view line) noexcept {
        separate();
        try {
            return write_stderr(paint(sgr_progress,
                                      display_metadata(std::format("openrouter-cli: {}", line)),
                                      color_) +
                                "\n");
        } catch (...) {
            return false;
        }
    }

    void announce(const std::optional<std::string>& line) noexcept {
        if (line.has_value()) {
            progress(*line);
        }
    }

    // Call before non-empty answer text goes to stdout.
    void before_answer(std::string_view text) noexcept {
        write_stderr(spacing_.before_answer(text));
    }

    // Call once the answer's last line has been ended on stdout; what follows is status only.
    void answer_finished() noexcept {
        spacing_.line_completed();
        separate();
    }

private:
    AnswerSpacing spacing_;
    bool color_ = false;
};

// Notices that belong before a chunk's text reaches the terminal: the provider, when the
// stream names it ahead of the answer, and the start of the answer.
void announce_chunk(StatusLines& status, StreamProgress& progress, const StreamOutcome& outcome,
                    std::string_view text) {
    if (outcome.provider.has_value()) {
        status.announce(progress.on_provider(*outcome.provider));
    }
    if (!text.empty()) {
        status.announce(progress.on_answer_text(Clock::now()));
        status.before_answer(text);
    }
}

StreamRun run_stream(const Http& http, const Prepared& prepared, AnswerSink& sink,
                     const CostLog& cost_log, const std::string& call_id, StatusLines& status) {
    StreamRun run;
    SseParser parser;
    ChatStream chat;
    bool generation_logged = false;
    const auto started = Clock::now();
    // An effort of "none" asks the model not to reason, so there is no hidden text to mention.
    StreamProgress progress(started,
                            prepared.model.has_reasoning &&
                                prepared.request.effort_sent.value_or(std::string()) != "none");

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
            status.separate();
            diagnostic("openrouter-cli: cost log write failed: {}", logged.error().message);
        }
    };

    StreamHandlers handlers;
    handlers.on_generation_id = log_generation;
    handlers.on_body_bytes = [&](std::string_view bytes) {
        status.announce(progress.on_bytes(bytes.size(), Clock::now()));
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
            announce_chunk(status, progress, chat.outcome(), *text);
            sink.write(*text);
            if (chat.outcome().generation_id.has_value()) {
                log_generation(*chat.outcome().generation_id);
            }
        }
    };

    handlers.should_continue = [&] { return !run.parse_error.has_value(); };
    handlers.on_progress = [&] { status.announce(progress.on_tick(Clock::now())); };

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

std::string incomplete_reason(const StreamOutcome& outcome, long long max_tokens,
                              std::optional<long long> model_limit) {
    const auto& reason = outcome.finish_reason;
    if (reason == "length") {
        return length_stop_reason(max_tokens, model_limit, outcome.usage);
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

Verdict judge(const StreamRun& run, long long max_tokens, std::optional<long long> model_limit) {
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
            text += "; the stored API key was rejected: run `openrouter-cli login --replace`";
        } else if (run.transfer.status == http_not_found ||
                   run.transfer.status == http_unavailable) {
            text += "; check the provider tags with `openrouter-cli show MODEL` (a saved one is "
                    "cleared with `unpin MODEL` or skipped with --any-provider), and under "
                    "--privacy zdr try --privacy no-training";
        }
        return {.outcome = "error", .exit = Exit::api, .message = text};
    }
    if (!stream_complete(outcome)) {
        return {
            .outcome = "incomplete",
            .exit = Exit::incomplete,
            .message = incomplete_reason(outcome, max_tokens, model_limit),
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

// A resumed session keeps the system text it was created with, so its role is not known here.
std::string_view role_label(const AskArgs& args, const Prepared& prepared) {
    if (prepared.session.has_value() && prepared.session->exists()) {
        return "from session";
    }
    return args.role == "review" ? "review" : "plain";
}

// ", providers A,B (saved)" for the progress line; empty when OpenRouter chooses unprompted.
std::string provider_note(const Prepared& prepared) {
    if (prepared.provider_source == "any") {
        return ", providers any";
    }
    if (prepared.providers.empty()) {
        return {};
    }
    return std::format(", providers {} ({})", display_metadata(join(prepared.providers, ",")),
                       prepared.provider_source);
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
    StatusLines status(same_destination(STDOUT_FILENO, STDERR_FILENO), stream_color(STDERR_FILENO));
    status.progress(std::format("asking {} (role {}, effort {}, up to {} output tokens, {}{})",
                                display_metadata(prepared->model.id), role_label(args, *prepared),
                                display_metadata(prepared->request.effort_sent.value_or("default")),
                                prepared->request.max_tokens, pending.privacy,
                                provider_note(*prepared)));

    const StreamRun run = run_stream(http, *prepared, *sink, cost_log, call_id, status);
    sink->finish(answer_path);
    status.answer_finished();
    const Verdict verdict =
        judge(run, prepared->request.max_tokens, prepared->model.max_completion_tokens);
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

// ----- groups -----------------------------------------------------------------------------

std::string group_name_of(const std::string& name) {
    return normalize_alias(name);
}

Error no_group(std::string_view name) {
    return Error{
        .code = Exit::model,
        .message = std::format("no group is named \"{}\"; see `openrouter-cli group list`",
                               display_metadata(name)),
    };
}

// The concrete ids of the members that still resolve; a retired member has none, so it can
// neither duplicate another nor block an edit that removes it.
std::vector<std::pair<std::string, std::string>>
resolvable_ids(const Config& config, const Catalog& catalog,
               const std::vector<std::string>& members) {
    std::vector<std::pair<std::string, std::string>> ids;
    for (const std::string& member : members) {
        auto split = split_model_choice(member, catalog.models);
        if (!split) {
            continue;
        }
        auto aliases = load_aliases(config.state_dir);
        if (!aliases) {
            continue;
        }
        const Resolution resolution = resolve(split->model, catalog.models, *aliases);
        if (resolution.kind == Resolution::Kind::found && resolution.model.has_value()) {
            ids.emplace_back(member, resolution.model->id);
        }
    }
    return ids;
}

Result<void> refuse_duplicate_models(const std::vector<std::pair<std::string, std::string>>& ids) {
    for (std::size_t first = 0; first < ids.size(); ++first) {
        for (std::size_t second = first + 1; second < ids.size(); ++second) {
            if (ids.at(first).second == ids.at(second).second) {
                return fail(Exit::usage,
                            std::format("\"{}\" and \"{}\" are both {}; a group asks each model "
                                        "once",
                                        display_metadata(ids.at(first).first),
                                        display_metadata(ids.at(second).first),
                                        display_metadata(ids.at(first).second)));
            }
        }
    }
    return {};
}

std::string member_list_text(const Group& group) {
    return display_metadata(join(group.members, ", "));
}

// The NAME and comma-separated MODELS of a group command.
struct GroupEdit {
    std::string name;
    std::string list;
};

// group add NAME "MODEL[,MODEL...]": creates the group or appends; every new member must
// resolve now, and the whole result is checked under the lock before it is written.
int cmd_group_add(const Config& config, const Http& http, const GroupEdit& edit) {
    const std::string name = group_name_of(edit.name);
    if (name.empty()) {
        return report(Error{
            .code = Exit::usage,
            .message = "the group name has no letters or digits",
        });
    }
    const auto members = parse_model_list(edit.list);
    if (!members) {
        return report(members.error());
    }
    const auto catalog = catalog_for(config, http, false);
    if (!catalog) {
        return report(catalog.error());
    }
    for (const std::string& member : *members) {
        if (auto choice = resolve_choice(config, *catalog, member); !choice) {
            return report(choice.error());
        }
    }
    const auto written = update_groups(config.state_dir, [&](Groups& groups) -> Result<void> {
        Group& group = groups[name];
        for (const std::string& member : *members) {
            if (std::ranges::contains(group.members, member)) {
                return fail(Exit::usage, std::format("\"{}\" is already in group {}",
                                                     display_metadata(member), name));
            }
            group.members.push_back(member);
        }
        if (group.members.size() > max_group_members) {
            return fail(Exit::usage, std::format("a group holds at most {} models; group {} "
                                                 "would have {}",
                                                 max_group_members, name, group.members.size()));
        }
        if (auto unique = refuse_duplicate_models(resolvable_ids(config, *catalog, group.members));
            !unique) {
            return unique;
        }
        group.saved = utc_now_iso();
        return {};
    });
    if (!written) {
        return report(written.error());
    }
    write_stdout(std::format("group {}: {}\n", name, member_list_text(written->at(name))));
    return code_of(Exit::ok);
}

// group remove NAME [MODELS]: members match the stored text exactly, with no catalog lookup,
// so a retired member is removable by what `group list` prints.
int cmd_group_remove(const Config& config, const GroupEdit& edit) {
    const std::string name = group_name_of(edit.name);
    std::vector<std::string> members;
    if (!edit.list.empty()) {
        auto parsed = parse_model_list(edit.list);
        if (!parsed) {
            return report(parsed.error());
        }
        members = std::move(*parsed);
    }
    bool removed_group = false;
    const auto written = update_groups(config.state_dir, [&](Groups& groups) -> Result<void> {
        const auto found = groups.find(name);
        if (found == groups.end()) {
            return std::unexpected(no_group(edit.name));
        }
        if (members.empty()) {
            groups.erase(found);
            removed_group = true;
            return {};
        }
        std::vector<std::string>& current = found->second.members;
        for (const std::string& member : members) {
            const auto position = std::ranges::find(current, member);
            if (position == current.end()) {
                return fail(Exit::model,
                            std::format("\"{}\" is not in group {}; see `openrouter-cli group "
                                        "list`",
                                        display_metadata(member), name));
            }
            current.erase(position);
        }
        if (current.empty()) {
            groups.erase(found);
            removed_group = true;
        } else {
            found->second.saved = utc_now_iso();
        }
        return {};
    });
    if (!written) {
        return report(written.error());
    }
    write_stdout(removed_group
                     ? std::format("removed group {}\n", name)
                     : std::format("group {}: {}\n", name, member_list_text(written->at(name))));
    return code_of(Exit::ok);
}

int cmd_group_list(const Config& config) {
    const auto groups = load_groups(config.state_dir);
    if (!groups) {
        return report(groups.error());
    }
    if (groups->empty()) {
        write_stdout("no groups yet; create one with openrouter-cli group add NAME "
                     "\"MODEL,MODEL\"\n");
        return code_of(Exit::ok);
    }
    std::string text;
    for (const auto& [name, group] : *groups) {
        text += std::format("{}: {} (saved {})\n", display_metadata(name), member_list_text(group),
                            display_metadata(group.saved));
    }
    write_stdout(text);
    return code_of(Exit::ok);
}

// One member of the group being asked, resolved once in the parent.
struct GroupMember {
    std::string typed;    // as stored in the group
    std::string model_id; // the concrete id the child must use
    std::optional<std::string> tag;
    std::string session; // its own session under --session, or empty
};

std::string member_selector(const GroupMember& member) {
    return member.tag.has_value() ? member.model_id + ":" + *member.tag : member.model_id;
}

// Shared question options, passed on only when they were given: an omitted --role must not
// become an explicit one, which an existing session would refuse.
std::vector<std::string> child_argv(const AskArgs& args, const GroupMember& member) {
    std::vector<std::string> argv{
        "/proc/self/exe", "-m", member_selector(member), "--expect-model-id", member.model_id,
    };
    for (const std::string& file : args.files) {
        argv.insert(argv.end(), {"-f", file});
    }
    if (args.role_given) {
        argv.insert(argv.end(), {"--role", args.role});
    }
    if (!args.system.empty()) {
        argv.insert(argv.end(), {"-s", args.system});
    }
    if (!args.effort.empty()) {
        argv.insert(argv.end(), {"-e", args.effort});
    }
    if (args.max_output_tokens > 0) {
        argv.insert(argv.end(), {"--max-output-tokens", std::to_string(args.max_output_tokens)});
    }
    if (!args.privacy.empty()) {
        argv.insert(argv.end(), {"--privacy", args.privacy});
    }
    if (args.any_provider) {
        argv.emplace_back("--any-provider");
    }
    if (!member.session.empty()) {
        argv.insert(argv.end(), {"--session", member.session});
    }
    argv.insert(argv.end(), {"--", "-"});
    return argv;
}

// The group conversation's rules: once its marker exists, every member asked must already
// have history, so no member is ever asked a follow-up without the first round's
// instructions.
Result<void> check_group_session(const Config& config, const AskArgs& args,
                                 const std::vector<GroupMember>& members, bool& started) {
    auto marked = group_session_started(config.state_dir, args.session);
    if (!marked) {
        return std::unexpected(marked.error());
    }
    started = *marked;
    if (!started) {
        return {};
    }
    for (const GroupMember& member : members) {
        auto session = Session::open(config.state_dir, member.session);
        if (!session) {
            return std::unexpected(session.error());
        }
        if (!session->exists() || session->data().history.empty()) {
            return fail(Exit::usage, std::format("member {} has no history in session {}; start "
                                                 "a new session name, or remove the member",
                                                 display_metadata(member.typed), args.session));
        }
    }
    return {};
}

Result<void> refuse_tagged_any(const AskArgs& args, const std::vector<GroupMember>& members) {
    if (!args.any_provider) {
        return {};
    }
    std::vector<std::string> tagged;
    for (const GroupMember& member : members) {
        if (member.tag.has_value()) {
            tagged.push_back(member.typed);
        }
    }
    if (tagged.empty()) {
        return {};
    }
    return fail(Exit::usage, std::format("--any-provider cannot be used with members that name "
                                         "their own provider: {}",
                                         display_metadata(join(tagged, ", "))));
}

// Each member's own session under --session NAME; two that would share one are refused.
Result<void> assign_sessions(const AskArgs& args, std::vector<GroupMember>& members) {
    if (args.session.empty()) {
        return {};
    }
    for (GroupMember& member : members) {
        auto name = member_session_name(args.session, member.model_id);
        if (!name) {
            return std::unexpected(name.error());
        }
        member.session = std::move(*name);
    }
    for (std::size_t first = 0; first < members.size(); ++first) {
        for (std::size_t second = first + 1; second < members.size(); ++second) {
            if (members.at(first).session == members.at(second).session) {
                return fail(Exit::usage,
                            std::format("members {} and {} would share session {}; use a "
                                        "shorter session name",
                                        display_metadata(members.at(first).typed),
                                        display_metadata(members.at(second).typed),
                                        members.at(first).session));
            }
        }
    }
    return {};
}

Result<std::vector<GroupMember>> group_members(const Config& config, const Http& http,
                                               const AskArgs& args, const Group& group) {
    const auto catalog = catalog_for(config, http, false);
    if (!catalog) {
        return std::unexpected(catalog.error());
    }
    std::vector<GroupMember> members;
    std::vector<std::pair<std::string, std::string>> ids;
    for (const std::string& typed : group.members) {
        auto choice = resolve_choice(config, *catalog, typed);
        if (!choice) {
            return fail(choice.error().code,
                        std::format("group member {}: {}", display_metadata(typed),
                                    choice.error().message));
        }
        ids.emplace_back(typed, choice->model.id);
        members.push_back(GroupMember{
            .typed = typed,
            .model_id = choice->model.id,
            .tag = std::move(choice->tag),
            .session = {},
        });
    }
    if (auto unique = refuse_duplicate_models(ids); !unique) {
        return std::unexpected(unique.error());
    }
    if (auto tagged = refuse_tagged_any(args, members); !tagged) {
        return std::unexpected(tagged.error());
    }
    if (auto sessions = assign_sessions(args, members); !sessions) {
        return std::unexpected(sessions.error());
    }
    return members;
}

std::string outcome_text(const MemberOutcome& outcome, const ChildResult& result) {
    switch (outcome.state) {
    case MemberState::complete:
        return "complete";
    case MemberState::incomplete:
        return "incomplete (exit 6, partial answer kept)";
    case MemberState::interrupted:
        return "interrupted (exit 8)";
    case MemberState::crashed:
        return result.term_signal != 0 ? std::format("crashed (signal {})", result.term_signal)
                                       : std::format("crashed (exit {})", result.exit_code);
    case MemberState::never_started:
        return std::format("never started ({})", result.spawn_error.message.empty()
                                                     ? "an earlier member could not be started"
                                                     : result.spawn_error.message);
    case MemberState::failed:
        return std::format("failed (exit {})", outcome.code);
    }
    return "unknown";
}

// Runs prepare() for every member as its child will, with the buffered query, so effort,
// input, file, session and saved-provider problems stop the group before anything is sent.
// prepare() only reads, checks and builds a request: it sends and commits nothing.
Result<void> dry_run_members(const AskArgs& args, const Config& config, const Http& http,
                             const std::vector<GroupMember>& members, const std::string& query) {
    for (const GroupMember& member : members) {
        AskArgs dry = args;
        dry.group.clear();
        dry.model = member_selector(member);
        dry.expect_model_id = member.model_id;
        dry.session = member.session;
        dry.query = query;
        dry.query_is_text = true;
        if (auto prepared = prepare(dry, config, http); !prepared) {
            return fail(prepared.error().code,
                        std::format("group member {}: {}", display_metadata(member.typed),
                                    prepared.error().message));
        }
    }
    return {};
}

// Holds the group conversation's lock and applies its rules. The marker is written before the
// first member is asked, so a round that fails part-way still makes the next one insist on
// history for every member.
Result<std::optional<FileLock>> open_group_session(const Config& config, const AskArgs& args,
                                                   const std::vector<GroupMember>& members) {
    if (args.session.empty()) {
        return std::optional<FileLock>{};
    }
    auto lock = lock_state_file(config.state_dir, "group-session-" + args.session + ".lock", false);
    if (!lock) {
        return std::unexpected(lock.error());
    }
    bool started = false;
    if (auto checked = check_group_session(config, args, members, started); !checked) {
        return std::unexpected(checked.error());
    }
    return std::optional<FileLock>{std::move(*lock)};
}

Result<void> mark_new_group_session(const Config& config, const AskArgs& args,
                                    const std::vector<GroupMember>& members,
                                    std::string_view group) {
    if (args.session.empty()) {
        return {};
    }
    auto started = group_session_started(config.state_dir, args.session);
    if (!started) {
        return std::unexpected(started.error());
    }
    if (*started) {
        return {};
    }
    std::vector<std::string> ids;
    ids.reserve(members.size());
    for (const GroupMember& member : members) {
        ids.push_back(member.model_id);
    }
    return mark_group_session(config.state_dir, args.session, ids, group);
}

void print_member_answer(const GroupMember& member, const ChildResult& result) {
    std::string block = std::format("===== {} ({}) =====\n{}", display_metadata(member.typed),
                                    display_metadata(member.model_id), result.out);
    if (!block.ends_with('\n')) {
        block += '\n';
    }
    write_stdout(block + "\n");
}

MemberOutcome outcome_of(const ChildResult& result) {
    if (!result.started) {
        // Every start failure is a local configuration problem (Exit::config).
        return {.state = MemberState::never_started, .code = code_of(Exit::config)};
    }
    if (result.term_signal != 0) {
        return {.state = MemberState::crashed, .code = result.term_signal};
    }
    return outcome_of_exit(result.exit_code);
}

// One line per member, then the totals; the exit code follows group_exit_code.
int summarize_group(std::string_view name, const std::vector<GroupMember>& members,
                    const std::vector<ChildResult>& results, bool cancelled) {
    std::vector<MemberOutcome> outcomes;
    std::size_t complete = 0;
    std::size_t incomplete = 0;
    for (std::size_t index = 0; index < results.size(); ++index) {
        const MemberOutcome outcome = outcome_of(results.at(index));
        complete += outcome.state == MemberState::complete ? 1 : 0;
        incomplete += outcome.state == MemberState::incomplete ? 1 : 0;
        diagnostic("openrouter-cli: group {}: {} ({}): {}", name,
                   display_metadata(members.at(index).typed),
                   display_metadata(member_selector(members.at(index))),
                   outcome_text(outcome, results.at(index)));
        outcomes.push_back(outcome);
    }
    diagnostic("openrouter-cli: group {}: {} complete, {} incomplete, {} failed", name, complete,
               incomplete, results.size() - complete - incomplete);
    return group_exit_code(outcomes, cancelled);
}

// -g NAME: every member is checked in this process first (resolution, duplicates, sessions and
// a dry run), and only then asked, each by its own child process running the ordinary
// single-model path.
int cmd_group_ask(const AskArgs& args, const Config& config, const Http& http) {
    if (!args.model.empty() || !args.providers.empty()) {
        return report(Error{
            .code = Exit::usage,
            .message = "-g cannot be combined with -m or -p; members carry their own provider "
                       "as MODEL:TAG or a saved pin",
        });
    }
    const auto groups = load_groups(config.state_dir);
    if (!groups) {
        return report(groups.error());
    }
    const std::string name = group_name_of(args.group);
    const auto found = groups->find(name);
    if (found == groups->end()) {
        return report(no_group(args.group));
    }
    const auto query = query_text(args, config.max_input_bytes);
    if (!query) {
        return report(query.error());
    }
    const auto members = group_members(config, http, args, found->second);
    if (!members) {
        return report(members.error());
    }
    const auto conversation_lock = open_group_session(config, args, *members);
    if (!conversation_lock) {
        return report(conversation_lock.error());
    }
    if (auto checked = dry_run_members(args, config, http, *members, *query); !checked) {
        return report(checked.error());
    }
    if (auto marked = mark_new_group_session(config, args, *members, name); !marked) {
        return report(marked.error());
    }

    std::vector<ChildSpec> children;
    for (const GroupMember& member : *members) {
        children.push_back(ChildSpec{.argv = child_argv(args, member)});
    }
    diagnostic("openrouter-cli: group {}: asking {} members (one billed call each)", name,
               members->size());
    const ChildEvents events{
        .stderr_line =
            [&members](std::size_t index, std::string_view line) {
                write_stderr(
                    std::format("[{}] {}\n", display_metadata(members->at(index).typed), line));
            },
        .finished =
            [&members](std::size_t index, const ChildResult& result) {
                print_member_answer(members->at(index), result);
            },
    };
    bool cancelled = false;
    const std::vector<ChildResult> results =
        run_children(children, SuperviseOptions{.input = *query}, events, cancelled);
    return summarize_group(name, *members, results, cancelled);
}

// ----- command line -----------------------------------------------------------------------

// The model commands exist under two parents; both registrations fill the same Cli fields.
struct ModelCommands {
    CLI::App* find = nullptr;
    CLI::App* show = nullptr;
    CLI::App* save = nullptr;
    CLI::App* remove = nullptr;
    CLI::App* list = nullptr;
    CLI::App* pin = nullptr;
    CLI::App* unpin = nullptr;

    [[nodiscard]] static bool used(const CLI::App* command) {
        return command != nullptr && command->parsed();
    }
};

// Every parsed option and subcommand of the command line.
struct Cli {
    AskArgs ask;
    std::string find_name;
    bool refresh = false;
    std::string show_name;
    std::string save_alias_name;
    std::string save_model_id;
    std::string remove_alias_name;
    std::string pin_name;
    std::vector<std::string> pin_tags;
    std::string unpin_name;
    std::string group_add_name;
    std::string group_add_list;
    std::string group_remove_name;
    std::string group_remove_list;
    CLI::App* group_add = nullptr;
    CLI::App* group_remove = nullptr;
    CLI::App* group_list = nullptr;
    std::string since = "all";
    bool resolve = false;
    bool login_replace = false;
    bool set_management = false;
    bool set_replace = false;
    bool logout_management = false;
    int activity_days = default_activity_days;

    ModelCommands short_models;  // openrouter-cli save ...
    ModelCommands nested_models; // openrouter-cli models save ..., the earlier spelling
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
    app.add_option("-m,--model", ask.model,
                   "Model alias, id, ~author/family-latest, or name; :TAG uses only that provider "
                   "for this question")
        ->option_text("MODEL[:TAG]");
    app.add_option("-g,--group", ask.group,
                   "Ask every model in a saved group at once; one billed call per member")
        ->option_text("NAME");
    // One value per occurrence: CLI11 lets a vector option take every following argument by
    // default, which would swallow the query (CLI11 book, "Options": allow_extra_args).
    app.add_option("-f,--file", ask.files, "File to include, line-numbered (repeatable)")
        ->allow_extra_args(false);
    app.add_option("--role", ask.role,
                   "plain (default) asks as written; review adds reviewer instructions")
        ->check(CLI::IsMember({"review", "plain"}));
    app.add_option("-s,--system-prompt", ask.system, "Extra system prompt text, or @FILE")
        ->option_text("TEXT");
    app.add_option("-e,--effort", ask.effort,
                   "Reasoning effort: max, xhigh, high, medium, low, minimal, none")
        ->option_text("LEVEL");
    app.add_option("--max-output-tokens", ask.max_output_tokens,
                   std::format("Output budget including reasoning (built-in default {}; `show "
                               "MODEL` prints the configured budget)",
                               default_max_output_tokens))
        ->check(CLI::PositiveNumber)
        ->option_text("N");
    app.add_option("-p,--provider", ask.providers,
                   "Use only this provider (endpoint tag from `show`; repeatable); replaces a "
                   "saved provider for this question")
        ->allow_extra_args(false)
        ->option_text("TAG");
    app.add_flag("--any-provider", ask.any_provider,
                 "Ignore the saved provider for this question; privacy settings still apply");
    // A group parent passes this to each member; an empty group keeps it out of help.
    app.add_option("--expect-model-id", ask.expect_model_id, "")->group("");
    app.add_option("--privacy", ask.privacy, "zdr (default) or no-training")
        ->check(CLI::IsMember({"zdr", "no-training"}))
        ->option_text("MODE");
    app.add_option("--session", ask.session, "Keep history under this name for follow-ups")
        ->option_text("NAME");
    app.add_option("query", ask.query, "The question; - reads it from stdin");
}

ModelCommands add_model_leaves(CLI::App& parent, Cli& cli) {
    ModelCommands commands;
    commands.find = parent.add_subcommand("find", "List models matching words");
    commands.find->add_flag("--refresh", cli.refresh, "Refetch the catalog first");
    commands.find->add_option("name", cli.find_name, "Words to match")->required();
    commands.show = parent.add_subcommand("show", "Prices, limits, efforts and provider tags");
    commands.show->add_option("model", cli.show_name, "Alias, id or name")->required();
    commands.save = parent.add_subcommand("save", "Save an alias for a model id");
    commands.save->add_option("alias", cli.save_alias_name, "Alias to save")->required();
    commands.save->add_option("model_id", cli.save_model_id, "Exact model id")->required();
    commands.remove = parent.add_subcommand("remove", "Delete a saved alias");
    commands.remove->add_option("alias", cli.remove_alias_name, "Alias to delete")->required();
    commands.list = parent.add_subcommand("list", "List saved aliases and providers");
    commands.pin = parent.add_subcommand(
        "pin", "Save a provider for a model: every later question to it uses only these tags "
               "(replaces earlier tags)");
    commands.pin->add_option("model", cli.pin_name, "Alias, id, or a name in quotes")->required();
    commands.pin->add_option("tags", cli.pin_tags, "Provider tags from `show MODEL` (one or more)")
        ->required()
        ->option_text("TAG...");
    commands.unpin = parent.add_subcommand(
        "unpin", "Clear a model's saved provider (exit 4 when none is saved)");
    commands.unpin->add_option("model", cli.unpin_name, "Alias, id, name, or an id from `list`")
        ->required();
    return commands;
}

void add_group_commands(CLI::App& app, Cli& cli) {
    auto* group = app.add_subcommand(
        "group", "Save groups of models and ask them all with -g NAME (one billed call each)");
    group->require_subcommand(1);
    group->fallthrough(false);
    cli.group_add = group->add_subcommand(
        "add", "Create a group or add models to it: one argument, models separated by commas "
               "(quote it when it has spaces); MODEL:TAG pins that member's provider; at most 8");
    cli.group_add->add_option("name", cli.group_add_name, "Group name")->required();
    cli.group_add
        ->add_option("models", cli.group_add_list, "AUTHOR/A,AUTHOR/B (aliases and names work)")
        ->required()
        ->option_text("MODELS");
    cli.group_remove = group->add_subcommand(
        "remove", "Remove the listed members (as `group list` prints them), or the whole group "
                  "when none are given");
    cli.group_remove->add_option("name", cli.group_remove_name, "Group name")->required();
    cli.group_remove->add_option("models", cli.group_remove_list, "Members separated by commas")
        ->option_text("[MODELS]");
    cli.group_list = group->add_subcommand("list", "Every group and its members (offline)");
}

void add_models_commands(CLI::App& app, Cli& cli) {
    cli.short_models = add_model_leaves(app, cli);
    auto* models = app.add_subcommand("models", "Find, inspect and save model ids");
    models->require_subcommand(1);
    models->fallthrough(false);
    cli.nested_models = add_model_leaves(*models, cli);
    // CLI11 copies a parent's group into each child when the child is created, so the group
    // is emptied only now: `models` leaves the top-level page while `models --help` still
    // lists its commands.
    models->group("");
}

void add_account_commands(CLI::App& app, Cli& cli) {
    cli.spend = app.add_subcommand("spend", "Totals from the global cost log");
    cli.spend->add_option("--since", cli.since, "today, 7d, 30d or all")
        ->check(CLI::IsMember({"today", "7d", "30d", "all"}))
        ->option_text("PERIOD");
    cli.spend->add_flag("--resolve", cli.resolve,
                        "Look up the charge of earlier calls whose cost is still unknown (free)");

    cli.login = app.add_subcommand(
        "login", "Sign in with your OpenRouter account in a browser; stores a new API key");
    cli.login->add_flag("--replace", cli.login_replace, "Replace the stored API key");
    cli.logout = app.add_subcommand("logout", "Delete the stored keys from this PC");
    cli.logout->add_flag("--management", cli.logout_management, "Delete only the Management key");

    auto* auth = app.add_subcommand("auth", "Store an API or Management key you already have");
    auth->require_subcommand(1);
    auth->fallthrough(false);
    cli.set_key = auth->add_subcommand(
        "set", "Store an existing key, read from a hidden prompt or from stdin");
    cli.set_key->add_flag("--management", cli.set_management, "The key is a Management key");
    cli.set_key->add_flag("--replace", cli.set_replace, "Replace a stored key of the same kind");

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
        return cmd_login(config, cli.login_replace);
    }
    if (cli.set_key->parsed()) {
        return cmd_auth_set(config, cli.set_management, cli.set_replace);
    }
    if (cli.logout->parsed()) {
        return cmd_logout(config, cli.logout_management);
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
    if (ModelCommands::used(cli.short_models.find) || ModelCommands::used(cli.nested_models.find)) {
        return cmd_models_find(config, public_http, cli.find_name, cli.refresh);
    }
    if (ModelCommands::used(cli.short_models.show) || ModelCommands::used(cli.nested_models.show)) {
        return cmd_models_show(config, public_http, cli.show_name);
    }
    if (ModelCommands::used(cli.short_models.save) || ModelCommands::used(cli.nested_models.save)) {
        return cmd_models_save(config, public_http,
                               {.alias = cli.save_alias_name, .model_id = cli.save_model_id});
    }
    if (ModelCommands::used(cli.short_models.remove) ||
        ModelCommands::used(cli.nested_models.remove)) {
        return cmd_models_remove(config, cli.remove_alias_name);
    }
    if (ModelCommands::used(cli.short_models.list) || ModelCommands::used(cli.nested_models.list)) {
        return cmd_models_list(config);
    }
    if (ModelCommands::used(cli.short_models.pin) || ModelCommands::used(cli.nested_models.pin)) {
        return cmd_models_pin(config, public_http, cli.pin_name, cli.pin_tags);
    }
    if (ModelCommands::used(cli.short_models.unpin) ||
        ModelCommands::used(cli.nested_models.unpin)) {
        return cmd_models_unpin(config, public_http, cli.unpin_name);
    }
    if (cli.group_add->parsed()) {
        return cmd_group_add(config, public_http,
                             GroupEdit{.name = cli.group_add_name, .list = cli.group_add_list});
    }
    if (cli.group_remove->parsed()) {
        return cmd_group_remove(
            config, GroupEdit{.name = cli.group_remove_name, .list = cli.group_remove_list});
    }
    if (cli.group_list->parsed()) {
        return cmd_group_list(config);
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
    if (!cli.ask.group.empty()) {
        return cmd_group_ask(cli.ask, config, http);
    }
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
            "--any-provider",
            "--group",
            "--expect-model-id",
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

// Examples use a placeholder alias and model id so the binary names no vendor. A command
// longer than 56 characters is split with a backslash, because example lines are never
// wrapped and a 60-column page must still fit them. A session
// follow-up drops -m because a session refuses any model but the one it was created with.
HelpContent root_help() {
    return HelpContent{
        .title = std::string(version_text),
        .examples =
            {
                HelpExample{
                    .caption = "Save a short alias for a model id",
                    .commands = {"openrouter-cli save reviewer AUTHOR/MODEL"},
                },
                HelpExample{
                    .caption = "Get a structured review of a plan file",
                    .commands =
                        {
                            R"(openrouter-cli -m reviewer --role review \)",
                            R"(  -f plan.md "Review this plan")",
                        },
                },
                HelpExample{
                    .caption = "Send a diff as the question, read from stdin",
                    .commands = {"git diff | openrouter-cli -m reviewer --role review -"},
                },
                HelpExample{
                    .caption =
                        "Keep a named conversation, then follow up (the session keeps its model)",
                    .commands =
                        {
                            R"(openrouter-cli --session plan -m reviewer \)",
                            R"(  --role review "Review it")",
                            R"(openrouter-cli --session plan "Is step 3 sound now?")",
                        },
                },
                HelpExample{
                    .caption = "Use only one provider (a TAG from `show MODEL`)",
                    .commands = {R"(openrouter-cli -m AUTHOR/MODEL:TAG "Question")"},
                },
                HelpExample{
                    .caption = "Save a provider for a model; later questions to it use it",
                    .commands = {"openrouter-cli pin AUTHOR/MODEL TAG"},
                },
                HelpExample{
                    .caption = "See saved providers, skip one for a question, or clear it",
                    .commands =
                        {
                            "openrouter-cli list",
                            R"(openrouter-cli -m AUTHOR/MODEL --any-provider "Q")",
                            "openrouter-cli unpin AUTHOR/MODEL",
                        },
                },
                HelpExample{
                    .caption = "Save a group of models, then ask them all at once",
                    .commands =
                        {
                            R"(openrouter-cli group add NAME "AUTHOR/A,AUTHOR/B")",
                            R"(openrouter-cli -g NAME --role review "Question")",
                        },
                },
            },
        .notes =
            {
                "Options come first and the query comes last. Put -- before a query that starts "
                "with - or matches a command name. Question options cannot be combined with a "
                "command.",
                "Providers: in -m MODEL:TAG the tag is the text after the last colon. "
                "AUTHOR/MODEL:free works only as an exact catalog id; the variants :nitro, "
                ":floor, :exacto, :online, :thinking, :extended and :batch are refused; any other "
                "word after the last colon is read as a provider tag. A provider saved with pin "
                "applies to every question to that model, session rounds included. -m MODEL:TAG "
                "or -p replaces it for one question, --any-provider ignores it, and :TAG and -p "
                "cannot be combined.",
                "Groups: -g NAME asks every member at once, one billed call each, and prints each "
                "answer under a ===== MEMBER ===== line as it finishes. With --session, each "
                "member keeps its own history; a follow-up repeats -g and --session and drops "
                "--role and -s.",
            },
    };
}

int run(std::span<char* const> argv) {
    CLI::App app{"Ask any OpenRouter model a question, or have it review a plan or code.",
                 "openrouter-cli"};
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
    add_group_commands(app, cli);
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
