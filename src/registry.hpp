#ifndef ORCLI_REGISTRY_HPP
#define ORCLI_REGISTRY_HPP

#include "common.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orcli {

// ----- saved providers (providers.json) ---------------------------------------------------

// The tags `pin` saved for one concrete model id, sent as provider.only when a question names
// no provider of its own.
struct SavedProvider {
    std::vector<std::string> tags;
    std::string saved; // ISO 8601 UTC
};

// Empty when providers.json does not exist; Exit::config when it is malformed in any way, so
// a broken entry never silently drops a saved restriction.
Result<std::map<std::string, SavedProvider>> load_providers(const std::filesystem::path& state_dir);

// Replaces the tags saved for model_id and returns the ones it replaced.
Result<std::vector<std::string>> save_provider(const std::filesystem::path& state_dir,
                                               const std::string& model_id,
                                               const std::vector<std::string>& tags);

// Removes the entry for model_id and returns it, or nothing when none was saved.
Result<std::optional<SavedProvider>> remove_provider(const std::filesystem::path& state_dir,
                                                     const std::string& model_id);

// ----- groups (groups.json) ---------------------------------------------------------------

inline constexpr std::size_t max_group_members = 8;

struct Group {
    std::vector<std::string> members; // as typed, trimmed; 1 to max_group_members
    std::string saved;                // ISO 8601 UTC
};

using Groups = std::map<std::string, Group>;

Result<Groups> load_groups(const std::filesystem::path& state_dir);

// Runs edit on the current groups under locks/groups.lock and writes the result atomically
// when edit succeeds; an error from edit leaves the file unchanged. Returns the written state.
Result<Groups> update_groups(const std::filesystem::path& state_dir,
                             const std::function<Result<void>(Groups&)>& edit);

// ----- group sessions (group-sessions/NAME.json) ------------------------------------------

// The marker that a group conversation named `name` has started. It is written before any
// member is asked and never changed, so a later round can insist that every member it asks
// already has history.
Result<bool> group_session_started(const std::filesystem::path& state_dir, std::string_view name);
Result<void> mark_group_session(const std::filesystem::path& state_dir, std::string_view name,
                                const std::vector<std::string>& model_ids, std::string_view group);

} // namespace orcli

#endif // ORCLI_REGISTRY_HPP
