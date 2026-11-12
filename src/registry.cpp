#include "registry.hpp"

#include "common.hpp"
#include "costlog.hpp"
#include "fsutil.hpp"
#include "session.hpp"

#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orcli {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr std::string_view providers_file_name = "providers.json";
constexpr std::string_view providers_lock_name = "providers.lock";
constexpr std::string_view groups_file_name = "groups.json";
constexpr std::string_view groups_lock_name = "groups.lock";
constexpr std::string_view group_sessions_dir_name = "group-sessions";
constexpr int json_indent = 2;

std::string encode(const json& document) {
    // Replacing invalid UTF-8 keeps dump() from throwing on malformed command-line text.
    return document.dump(json_indent, ' ', false, json::error_handler_t::replace) + "\n";
}

// The text of a registry file, or nothing when it does not exist.
Result<std::optional<std::string>> read_registry(const fs::path& file) {
    const auto present = file_present(file);
    if (!present) {
        return std::unexpected(present.error());
    }
    if (!*present) {
        return std::optional<std::string>{};
    }
    auto text = read_private_file(file);
    if (!text) {
        return std::unexpected(text.error());
    }
    return std::optional<std::string>{std::move(*text)};
}

// A non-empty array of non-empty strings, or nothing.
std::optional<std::vector<std::string>> string_list(const json& value) {
    if (!value.is_array() || value.empty()) {
        return std::nullopt;
    }
    std::vector<std::string> items;
    for (const json& item : value) {
        if (!item.is_string() || item.get_ref<const std::string&>().empty()) {
            return std::nullopt;
        }
        items.push_back(item.get<std::string>());
    }
    return items;
}

std::optional<std::string> saved_time(const json& entry) {
    const auto found = entry.find("saved");
    if (found == entry.end() || !found->is_string()) {
        return std::nullopt;
    }
    return found->get<std::string>();
}

// ----- providers ----------------------------------------------------------------------------

Error corrupt_providers(const fs::path& file, std::string_view model_id) {
    const std::string which =
        model_id.empty() ? std::string() : std::format(" (model {})", model_id);
    return Error{
        .code = Exit::config,
        .message = std::format("saved provider file {} is corrupt{}; fix or remove that entry, "
                               "or pass --any-provider for this question",
                               file.string(), which),
    };
}

Result<std::map<std::string, SavedProvider>> decode_providers(std::string_view text,
                                                              const fs::path& file) {
    const json document = json::parse(text, nullptr, false);
    if (!document.is_object() || !document.contains("providers") ||
        !document.at("providers").is_object()) {
        return std::unexpected(corrupt_providers(file, ""));
    }
    std::map<std::string, SavedProvider> providers;
    for (const auto& [model_id, entry] : document.at("providers").items()) {
        if (model_id.empty() || !entry.is_object() || !entry.contains("tags")) {
            return std::unexpected(corrupt_providers(file, model_id));
        }
        auto tags = string_list(entry.at("tags"));
        auto saved = saved_time(entry);
        if (!tags.has_value() || !saved.has_value()) {
            return std::unexpected(corrupt_providers(file, model_id));
        }
        providers.emplace(model_id,
                          SavedProvider{.tags = std::move(*tags), .saved = std::move(*saved)});
    }
    return providers;
}

std::string encode_providers(const std::map<std::string, SavedProvider>& providers) {
    json entries = json::object();
    for (const auto& [model_id, entry] : providers) {
        entries.emplace(model_id, json{{"tags", entry.tags}, {"saved", entry.saved}});
    }
    return encode(json{{"providers", std::move(entries)}});
}

// ----- groups -------------------------------------------------------------------------------

Error corrupt_groups(const fs::path& file, std::string_view name) {
    const std::string which = name.empty() ? std::string() : std::format(" (group {})", name);
    return Error{
        .code = Exit::config,
        .message = std::format("group file {} is corrupt{}; fix or remove that entry",
                               file.string(), which),
    };
}

Result<Groups> decode_groups(std::string_view text, const fs::path& file) {
    const json document = json::parse(text, nullptr, false);
    if (!document.is_object() || !document.contains("groups") ||
        !document.at("groups").is_object()) {
        return std::unexpected(corrupt_groups(file, ""));
    }
    Groups groups;
    for (const auto& [name, entry] : document.at("groups").items()) {
        if (name.empty() || !entry.is_object() || !entry.contains("members")) {
            return std::unexpected(corrupt_groups(file, name));
        }
        auto members = string_list(entry.at("members"));
        auto saved = saved_time(entry);
        if (!members.has_value() || members->size() > max_group_members || !saved.has_value()) {
            return std::unexpected(corrupt_groups(file, name));
        }
        groups.emplace(name, Group{.members = std::move(*members), .saved = std::move(*saved)});
    }
    return groups;
}

