#include "registry.hpp"

#include "common.hpp"
#include "fsutil.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using orcli::Exit;
using orcli::Group;
using orcli::Groups;
using orcli::Result;

namespace {

class TempDir {
public:
    TempDir() {
        std::random_device device;
        std::uniform_int_distribution<std::uint32_t> distribution;
        path_ = fs::temp_directory_path() / std::format("orcli-registry-{:08x}{:08x}",
                                                        distribution(device), distribution(device));
        fs::create_directory(path_);
        fs::permissions(path_, fs::perms::owner_all);
        fs::create_directory(state());
        fs::permissions(state(), fs::perms::owner_all);
    }
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    [[nodiscard]] fs::path state() const { return path_ / "state"; }

private:
    fs::path path_;
};

void write_private(const fs::path& file, std::string_view text) {
    std::ofstream(file) << text;
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write);
}

std::string read_all(const fs::path& file) {
    std::ifstream input(file);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("saved providers round-trip, replace and remove by model id") {
    const TempDir dir;
    CHECK(orcli::load_providers(dir.state()).value().empty());

    const auto first = orcli::save_provider(dir.state(), "a/b", {"one", "two/global"});
    REQUIRE(first.has_value());
    CHECK(first->empty());
    const auto replaced = orcli::save_provider(dir.state(), "a/b", {"three"});
    REQUIRE(replaced.has_value());
    CHECK(*replaced == std::vector<std::string>{"one", "two/global"});
    REQUIRE(orcli::save_provider(dir.state(), "c/d:free", {"x"}).has_value());

    const auto loaded = orcli::load_providers(dir.state());
    REQUIRE(loaded.has_value());
    CHECK(loaded->size() == 2);
    CHECK(loaded->at("a/b").tags == std::vector<std::string>{"three"});
    CHECK(loaded->at("a/b").saved.ends_with("Z"));

    const auto removed = orcli::remove_provider(dir.state(), "a/b");
    REQUIRE(removed.has_value());
    REQUIRE(removed->has_value());
    CHECK((*removed)->tags == std::vector<std::string>{"three"});
    const auto again = orcli::remove_provider(dir.state(), "a/b");
    REQUIRE(again.has_value());
    CHECK_FALSE(again->has_value());
    CHECK(orcli::load_providers(dir.state()).value().size() == 1);
}

TEST_CASE("a malformed saved-provider file is a configuration error naming the entry") {
    for (const std::string_view text :
         {R"(not json)", R"([])", R"({"providers": []})",
          R"({"providers": {"a/b": {"tags": [], "saved": "t"}}})",
          R"({"providers": {"a/b": {"tags": [""], "saved": "t"}}})",
          R"({"providers": {"a/b": {"tags": [1], "saved": "t"}}})",
          R"({"providers": {"a/b": {"tags": ["x"]}}})", R"({"providers": {"a/b": "x"}})"}) {
        CAPTURE(text);
        const TempDir dir;
        const fs::path file = dir.state() / "providers.json";
        write_private(file, text);
        const auto loaded = orcli::load_providers(dir.state());
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error().code == Exit::config);
        CHECK(loaded.error().message.contains(file.string()));
        CHECK(loaded.error().message.contains("--any-provider"));
        const auto saved = orcli::save_provider(dir.state(), "c/d", {"x"});
        REQUIRE_FALSE(saved.has_value());
        CHECK(read_all(file) == text);
    }
    const TempDir dir;
    write_private(dir.state() / "providers.json",
                  R"({"providers": {"a/b": {"tags": [], "saved": "t"}}})");
    CHECK(orcli::load_providers(dir.state()).error().message.contains("(model a/b)"));
}

TEST_CASE("groups are edited under the lock and a failed edit writes nothing") {
    const TempDir dir;
    CHECK(orcli::load_groups(dir.state()).value().empty());
    const auto created = orcli::update_groups(dir.state(), [](Groups& groups) -> Result<void> {
        groups["coding"] = Group{.members = {"a/b", "c/d:tag"}, .saved = "t"};
        return {};
    });
    REQUIRE(created.has_value());
    const fs::path file = dir.state() / "groups.json";
    const std::string before = read_all(file);

    const auto refused = orcli::update_groups(dir.state(), [](Groups& groups) -> Result<void> {
        groups.at("coding").members.emplace_back("e/f");
        return orcli::fail(Exit::usage, "no");
    });
    REQUIRE_FALSE(refused.has_value());
    CHECK(read_all(file) == before);
    const auto loaded = orcli::load_groups(dir.state());
    REQUIRE(loaded.has_value());
    CHECK(loaded->at("coding").members == std::vector<std::string>{"a/b", "c/d:tag"});
}

TEST_CASE("a malformed group file is a configuration error") {
    for (const std::string_view text :
         {R"({"groups": {"g": {"members": [], "saved": "t"}}})",
          R"({"groups": {"g": {"members": ["a","b","c","d","e","f","g","h","i"], "saved": "t"}}})",
          R"({"groups": {"g": {"members": ["a"]}}})", R"({"groups": 1})"}) {
        CAPTURE(text);
        const TempDir dir;
        write_private(dir.state() / "groups.json", text);
        const auto loaded = orcli::load_groups(dir.state());
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error().code == Exit::config);
        CHECK(loaded.error().message.contains("groups.json is corrupt"));
    }
}

TEST_CASE("a group-session marker lives apart from the session files") {
    const TempDir dir;
    CHECK_FALSE(orcli::group_session_started(dir.state(), "review").value());
    REQUIRE(orcli::mark_group_session(dir.state(), "review", {"a/b", "c/d"}, "coding").has_value());
    CHECK(orcli::group_session_started(dir.state(), "review").value());
    CHECK(fs::exists(dir.state() / "group-sessions" / "review.json"));
    CHECK_FALSE(fs::exists(dir.state() / "sessions" / "review.group.json"));
    CHECK(orcli::group_session_started(dir.state(), "a/b").error().code == Exit::usage);
}
