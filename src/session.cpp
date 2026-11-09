#include "session.hpp"

#include "common.hpp"
#include "fsutil.hpp"

#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace orcli {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t max_session_name_length = 64;
constexpr std::size_t uuid_bytes = 16;
constexpr long long session_format_version = 1;
constexpr int json_indent = 2;

bool session_name_char(char letter) {
    return (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z') ||
           (letter >= '0' && letter <= '9') || letter == '.' || letter == '_' || letter == '-';
}

std::optional<std::string> string_at(const nlohmann::json& object, std::string_view key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) {
        return std::nullopt;
    }
    return found->get<std::string>();
}

// The file holds prompt text and model output, so errors name the path but never echo content.
std::unexpected<Error> corrupt(const fs::path& file) {
    return fail(Exit::config,
                std::format("session file {} is corrupt; move it aside to start the session again",
                            file.string()));
}

std::optional<std::vector<Message>> parse_history(const nlohmann::json& root) {
    const auto found = root.find("history");
    if (found == root.end() || !found->is_array()) {
        return std::nullopt;
    }
    std::vector<Message> history;
    history.reserve(found->size());
    for (const nlohmann::json& entry : *found) {
        auto role = string_at(entry, "role");
        auto content = string_at(entry, "content");
        if (!role || !content) {
            return std::nullopt;
        }
        history.push_back(Message{.role = std::move(*role), .content = std::move(*content)});
    }
    return history;
}

Result<SessionData> parse_session(std::string_view text, const fs::path& file) {
    const auto root = nlohmann::json::parse(text, nullptr, false);
    if (!root.is_object()) {
        return corrupt(file);
    }
    const auto version = root.find("v");
    if (version == root.end() || !version->is_number_integer() ||
        version->get<long long>() != session_format_version) {
        return corrupt(file);
    }
    auto model_id = string_at(root, "model_id");
    auto system_prompt = string_at(root, "system_prompt");
    auto session_uuid = string_at(root, "session_uuid");
    auto history = parse_history(root);
    if (!model_id || !system_prompt || !session_uuid || !history) {
        return corrupt(file);
    }
    return SessionData{
        .model_id = std::move(*model_id),
        .system_prompt = std::move(*system_prompt),
        .session_uuid = std::move(*session_uuid),
        .history = std::move(*history),
    };
}

std::string serialize(const SessionData& data) {
    nlohmann::json history = nlohmann::json::array();
    for (const Message& message : data.history) {
        nlohmann::json entry = nlohmann::json::object();
        entry.emplace("role", message.role);
        entry.emplace("content", message.content);
        history.push_back(std::move(entry));
    }
    nlohmann::json root = nlohmann::json::object();
    root.emplace("v", session_format_version);
    root.emplace("model_id", data.model_id);
    root.emplace("system_prompt", data.system_prompt);
    root.emplace("session_uuid", data.session_uuid);
    root.emplace("history", std::move(history));
    // Model output is not guaranteed to be valid UTF-8; replacing bad bytes keeps dump from
    // throwing and the file parseable.
    return root.dump(json_indent, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

Result<std::string> read_text(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input.is_open()) {
        return fail(Exit::config, std::format("cannot read session file {}", file.string()));
    }
    std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) {
        return fail(Exit::config, std::format("cannot read session file {}", file.string()));
    }
    return text;
}

Result<SessionData> load_session(const fs::path& file) {
    const auto checked = check_private_file(file);
    if (!checked) {
        return std::unexpected(checked.error());
    }
    return read_text(file).and_then(
        [&](const std::string& text) { return parse_session(text, file); });
}

} // namespace

bool valid_session_name(std::string_view name) {
    if (name.empty() || name.size() > max_session_name_length || name == "." || name == "..") {
        return false;
    }
    return std::ranges::all_of(name, session_name_char);
}

Result<Session> Session::open(const fs::path& state_dir, const std::string& name) {
    if (!valid_session_name(name)) {
        return fail(Exit::usage, "invalid session name; use 1 to 64 letters, digits, '.', '_' or "
                                 "'-' (not \".\" or \"..\")");
    }
    const fs::path locks = state_dir / "locks";
    const fs::path sessions = state_dir / "sessions";
    for (const fs::path& dir : std::array{state_dir, locks, sessions}) {
        const auto made = ensure_private_dir(dir);
        if (!made) {
            return std::unexpected(made.error());
        }
    }
    auto lock = FileLock::acquire(locks / std::format("session-{}.lock", name), false);
    if (!lock) {
        if (lock.error().code == Exit::refused) {
            return fail(
                Exit::refused,
                std::format("session {} is in use by another openrouter-cli process", name));
        }
        return std::unexpected(lock.error());
    }
    Session session(std::move(*lock), sessions / std::format("{}.json", name));
    std::error_code error;
    const bool present = fs::exists(fs::symlink_status(session.file_, error));
    if (error && error != std::errc::no_such_file_or_directory) {
        return fail(Exit::config, std::format("cannot inspect session file {}: {}",
                                              session.file_.string(), error.message()));
    }
    if (present) {
        auto loaded = load_session(session.file_);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        session.data_ = std::move(*loaded);
        session.exists_ = true;
    }
    return session;
}

Result<void> Session::create(const std::string& model_id, const std::string& system_prompt) {
    if (exists_) {
        return fail(Exit::usage, "the session already exists; its model and system prompt are "
                                 "fixed at creation");
    }
    data_ = SessionData{
        .model_id = model_id,
        .system_prompt = system_prompt,
        .session_uuid = random_hex(uuid_bytes),
        .history = {},
    };
    return {};
}

Result<void> Session::commit(const Message& user, const Message& assistant) {
    if (data_.session_uuid.empty()) {
        return fail(Exit::usage, "internal error: a new session was committed before create");
    }
    SessionData updated = data_;
    updated.history.push_back(user);
    updated.history.push_back(assistant);
    const auto written = atomic_write(file_, serialize(updated));
    if (!written) {
        return std::unexpected(written.error());
    }
    data_ = std::move(updated);
    exists_ = true;
    return {};
}

} // namespace orcli
