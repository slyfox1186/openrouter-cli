#include "context.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kLimit = 1'000'000;
constexpr std::size_t kBinaryProbeBytes = 8192;
constexpr int kNumberedLines = 10;

fs::path make_temp_dir() {
    std::random_device entropy;
    while (true) {
        const fs::path candidate = fs::temp_directory_path() /
                                   std::format("orcli-context-{:08x}{:08x}", entropy(), entropy());
        if (fs::create_directory(candidate)) {
            return candidate;
        }
    }
}

class TempDir {
public:
    TempDir() : path_(make_temp_dir()) {}
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void write_file(const fs::path& file, std::string_view content) {
    std::ofstream out(file, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    REQUIRE(out.good());
}

orcli::FileSnapshot snapshot(const std::string& display_path, std::string_view content) {
    return orcli::FileSnapshot{.display_path = display_path,
                               .resolved = fs::path("/srv") / display_path,
                               .content = std::string(content)};
}

} // namespace

TEST_CASE("fence_for is longer than any backtick run") {
    CHECK(orcli::fence_for("plain text") == "```");
    CHECK(orcli::fence_for("inline `code` and ``more``") == "```");
    CHECK(orcli::fence_for("a ``` b") == "````");
    CHECK(orcli::fence_for("x ```` y ``") == "`````");
}

TEST_CASE("pack_files right-aligns line numbers to the widest number") {
    std::string content;
    for (int line = 1; line <= kNumberedLines; ++line) {
        content += std::format("line {}\n", line);
    }
    CHECK(orcli::pack_files({snapshot("notes.txt", content)}) == "File: notes.txt\n"
                                                                 "```\n"
                                                                 " 1 | line 1\n"
                                                                 " 2 | line 2\n"
                                                                 " 3 | line 3\n"
                                                                 " 4 | line 4\n"
                                                                 " 5 | line 5\n"
                                                                 " 6 | line 6\n"
                                                                 " 7 | line 7\n"
                                                                 " 8 | line 8\n"
                                                                 " 9 | line 9\n"
                                                                 "10 | line 10\n"
                                                                 "```\n");
}

TEST_CASE("pack_files handles empty files and a missing final newline") {
    CHECK(orcli::pack_files({snapshot("empty.txt", "")}) == "File: empty.txt\n```\n```\n");
    CHECK(orcli::pack_files({snapshot("f.txt", "a\nb")}) ==
          "File: f.txt\n```\n1 | a\n2 | b\n```\n");
    CHECK(orcli::pack_files({snapshot("blank.txt", "\n\n")}) ==
          "File: blank.txt\n```\n1 | \n2 | \n```\n");
}

TEST_CASE("pack_files grows the fence and separates files with a blank line") {
    CHECK(orcli::pack_files({snapshot("a.md", "````\n"), snapshot("b.txt", "x\n")}) ==
          "File: a.md\n`````\n1 | ````\n`````\n"
          "\n"
          "File: b.txt\n```\n1 | x\n```\n");
}

TEST_CASE("read_snapshot keeps the display path as given") {
    const TempDir dir;
    REQUIRE(fs::create_directory(dir.path() / "sub"));
    write_file(dir.path() / "a.txt", "hello\n");
    const fs::path given = dir.path() / "sub" / ".." / "a.txt";
    const auto result = orcli::read_snapshot(given, kLimit);
    REQUIRE(result.has_value());
    const orcli::FileSnapshot& snap = result.value();
    CHECK(snap.display_path == given.string());
    CHECK(snap.resolved == fs::canonical(dir.path() / "a.txt"));
    CHECK(snap.content == "hello\n");
}

TEST_CASE("read_snapshot refuses binary files without echoing content") {
    const TempDir dir;
    const fs::path file = dir.path() / "data.bin";
    write_file(file, std::string("SENTINEL") + '\0' + "more");
    const auto result = orcli::read_snapshot(file, kLimit);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == orcli::Exit::refused);
    CHECK_FALSE(result.error().message.contains("SENTINEL"));
}

TEST_CASE("read_snapshot only probes the first 8 KiB for NUL bytes") {
    const TempDir dir;
    const fs::path late = dir.path() / "late.txt";
    write_file(late, std::string(kBinaryProbeBytes, 'a') + '\0');
    CHECK(orcli::read_snapshot(late, kLimit).has_value());
    const fs::path early = dir.path() / "early.txt";
    write_file(early, std::string(kBinaryProbeBytes - 1, 'a') + '\0');
    CHECK_FALSE(orcli::read_snapshot(early, kLimit).has_value());
}

TEST_CASE("read_snapshot enforces the size limit") {
    const TempDir dir;
    const fs::path file = dir.path() / "ten.txt";
    const std::string content = "0123456789";
    write_file(file, content);
    CHECK(orcli::read_snapshot(file, content.size()).has_value());
    const auto result = orcli::read_snapshot(file, content.size() - 1);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == orcli::Exit::refused);
    CHECK(result.error().message.contains(file.string()));
    CHECK(result.error().message.contains(std::format("{}-byte limit", content.size() - 1)));
}

TEST_CASE("read_snapshot refuses directories and missing files") {
    const TempDir dir;
    const auto directory = orcli::read_snapshot(dir.path(), kLimit);
    REQUIRE_FALSE(directory.has_value());
    CHECK(directory.error().code == orcli::Exit::refused);
    const auto missing = orcli::read_snapshot(dir.path() / "missing.txt", kLimit);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == orcli::Exit::usage);
}

TEST_CASE("read_snapshot reads credential-named files like any other file") {
    const TempDir dir;
    const std::string text = "OPENROUTER_API_KEY=SENTINEL\n";
    write_file(dir.path() / ".env", text);
    fs::create_symlink(dir.path() / ".env", dir.path() / "notes.txt");
    const auto linked = orcli::read_snapshot(dir.path() / "notes.txt", kLimit);
    REQUIRE(linked.has_value());
    CHECK(linked->content == text);
    const auto direct = orcli::read_snapshot(dir.path() / ".env", kLimit);
    REQUIRE(direct.has_value());
    CHECK(direct->content == text);
    fs::create_directory(dir.path() / ".ssh");
    write_file(dir.path() / ".ssh/id_ed25519", text);
    CHECK(orcli::read_snapshot(dir.path() / ".ssh/id_ed25519", kLimit).has_value());
}

TEST_CASE("bounded packing counts framing and line number overhead") {
    const std::vector<orcli::FileSnapshot> files{snapshot("a.txt", "one\ntwo")};
    const auto expected = orcli::pack_files(files);
    const auto exact = orcli::pack_files(files, expected.size());
    REQUIRE(exact.has_value());
    CHECK(*exact == expected);
    CHECK_FALSE(orcli::pack_files(files, expected.size() - 1).has_value());
    CHECK_FALSE(orcli::pack_files(files, files.front().content.size()).has_value());
}
