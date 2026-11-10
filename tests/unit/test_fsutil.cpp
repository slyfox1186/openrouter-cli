#include "fsutil.hpp"

#include "common.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <random>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <chrono>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using orcli::Exit;

namespace {

constexpr fs::perms private_dir_perms = fs::perms::owner_all;
constexpr fs::perms private_file_perms = fs::perms::owner_read | fs::perms::owner_write;
constexpr fs::perms shared_dir_perms = fs::perms::owner_all | fs::perms::group_read |
                                       fs::perms::group_exec | fs::perms::others_read |
                                       fs::perms::others_exec;
constexpr fs::perms shared_file_perms =
    private_file_perms | fs::perms::group_read | fs::perms::others_read;
constexpr fs::perms read_only_dir_perms = fs::perms::owner_read | fs::perms::owner_exec;
constexpr std::size_t sample_hex_bytes = 16;
constexpr int writer_threads = 8;
constexpr int records_per_thread = 50;
constexpr std::size_t record_padding = 300;

class TempDir {
public:
    TempDir() {
        std::random_device device;
        std::uniform_int_distribution<std::uint32_t> distribution;
        path_ = fs::temp_directory_path() / std::format("orcli-fsutil-{:08x}{:08x}",
                                                        distribution(device), distribution(device));
        fs::create_directory(path_);
        fs::permissions(path_, private_dir_perms);
    }
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

// Restores a directory's mode so TempDir can remove it even when a check fails first.
class RestorePerms {
public:
    RestorePerms(fs::path dir, fs::perms perms) : dir_(std::move(dir)), perms_(perms) {}
    RestorePerms(RestorePerms&&) = delete;
    RestorePerms& operator=(RestorePerms&&) = delete;
    RestorePerms(const RestorePerms&) = delete;
    RestorePerms& operator=(const RestorePerms&) = delete;
    ~RestorePerms() {
        std::error_code ignored;
        fs::permissions(dir_, perms_, ignored);
    }

private:
    fs::path dir_;
    fs::perms perms_;
};

std::set<std::string> entry_names(const fs::path& dir) {
    std::set<std::string> names;
    for (const auto& entry : fs::directory_iterator(dir)) {
        names.insert(entry.path().filename().string());
    }
    return names;
}

std::string read_file(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

fs::perms perms_of(const fs::path& path) {
    return fs::symlink_status(path).permissions();
}

std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> lines;
    for (const auto piece : std::views::split(text, '\n')) {
        lines.emplace_back(piece.begin(), piece.end());
    }
    if (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    return lines;
}

} // namespace

TEST_CASE("ensure_private_dir creates missing directories with mode 0700") {
    const TempDir temp;
    const fs::path nested = temp.path() / "state" / "locks";
    REQUIRE(orcli::ensure_private_dir(nested).has_value());
    CHECK(fs::is_directory(nested));
    CHECK(perms_of(nested) == private_dir_perms);
    CHECK(perms_of(temp.path() / "state") == private_dir_perms);
    CHECK(orcli::ensure_private_dir(nested).has_value());
}

TEST_CASE("ensure_private_dir rejects a directory readable by others") {
    const TempDir temp;
    const fs::path dir = temp.path() / "shared";
    fs::create_directory(dir);
    fs::permissions(dir, shared_dir_perms);
    const auto result = orcli::ensure_private_dir(dir);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Exit::config);
    CHECK(result.error().message.find("chmod 700") != std::string::npos);
    CHECK(perms_of(dir) == shared_dir_perms);
}

TEST_CASE("ensure_private_dir rejects a symlink and a regular file") {
    const TempDir temp;
    const fs::path real = temp.path() / "real";
    REQUIRE(orcli::ensure_private_dir(real).has_value());
    const fs::path link = temp.path() / "link";
    fs::create_directory_symlink(real, link);
    const auto through_link = orcli::ensure_private_dir(link);
    REQUIRE_FALSE(through_link.has_value());
    CHECK(through_link.error().code == Exit::config);

    const fs::path file = temp.path() / "file";
    REQUIRE(orcli::atomic_write(file, "x").has_value());
    const auto on_file = orcli::ensure_private_dir(file);
    REQUIRE_FALSE(on_file.has_value());
    CHECK(on_file.error().code == Exit::config);
}

TEST_CASE("check_private_file accepts missing and private files only") {
    const TempDir temp;
    CHECK(orcli::check_private_file(temp.path() / "missing").has_value());

    const fs::path file = temp.path() / "file";
    REQUIRE(orcli::atomic_write(file, "data").has_value());
    CHECK(orcli::check_private_file(file).has_value());

    fs::permissions(file, shared_file_perms);
    const auto shared = orcli::check_private_file(file);
    REQUIRE_FALSE(shared.has_value());
    CHECK(shared.error().code == Exit::config);
    CHECK(shared.error().message.find("chmod 600") != std::string::npos);

    const fs::path link = temp.path() / "link";
    fs::permissions(file, private_file_perms);
    fs::create_symlink(file, link);
    const auto linked = orcli::check_private_file(link);
    REQUIRE_FALSE(linked.has_value());
    CHECK(linked.error().code == Exit::config);
}

TEST_CASE("a non-blocking lock is refused while another FileLock holds it") {
    const TempDir temp;
    const fs::path lock_file = temp.path() / "test.lock";
    {
        const auto holder = orcli::FileLock::acquire(lock_file, false);
        REQUIRE(holder.has_value());
        const auto contender = orcli::FileLock::acquire(lock_file, false);
        REQUIRE_FALSE(contender.has_value());
        CHECK(contender.error().code == Exit::refused);
    }
    const auto after_release = orcli::FileLock::acquire(lock_file, false);
    CHECK(after_release.has_value());
    CHECK(fs::exists(lock_file));
    CHECK(perms_of(lock_file) == private_file_perms);
}

TEST_CASE("a moved FileLock keeps the lock until the new owner is destroyed") {
    const TempDir temp;
    const fs::path lock_file = temp.path() / "test.lock";
    {
        auto original = orcli::FileLock::acquire(lock_file, true);
        REQUIRE(original.has_value());
        const orcli::FileLock moved = std::move(*original);
        original = orcli::fail(Exit::usage, "released the moved-from holder");
        CHECK_FALSE(orcli::FileLock::acquire(lock_file, false).has_value());

        auto assigned = orcli::FileLock::acquire(temp.path() / "other.lock", false);
        REQUIRE(assigned.has_value());
        auto second = orcli::FileLock::acquire(temp.path() / "second.lock", false);
        REQUIRE(second.has_value());
        *assigned = std::move(*second);
        CHECK(orcli::FileLock::acquire(temp.path() / "other.lock", false).has_value());
        CHECK_FALSE(orcli::FileLock::acquire(temp.path() / "second.lock", false).has_value());
    }
    CHECK(orcli::FileLock::acquire(lock_file, false).has_value());
}

TEST_CASE("atomic_write replaces content and leaves no temporary files") {
    const TempDir temp;
    const fs::path target = temp.path() / "registry.json";
    REQUIRE(orcli::atomic_write(target, "first").has_value());
    CHECK(read_file(target) == "first");
    REQUIRE(orcli::atomic_write(target, "second").has_value());
    CHECK(read_file(target) == "second");
    CHECK(perms_of(target) == private_file_perms);

    std::size_t entries = 0;
    for (const auto& entry : fs::directory_iterator(temp.path())) {
        CHECK(entry.path().filename() == "registry.json");
        ++entries;
    }
    CHECK(entries == 1);
}

TEST_CASE("random_hex returns two lowercase hex characters per byte") {
    const std::string first = orcli::random_hex(sample_hex_bytes);
    CHECK(first.size() == sample_hex_bytes * 2);
    CHECK(first.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(first != orcli::random_hex(sample_hex_bytes));
    CHECK(orcli::random_hex(0).empty());
}

TEST_CASE("atomic_replace reports a durable replacement") {
    const TempDir temp;
    const fs::path target = temp.path() / "catalog.json";
    const auto created = orcli::atomic_replace(target, "first");
    REQUIRE(created.has_value());
    CHECK(created->durable);
    const auto replaced = orcli::atomic_replace(target, "second");
    REQUIRE(replaced.has_value());
    CHECK(replaced->durable);
    CHECK(read_file(target) == "second");
    CHECK(perms_of(target) == private_file_perms);
    CHECK(entry_names(temp.path()) == std::set<std::string>{"catalog.json"});
}

TEST_CASE("atomic_replace that fails before the rename leaves the target untouched") {
    const TempDir temp;
    const fs::path dir = temp.path() / "locked";
    REQUIRE(orcli::ensure_private_dir(dir).has_value());
    const fs::path target = dir / "registry.json";
    REQUIRE(orcli::atomic_write(target, "old").has_value());
    const RestorePerms restore(dir, private_dir_perms);
    fs::permissions(dir, read_only_dir_perms);

    const auto replaced = orcli::atomic_replace(target, "new");
    REQUIRE_FALSE(replaced.has_value());
    CHECK(replaced.error().code == Exit::config);
    CHECK(read_file(target) == "old");
    CHECK(entry_names(dir) == std::set<std::string>{"registry.json"});

    const auto written = orcli::atomic_write(target, "new");
    REQUIRE_FALSE(written.has_value());
    CHECK(read_file(target) == "old");
}

TEST_CASE("create_exclusive creates a private file and refuses an existing one") {
    const TempDir temp;
    const fs::path file = temp.path() / "answer.md";
    const auto created = orcli::create_exclusive(file);
    REQUIRE(created.has_value());
    CHECK(::close(*created) == 0);
    CHECK(perms_of(file) == private_file_perms);

    const auto again = orcli::create_exclusive(file);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code == Exit::config);
}

TEST_CASE("append_record creates the file and repairs a partial last line") {
    const TempDir temp;
    const fs::path file = temp.path() / "costs.jsonl";
    const fs::path lock_file = temp.path() / "costs.lock";

    REQUIRE(orcli::append_record({.file = file, .lock_file = lock_file}, "first\n").has_value());
    CHECK(read_file(file) == "first\n");
    CHECK(perms_of(file) == private_file_perms);

    REQUIRE(orcli::atomic_write(file, "first\n{\"partial").has_value());
    REQUIRE(orcli::append_record({.file = file, .lock_file = lock_file}, "second\n").has_value());
    CHECK(read_file(file) == "first\n{\"partial\nsecond\n");

    const auto unterminated = orcli::append_record({.file = file, .lock_file = lock_file}, "third");
    REQUIRE_FALSE(unterminated.has_value());
    CHECK(unterminated.error().code == Exit::usage);
    CHECK_FALSE(orcli::append_record({.file = file, .lock_file = lock_file}, "").has_value());
}

TEST_CASE("append_record rejects a log file readable by others") {
    const TempDir temp;
    const fs::path file = temp.path() / "costs.jsonl";
    REQUIRE(orcli::atomic_write(file, "first\n").has_value());
    fs::permissions(file, shared_file_perms);
    const auto result =
        orcli::append_record({.file = file, .lock_file = temp.path() / "costs.lock"}, "second\n");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Exit::config);
    CHECK(read_file(file) == "first\n");
}