std::string encode_groups(const Groups& groups) {
    json entries = json::object();
    for (const auto& [name, group] : groups) {
        entries.emplace(name, json{{"members", group.members}, {"saved", group.saved}});
    }
    return encode(json{{"groups", std::move(entries)}});
}

} // namespace

Result<std::map<std::string, SavedProvider>> load_providers(const fs::path& state_dir) {
    const fs::path file = state_dir / providers_file_name;
    const auto text = read_registry(file);
    if (!text) {
        return std::unexpected(text.error());
    }
    if (!text->has_value()) {
        return std::map<std::string, SavedProvider>{};
    }
    return decode_providers(**text, file);
}

Result<std::vector<std::string>> save_provider(const fs::path& state_dir,
                                               const std::string& model_id,
                                               const std::vector<std::string>& tags) {
    const auto lock = lock_state_file(state_dir, providers_lock_name);
    if (!lock) {
        return std::unexpected(lock.error());
    }
    auto providers = load_providers(state_dir);
    if (!providers) {
        return std::unexpected(providers.error());
    }
    std::vector<std::string> replaced;
    if (const auto found = providers->find(model_id); found != providers->end()) {
        replaced = found->second.tags;
    }
    providers->insert_or_assign(model_id, SavedProvider{.tags = tags, .saved = utc_now_iso()});
    if (const auto written =
            atomic_write(state_dir / providers_file_name, encode_providers(*providers));
        !written) {
        return std::unexpected(written.error());
    }
    return replaced;
}

Result<std::optional<SavedProvider>> remove_provider(const fs::path& state_dir,
                                                     const std::string& model_id) {
    const auto lock = lock_state_file(state_dir, providers_lock_name);
    if (!lock) {
        return std::unexpected(lock.error());
    }
    auto providers = load_providers(state_dir);
    if (!providers) {
        return std::unexpected(providers.error());
    }
    const auto found = providers->find(model_id);
    if (found == providers->end()) {
        return std::optional<SavedProvider>{};
    }
    std::optional<SavedProvider> removed = std::move(found->second);
    providers->erase(found);
    if (const auto written =
            atomic_write(state_dir / providers_file_name, encode_providers(*providers));
        !written) {
        return std::unexpected(written.error());
    }
    return removed;
}

Result<Groups> load_groups(const fs::path& state_dir) {
    const fs::path file = state_dir / groups_file_name;
    const auto text = read_registry(file);
    if (!text) {
        return std::unexpected(text.error());
    }
    if (!text->has_value()) {
        return Groups{};
    }
    return decode_groups(**text, file);
}

Result<Groups> update_groups(const fs::path& state_dir,
                             const std::function<Result<void>(Groups&)>& edit) {
    const auto lock = lock_state_file(state_dir, groups_lock_name);
    if (!lock) {
        return std::unexpected(lock.error());
    }
    auto groups = load_groups(state_dir);
    if (!groups) {
        return std::unexpected(groups.error());
    }
    if (auto edited = edit(*groups); !edited) {
        return std::unexpected(edited.error());
    }
    if (const auto written = atomic_write(state_dir / groups_file_name, encode_groups(*groups));
        !written) {
        return std::unexpected(written.error());
    }
    return std::move(*groups);
}

Result<bool> group_session_started(const fs::path& state_dir, std::string_view name) {
    if (!valid_session_name(name)) {
        return fail(Exit::usage, "invalid session name; use 1 to 64 letters, digits, '.', '_' or "
                                 "'-'");
    }
    return file_present(state_dir / group_sessions_dir_name / (std::string(name) + ".json"));
}

Result<void> mark_group_session(const fs::path& state_dir, std::string_view name,
                                const std::vector<std::string>& model_ids, std::string_view group) {
    if (!valid_session_name(name)) {
        return fail(Exit::usage, "invalid session name; use 1 to 64 letters, digits, '.', '_' or "
                                 "'-'");
    }
    const fs::path dir = state_dir / group_sessions_dir_name;
    if (const auto made = ensure_private_dir(dir); !made) {
        return std::unexpected(made.error());
    }
    const json marker{{"group", group}, {"models", model_ids}, {"started", utc_now_iso()}};
    return atomic_write(dir / (std::string(name) + ".json"), encode(marker));
}

} // namespace orcli
