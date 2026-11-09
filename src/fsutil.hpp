#pragma once

#include "common.hpp"

#include <filesystem>
#include <string_view>

namespace orcli {

// Creates the directory (and parents) with mode 0700 when missing. Exit::config when it is a
// symlink, not a directory, not owned by the user, or grants group or other permissions.
Result<void> ensure_private_dir(const std::filesystem::path& dir);

// Exit::config when an existing file is a symlink, not owned by the user, or grants group or
// other permissions. A missing file is fine.
Result<void> check_private_file(const std::filesystem::path& file);

// flock(2) on a dedicated lock file that is never renamed or replaced.
class FileLock {
public:
    // blocking=false: Exit::refused when another process holds the lock.
    static Result<FileLock> acquire(const std::filesystem::path& lock_file, bool blocking);
    FileLock(FileLock&& other) noexcept;
    FileLock& operator=(FileLock&& other) noexcept;
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    ~FileLock();

private:
    explicit FileLock(int fd) : fd_(fd) {}
    int fd_ = -1;
};

// Writes a uniquely named O_EXCL temp file (0600) beside target, fsyncs it, renames it over
// target and fsyncs the directory.
Result<void> atomic_write(const std::filesystem::path& target, std::string_view data);

// Creates a new file with O_CREAT|O_EXCL|O_NOFOLLOW and mode 0600; returns the descriptor.
Result<int> create_exclusive(const std::filesystem::path& file);

// Appends data (which must end in '\n') under lock_file. If the file does not end with a
// newline, one is written first so a crashed partial line cannot swallow this record. Checks
// every write and fsyncs before releasing the lock. Creates the file 0600 when missing.
Result<void> append_record(const std::filesystem::path& file, const std::filesystem::path& lock_file,
                           std::string_view data);

} // namespace orcli