TEST_CASE("concurrent append_record calls keep every line intact") {
    const TempDir temp;
    const fs::path file = temp.path() / "costs.jsonl";
    const fs::path lock_file = temp.path() / "costs.lock";
    const std::string padding(record_padding, 'x');
    std::atomic<int> failures = 0;
    std::set<std::string> expected;
    for (int writer = 0; writer < writer_threads; ++writer) {
        for (int record = 0; record < records_per_thread; ++record) {
            expected.insert(std::format("writer {} record {} {}", writer, record, padding));
        }
    }
    {
        std::vector<std::jthread> threads;
        for (int writer = 0; writer < writer_threads; ++writer) {
            threads.emplace_back([&, writer] {
                for (int record = 0; record < records_per_thread; ++record) {
                    const std::string line =
                        std::format("writer {} record {} {}\n", writer, record, padding);
                    if (!orcli::append_record({.file = file, .lock_file = lock_file}, line)
                             .has_value()) {
                        ++failures;
                    }
                }
            });
        }
    }
    CHECK(failures.load() == 0);
    const std::vector<std::string> lines = lines_of(read_file(file));
    CHECK(lines.size() == expected.size());
    const std::set<std::string> seen(lines.begin(), lines.end());
    CHECK(seen == expected);
}

TEST_CASE("private files and locks reject nonregular objects") {
    const TempDir temp;
    const auto fifo = temp.path() / "pipe";
    REQUIRE(::mkfifo(fifo.c_str(), S_IRUSR | S_IWUSR) == 0);
    CHECK_FALSE(orcli::check_private_file(fifo).has_value());
    CHECK_FALSE(orcli::FileLock::acquire(fifo, false).has_value());
    CHECK_FALSE(orcli::check_private_file(temp.path()).has_value());
    CHECK_FALSE(orcli::FileLock::acquire(temp.path(), false).has_value());
}

