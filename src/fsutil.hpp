#ifndef ORCLI_FSUTIL_HPP
#define ORCLI_FSUTIL_HPP

#include "common.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace orcli {

using LockDeadline = std::optional<std::chrono::steady_clock::time_point>;

// Creates the directory (and parents) with mode 0700 when missing. Exit::config when it is a
// symlink, not a directory, not owned by the user, or grants group or other permissions.
Result<void> ensure_private_dir(const std::filesystem::path& dir);

// Exit::config when an existing file is not regular, not owned by the user, or grants group
// or other permissions. A missing file is fine.
Result<void> check_private_file(const std::filesystem::path& file);

// flock(2) on a dedicated lock file that is never renamed or replaced.
class FileLock {
public:
    // blocking=false: Exit::refused when another process holds the lock.
    // Default waits honor cancellation. A cleanup deadline permits interrupted accounting
    // until that absolute time; callers reuse it across all outstanding records.
    static Result<FileLock> acquire(const std::filesystem::path& lock_file, bool blocking,
                                    LockDeadline deadline = {});
    FileLock(FileLock&& other) noexcept;
    FileLock& operator=(FileLock&& other) noexcept;
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    ~FileLock();

private:
    explicit FileLock(int descriptor) : fd_(descriptor) {}
    int fd_ = -1;
};

// Lowercase hex of `bytes` random bytes from std::random_device (getrandom(2) on Linux); the
// one helper for temp-file suffixes, session ids and call ids.
std::string random_hex(std::size_t bytes);

struct Replaced {
    // False when the rename succeeded but the directory fsync failed: the new content is in
    // place, but its survival across a power loss is unconfirmed.
    bool durable = true;
};

// Writes a uniquely named O_EXCL temp file (0600) beside target, fsyncs it, renames it over
// target and fsyncs the directory. The rename is the commit point: an error means target was
// not replaced (the temp file is removed); a value means it was.
Result<Replaced> atomic_replace(const std::filesystem::path& target, std::string_view data);

// atomic_replace for callers that need durability: durable == false becomes an error.
Result<void> atomic_write(const std::filesystem::path& target, std::string_view data);

// Creates a new file with O_CREAT|O_EXCL|O_NOFOLLOW and mode 0600; returns the descriptor.
Result<int> create_exclusive(const std::filesystem::path& file);

struct AppendPaths {
    std::filesystem::path file;
    std::filesystem::path lock_file;
};

// Appends data (which must end in '\n') under lock_file. If the file does not end with a
// newline, one is written first so a crashed partial line cannot swallow this record. Checks
// every write and fsyncs before releasing the lock. Creates the file 0600 when missing.
Result<void> append_record(const AppendPaths& paths, std::string_view data,
                           LockDeadline deadline = {});

// Reads only regular files, with the bound enforced while reading.
Result<std::string> read_regular_file(const std::filesystem::path& file, std::size_t max_bytes,
                                      bool absent_empty = false);

// Does not change stdin flags; errors cleanse any accumulated input.
Result<std::string> read_input(std::size_t max_bytes, bool stop_at_newline);

// Synchronizes completed file contents and publication of its directory entry.
Result<void> sync_file(int descriptor, const std::filesystem::path& file);

} // namespace orcli

#endif // ORCLI_FSUTIL_HPP
