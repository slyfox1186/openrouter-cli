#include "fsutil.hpp"

#include "common.hpp"
#include "credentials.hpp"
#include "http.hpp"
#include "posix.hpp"

#include <openssl/crypto.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace orcli {

namespace fs = std::filesystem;

namespace {

using posix::retry_eintr;

constexpr mode_t private_dir_mode = S_IRWXU;
constexpr mode_t private_file_mode = S_IRUSR | S_IWUSR;
constexpr mode_t group_other_bits = S_IRWXG | S_IRWXO;
constexpr std::size_t temp_suffix_bytes = 8;
constexpr std::size_t read_chunk_size = 65536;
constexpr std::chrono::milliseconds lock_poll_interval{20};
constexpr int input_poll_milliseconds = 100;
constexpr std::size_t input_chunk_size = 4096;
// uniform_int_distribution is undefined for char types, so bytes are drawn as unsigned int.
constexpr unsigned int byte_max = 0xFF;

// Reads errno first, before any other call can overwrite it.
std::unexpected<Error> sys_fail(std::string_view action, const fs::path& path) {
    const int error_number = errno;
    return fail(Exit::config,
                std::format("cannot {} {}: {}", action, path.string(),
                            std::error_code(error_number, std::system_category()).message()));
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
    if (!S_ISREG(info.st_mode)) {
        return fail(Exit::config, std::format("{} is not a regular file", file.string()));
    }
    return check_private(file, info, "chmod 600");
}

Result<bool> file_present(const fs::path& file) {
    std::error_code error;
    const fs::file_status status = fs::symlink_status(file, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        return fail(Exit::config,
                    std::format("cannot inspect {}: {}", file.string(), error.message()));
    }
    if (status.type() == fs::file_type::not_found) {
        return false;
    }
    if (error) {
        return fail(Exit::config,
                    std::format("cannot inspect {}: {}", file.string(), error.message()));
    }
    return true;
}

Result<std::string> read_private_file(const fs::path& file) {
    if (const auto checked = check_private_file(file); !checked) {
        return std::unexpected(checked.error());
    }
    return read_regular_file(file, std::numeric_limits<std::size_t>::max());
}

Result<FileLock> FileLock::acquire(const fs::path& lock_file, bool blocking,
                                   LockDeadline deadline) {
    const auto checked = check_private_file(lock_file);
    if (!checked) {
        return std::unexpected(checked.error());
    }
    auto handle = open_file(lock_file, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (!handle) {
        return std::unexpected(handle.error());
    }
    struct stat info{};
    if (::fstat(handle->get(), &info) != 0) {
        return sys_fail("inspect", lock_file);
    }
    if (!S_ISREG(info.st_mode) || info.st_nlink != 1) {
        return fail(Exit::config, "lock must be a regular file with one link");
    }
    const auto private_file = check_private(lock_file, info, "chmod 600");
    if (!private_file) {
        return std::unexpected(private_file.error());
    }
    for (;;) {
        if ((!deadline && interrupt_requested()) ||
            (deadline && std::chrono::steady_clock::now() >= *deadline)) {
            return fail(Exit::interrupted, "interrupted while waiting for a file lock");
        }
        if (::flock(handle->get(), LOCK_EX | LOCK_NB) == 0) {
            return FileLock(handle->release());
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            return sys_fail("lock", lock_file);
        }
        if (!blocking) {
            return fail(Exit::refused, std::format("{} is busy", lock_file.string()));
        }
        const auto wake = std::chrono::steady_clock::now() + lock_poll_interval;
        std::this_thread::sleep_until(deadline ? std::min(wake, *deadline) : wake);
    }
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

Result<FileLock> lock_state_file(const fs::path& state_dir, std::string_view lock_name,
                                 bool blocking) {
    if (const auto made = ensure_private_dir(state_dir); !made) {
        return std::unexpected(made.error());
    }
    const fs::path locks = state_dir / "locks";
    if (const auto made = ensure_private_dir(locks); !made) {
        return std::unexpected(made.error());
    }
    return FileLock::acquire(locks / lock_name, blocking);
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

Result<void> append_record(const AppendPaths& paths, std::string_view data, LockDeadline deadline) {
    if (data.empty() || !data.ends_with('\n')) {
        return fail(Exit::usage, "internal error: an appended record must end with a newline");
    }
    const auto lock = FileLock::acquire(paths.lock_file, true, deadline);
    if (!lock) {
        return std::unexpected(lock.error());
    }
    const auto checked = check_private_file(paths.file);
    if (!checked) {
        return std::unexpected(checked.error());
    }
    auto handle =
        open_file(paths.file, O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (!handle) {
        return std::unexpected(handle.error());
    }
    struct stat info{};
    if (::fstat(handle->get(), &info) != 0) {
        return sys_fail("inspect", paths.file);
    }
    if (!S_ISREG(info.st_mode)) {
        return fail(Exit::config, "accounting file must be a regular file");
    }
    const auto private_file = check_private(paths.file, info, "chmod 600");
    if (!private_file) {
        return std::unexpected(private_file.error());
    }
    return repair_trailing_newline(handle->get(), paths.file)
        .and_then([&] { return write_all(handle->get(), data, paths.file); })
        .and_then([&] { return sync_descriptor(handle->get(), paths.file); })
        .and_then([&] { return handle->close(paths.file); })
        .and_then([&] {
            return sync_directory(paths.file.parent_path().empty() ? fs::path(".")
                                                                   : paths.file.parent_path());
        });
}

Result<void> sync_file(int descriptor, const fs::path& file) {
    return sync_descriptor(descriptor, file).and_then([&] {
        return sync_directory(file.parent_path().empty() ? fs::path(".") : file.parent_path());
    });
}

Result<std::string> read_regular_file(const fs::path& file, std::size_t max_bytes,
                                      bool absent_empty) {
    const int raw = ::open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (raw < 0) {
        if (absent_empty && errno == ENOENT) {
            return std::string{};
        }
        return sys_fail("open", file);
    }
    const Descriptor handle(raw);
    struct stat info{};
    if (::fstat(handle.get(), &info) != 0) {
        return sys_fail("inspect", file);
    }
    if (!S_ISREG(info.st_mode)) {
        return fail(Exit::config, std::format("{} is not a regular file", file.string()));
    }
    if (info.st_size < 0 || std::cmp_greater(info.st_size, max_bytes)) {
        return fail(Exit::refused,
                    std::format("{} exceeds the {}-byte limit", file.string(), max_bytes));
    }
    std::string text;
    // The size was checked against the limit above; the loop still enforces the limit, because
    // the file can grow while it is read.
    text.reserve(static_cast<std::size_t>(info.st_size));
    std::array<char, read_chunk_size> buffer{};
    for (;;) {
        if (interrupt_requested()) {
            return fail(Exit::interrupted, "interrupted while reading a file");
        }
        const ssize_t count = ::read(handle.get(), buffer.data(), buffer.size());
        if (count == 0) {
            return text;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return sys_fail("read", file);
        }
        const auto size = static_cast<std::size_t>(count);
        if (size > max_bytes - text.size()) {
            return fail(Exit::refused,
                        std::format("{} exceeds the {}-byte limit", file.string(), max_bytes));
        }
        text.append(buffer.data(), size);
    }
}

Result<std::string> read_input(std::size_t max_bytes, bool stop_at_newline) {
    std::string text;
    // Key inputs fit one chunk; a large query limit should not allocate before input arrives.
    text.reserve(std::min(max_bytes, input_chunk_size));
    std::array<char, input_chunk_size> buffer{};
    for (;;) {
        if (interrupt_requested()) {
            cleanse(text);
            return fail(Exit::interrupted, "interrupted while reading standard input");
        }
        pollfd input{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
        const int ready = ::poll(&input, 1, input_poll_milliseconds);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready < 0) {
            cleanse(text);
            return fail(Exit::config, "cannot read standard input");
        }
        if (ready == 0) {
            continue;
        }
        const ssize_t count =
            ::read(STDIN_FILENO, buffer.data(), stop_at_newline ? 1 : buffer.size());
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            cleanse(text);
            return fail(Exit::config, "cannot read standard input");
        }
        if (count == 0 || (stop_at_newline && buffer.front() == '\n')) {
            OPENSSL_cleanse(buffer.data(), buffer.size());
            return text;
        }
        const auto size = static_cast<std::size_t>(count);
        if (size > max_bytes - text.size()) {
            OPENSSL_cleanse(buffer.data(), buffer.size());
            cleanse(text);
            return fail(Exit::usage,
                        std::format("standard input exceeds the {}-byte limit", max_bytes));
        }
        text.append(buffer.data(), size);
        OPENSSL_cleanse(buffer.data(), buffer.size());
    }
}

} // namespace orcli
