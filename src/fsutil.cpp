#include "fsutil.hpp"

#include "common.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace orcli {

namespace fs = std::filesystem;

namespace {

constexpr mode_t private_dir_mode = S_IRWXU;
constexpr mode_t private_file_mode = S_IRUSR | S_IWUSR;
constexpr mode_t group_other_bits = S_IRWXG | S_IRWXO;
constexpr std::size_t temp_suffix_bytes = 8;
// uniform_int_distribution is undefined for char types, so bytes are drawn as unsigned int.
constexpr unsigned int byte_max = 0xFF;

// Reads errno first, before any other call can overwrite it.
std::unexpected<Error> sys_fail(std::string_view action, const fs::path& path) {
    const int error_number = errno;
    return fail(Exit::config,
                std::format("cannot {} {}: {}", action, path.string(),
                            std::error_code(error_number, std::system_category()).message()));
}

template <class Call> auto retry_eintr(Call call) {
    for (;;) {
        const auto value = call();
        if (value != -1 || errno != EINTR) {
            return value;
        }
    }
}

// close(2) is not retried on EINTR: on Linux the descriptor is released either way.
void close_descriptor(int descriptor) noexcept {
    if (descriptor >= 0) {
        ::close(descriptor);
    }
}

class Descriptor {
public:
    explicit Descriptor(int value) noexcept : value_(value) {}
    Descriptor(Descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Descriptor& operator=(Descriptor&&) = delete;
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    ~Descriptor() { close_descriptor(value_); }

    [[nodiscard]] int get() const noexcept { return value_; }
    [[nodiscard]] int release() noexcept { return std::exchange(value_, -1); }

    // A failed close can report a lost write (NFS, quota), so files that were written use this.
    Result<void> close(const fs::path& path) {
        if (::close(release()) != 0) {
            return sys_fail("close", path);
        }
        return {};
    }

private:
    int value_;
};

// The mode applies only when flags include O_CREAT.
Result<Descriptor> open_file(const fs::path& path, int flags) {
    const int value = retry_eintr([&] { return ::open(path.c_str(), flags, private_file_mode); });
    if (value < 0) {
        return sys_fail("open", path);
    }
    return Descriptor(value);
}

Result<void> write_all(int descriptor, std::string_view data, const fs::path& path) {
    std::string_view rest = data;
    while (!rest.empty()) {
        const ssize_t written =
            retry_eintr([&] { return ::write(descriptor, rest.data(), rest.size()); });
        if (written < 0) {
            return sys_fail("write", path);
        }
        rest.remove_prefix(static_cast<std::size_t>(written));
    }
    return {};
}

Result<void> sync_descriptor(int descriptor, const fs::path& path) {
    if (retry_eintr([&] { return ::fsync(descriptor); }) != 0) {
        return sys_fail("fsync", path);
    }
    return {};
}

Result<void> sync_directory(const fs::path& dir) {
    auto handle = open_file(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (!handle) {
        return std::unexpected(handle.error());
    }
    return sync_descriptor(handle->get(), dir).and_then([&] { return handle->close(dir); });
}

// Removes the temporary file unless the rename that publishes it succeeded.
class TempFileGuard {
public:
    explicit TempFileGuard(fs::path path) : path_(std::move(path)) {}
    TempFileGuard(TempFileGuard&&) = delete;
    TempFileGuard& operator=(TempFileGuard&&) = delete;
    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;
    ~TempFileGuard() {
        if (armed_) {
            std::error_code ignored;
            fs::remove(path_, ignored);
        }
    }
    void dismiss() noexcept { armed_ = false; }

private:
    fs::path path_;
    bool armed_ = true;
};

Result<void> check_private(const fs::path& path, const struct stat& info, std::string_view fix) {
    if (info.st_uid != ::geteuid()) {
        return fail(Exit::config,
                    std::format("{} is owned by another user; it must belong to the user running "
                                "openrouter-cli",
                                path.string()));
    }
    if ((info.st_mode & group_other_bits) != 0) {
        return fail(Exit::config,
                    std::format("{} grants group or other permissions; fix with: {} {}",
                                path.string(), fix, path.string()));
    }
    return {};
}

Result<void> make_directory(const fs::path& dir) {
    if (::mkdir(dir.c_str(), private_dir_mode) == 0) {
        // The umask may have cleared owner bits, so the mode is set explicitly.
        if (::chmod(dir.c_str(), private_dir_mode) != 0) {
            return sys_fail("chmod", dir);
        }
        return {};
    }
    if (errno == EEXIST) {
        return {};
    }
    return sys_fail("create directory", dir);
}

} // namespace

Result<void> ensure_private_dir(const fs::path& dir) {
    fs::path prefix;
    for (const fs::path& part : dir) {
        prefix /= part;
        const auto made = make_directory(prefix);
        if (!made) {
            return std::unexpected(made.error());
        }
    }
    struct stat info{};
    if (::lstat(dir.c_str(), &info) != 0) {
        return sys_fail("inspect", dir);
    }
    if (S_ISLNK(info.st_mode)) {
        return fail(Exit::config,
                    std::format("{} is a symlink; it must be a real directory", dir.string()));
    }
    if (!S_ISDIR(info.st_mode)) {
        return fail(Exit::config, std::format("{} exists but is not a directory", dir.string()));
    }
    return check_private(dir, info, "chmod 700");
}

Result<void> check_private_file(const fs::path& file) {
    struct stat info{};
    if (::lstat(file.c_str(), &info) != 0) {
        if (errno == ENOENT) {
            return {};
        }
        return sys_fail("inspect", file);
    }
    if (S_ISLNK(info.st_mode)) {
        return fail(Exit::config,
                    std::format("{} is a symlink; it must be a regular file", file.string()));
    }
    return check_private(file, info, "chmod 600");
}

Result<FileLock> FileLock::acquire(const fs::path& lock_file, bool blocking) {
    auto handle = open_file(lock_file, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW);
    if (!handle) {
        return std::unexpected(handle.error());
    }
    const int operation = blocking ? LOCK_EX : (LOCK_EX | LOCK_NB);
    if (retry_eintr([&] { return ::flock(handle->get(), operation); }) != 0) {
        if (errno == EWOULDBLOCK) {
            return fail(Exit::refused, std::format("{} is busy", lock_file.string()));
        }
        return sys_fail("lock", lock_file);
    }
    return FileLock(handle->release());
}

FileLock::FileLock(FileLock&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

FileLock& FileLock::operator=(FileLock&& other) noexcept {
    if (this != &other) {
        close_descriptor(std::exchange(fd_, std::exchange(other.fd_, -1)));
    }
    return *this;
}

// Closing the descriptor releases the flock; the lock file itself stays for the next holder.
FileLock::~FileLock() {
    close_descriptor(fd_);
}

std::string random_hex(std::size_t bytes) {
    std::random_device device;
    std::uniform_int_distribution<unsigned int> distribution(0, byte_max);
    std::string hex;
    hex.reserve(bytes * 2);
    for (std::size_t index = 0; index < bytes; ++index) {
        hex += std::format("{:02x}", distribution(device));
    }
    return hex;
}

Result<Replaced> atomic_replace(const fs::path& target, std::string_view data) {
    fs::path temp = target;
    temp += std::format(".tmp.{}.{}", ::getpid(), random_hex(temp_suffix_bytes));
    auto handle = open_file(temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW);
    if (!handle) {
        return std::unexpected(handle.error());
    }
    TempFileGuard guard(temp);
    const auto written = write_all(handle->get(), data, temp)
                             .and_then([&] { return sync_descriptor(handle->get(), temp); })
                             .and_then([&] { return handle->close(temp); });
    if (!written) {
        return std::unexpected(written.error());
    }
    if (std::rename(temp.c_str(), target.c_str()) != 0) {
        return sys_fail("rename into place", target);
    }
    guard.dismiss();
    // Past the rename the new content is visible, so reporting an error would make callers
    // believe the old content survived.
    const fs::path parent = target.parent_path();
    return Replaced{.durable = sync_directory(parent.empty() ? fs::path(".") : parent).has_value()};
}

Result<void> atomic_write(const fs::path& target, std::string_view data) {
    const auto replaced = atomic_replace(target, data);
    if (!replaced) {
        return std::unexpected(replaced.error());
    }
    if (!replaced->durable) {
        return fail(Exit::config,
                    std::format("replaced {} but could not sync its directory, so the change may "
                                "not survive a power loss",
                                target.string()));
    }
    return {};
}

Result<int> create_exclusive(const fs::path& file) {
    auto handle = open_file(file, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW);
    if (!handle) {
        return std::unexpected(handle.error());
    }
    return handle->release();
}

namespace {

// A record appended after a crash mid-write would otherwise join the partial line.
Result<void> repair_trailing_newline(int descriptor, const fs::path& file) {
    struct stat info{};
    if (::fstat(descriptor, &info) != 0) {
        return sys_fail("inspect", file);
    }
    if (info.st_size == 0) {
        return {};
    }
    char last = '\0';
    const ssize_t read_count =
        retry_eintr([&] { return ::pread(descriptor, &last, 1, info.st_size - 1); });
    if (read_count < 0) {
        return sys_fail("read", file);
    }
    if (read_count != 1) {
        return fail(Exit::config, std::format("cannot read the last byte of {}", file.string()));
    }
    if (last == '\n') {
        return {};
    }
    return write_all(descriptor, "\n", file);
}

} // namespace

Result<void> append_record(const AppendPaths& paths, std::string_view data) {
    if (data.empty() || !data.ends_with('\n')) {
        return fail(Exit::usage, "internal error: an appended record must end with a newline");
    }
    const auto lock = FileLock::acquire(paths.lock_file, true);
    if (!lock) {
        return std::unexpected(lock.error());
    }
    const auto checked = check_private_file(paths.file);
    if (!checked) {
        return std::unexpected(checked.error());
    }
    auto handle = open_file(paths.file, O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
    if (!handle) {
        return std::unexpected(handle.error());
    }
    return repair_trailing_newline(handle->get(), paths.file)
        .and_then([&] { return write_all(handle->get(), data, paths.file); })
        .and_then([&] { return sync_descriptor(handle->get(), paths.file); })
        .and_then([&] { return handle->close(paths.file); });
}

} // namespace orcli