TEST_CASE("lock cleanup uses a caller supplied absolute deadline") {
    const TempDir temp;
    const auto path = temp.path() / "lock";
    const auto held = orcli::FileLock::acquire(path, false);
    REQUIRE(held.has_value());
    const auto result = orcli::FileLock::acquire(path, true, std::chrono::steady_clock::now());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Exit::interrupted);
}

TEST_CASE("regular reads enforce bounds and do not treat inspection errors as absence") {
    const TempDir temp;
    const auto file = temp.path() / "data";
    REQUIRE(orcli::atomic_write(file, "abcd").has_value());
    const auto exact = orcli::read_regular_file(file, 4);
    REQUIRE(exact.has_value());
    CHECK(*exact == "abcd");
    CHECK_FALSE(orcli::read_regular_file(file, 3).has_value());
    CHECK_FALSE(orcli::read_regular_file(file / "child", 4, true).has_value());
    CHECK_FALSE(orcli::read_regular_file(temp.path(), 4, true).has_value());
    const auto absent = orcli::read_regular_file(temp.path() / "absent", 4, true);
    REQUIRE(absent.has_value());
    CHECK(absent->empty());
}

TEST_CASE("unsafe lock links and shared permissions are rejected") {
    const TempDir temp;
    const auto lock = temp.path() / "lock";
    REQUIRE(orcli::atomic_write(lock, "").has_value());
    fs::create_hard_link(lock, temp.path() / "alias");
    CHECK_FALSE(orcli::FileLock::acquire(lock, false).has_value());
    fs::remove(temp.path() / "alias");
    fs::permissions(lock, shared_file_perms);
    CHECK_FALSE(orcli::FileLock::acquire(lock, false).has_value());
}
