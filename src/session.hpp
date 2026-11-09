#ifndef ORCLI_SESSION_HPP
#define ORCLI_SESSION_HPP

#include "common.hpp"
#include "fsutil.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orcli {

// [A-Za-z0-9._-]{1,64}, not "." or "..".
bool valid_session_name(std::string_view name);

struct SessionData {
    std::string model_id;      // pinned at creation
    std::string system_prompt; // pinned at creation
    std::string session_uuid;  // sent as session_id for sticky routing
    std::vector<Message> history;
};

class Session {
public:
    // Validates the name (Exit::usage), takes state_dir/locks/session-<name>.lock without
    // blocking (Exit::refused "session busy") and loads state_dir/sessions/<name>.json if
    // present. The lock is held until the Session is destroyed.
    static Result<Session> open(const std::filesystem::path& state_dir, const std::string& name);

    [[nodiscard]] bool exists() const { return exists_; }
    [[nodiscard]] const SessionData& data() const { return data_; }

    // For a new session: pins model, system prompt and a fresh uuid. Exit::usage if it exists.
    Result<void> create(const std::string& model_id, const std::string& system_prompt);

    // Appends one completed exchange and atomically rewrites the session file.
    Result<void> commit(const Message& user, const Message& assistant);

private:
    Session(FileLock lock, std::filesystem::path file)
        : lock_(std::move(lock)), file_(std::move(file)) {}
    FileLock lock_;
    std::filesystem::path file_;
    bool exists_ = false;
    SessionData data_;
};

} // namespace orcli

#endif // ORCLI_SESSION_HPP
