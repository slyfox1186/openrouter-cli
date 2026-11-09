#include "session.hpp"

#include "common.hpp"
#include "fsutil.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <random>
#include <string>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;
using orcli::Exit;
using orcli::Message;
using orcli::Session;

namespace {

constexpr std::size_t uuid_length = 32;
constexpr std::size_t longest_name = 64;
constexpr fs::perms private_file_perms = fs::perms::owner_read | fs::perms::owner_write;

class TempDir {
public:
    TempDir() {
        std::random_device device;
        std::uniform_int_distribution<std::uint32_t> distribution;
        path_ = fs::temp_directory_path() / std::format("orcli-session-{:08x}{:08x}",
                                                        distribution(device), distribution(device));
        fs::create_directory(path_);
        fs::permissions(path_, fs::perms::owner_all);
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

bool lowercase_hex(char letter) {
    return (letter >= '0' && letter <= '9') || (letter >= 'a' && letter <= 'f');
}

} // namespace

TEST_CASE("valid_session_name accepts the documented character set only") {
    CHECK(orcli::valid_session_name("a"));
    CHECK(orcli::valid_session_name("review-1.2_final"));
    CHECK(orcli::valid_session_name("..."));
    CHECK(orcli::valid_session_name(std::string(longest_name, 'n')));
    CHECK_FALSE(orcli::valid_session_name(""));
    CHECK_FALSE(orcli::valid_session_name("."));
    CHECK_FALSE(orcli::valid_session_name(".."));
    CHECK_FALSE(orcli::valid_session_name(std::string(longest_name + 1, 'n')));
    CHECK_FALSE(orcli::valid_session_name("a/b"));
    CHECK_FALSE(orcli::valid_session_name("a b"));
    CHECK_FALSE(orcli::valid_session_name("caf\xc3\xa9"));
}

TEST_CASE("Session::open rejects an invalid name") {
    const TempDir temp;
    const auto opened = Session::open(temp.state(), "../escape");
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().code == Exit::usage);
}

TEST_CASE("a session in use by another holder is refused") {
    const TempDir temp;
    const auto first = Session::open(temp.state(), "review");
    REQUIRE(first.has_value());
    const auto second = Session::open(temp.state(), "review");
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code == Exit::refused);
    CHECK(second.error().message.find("review") != std::string::npos);
    CHECK(Session::open(temp.state(), "other").has_value());
}

TEST_CASE("create, commit and reopen round-trip the session") {
    const TempDir temp;
    std::string uuid;
    {
        auto session = Session::open(temp.state(), "review");
        REQUIRE(session.has_value());
        CHECK_FALSE(session->exists());
        REQUIRE(session->create("vendor/model-a", "Be terse.").has_value());
        uuid = session->data().session_uuid;
        CHECK(uuid.size() == uuid_length);
        CHECK(std::ranges::all_of(uuid, lowercase_hex));
        REQUIRE(session
                    ->commit(Message{.role = "user", .content = "question"},
                             Message{.role = "assistant", .content = "answer"})
                    .has_value());
        CHECK(session->exists());
    }
    const fs::path file = temp.state() / "sessions" / "review.json";
    CHECK(fs::symlink_status(file).permissions() == private_file_perms);

    auto reopened = Session::open(temp.state(), "review");
    REQUIRE(reopened.has_value());
    CHECK(reopened->exists());
    const orcli::SessionData& data = reopened->data();
    CHECK(data.model_id == "vendor/model-a");
    CHECK(data.system_prompt == "Be terse.");
    CHECK(data.session_uuid == uuid);
    REQUIRE(data.history.size() == 2);
    CHECK(data.history.at(0).role == "user");
    CHECK(data.history.at(0).content == "question");
    CHECK(data.history.at(1).role == "assistant");
    CHECK(data.history.at(1).content == "answer");

    const auto recreated = reopened->create("vendor/model-b", "Other.");
    REQUIRE_FALSE(recreated.has_value());
    CHECK(recreated.error().code == Exit::usage);
    CHECK(reopened->data().model_id == "vendor/model-a");
}

TEST_CASE("commit appends exactly the exchange it is given") {
    const TempDir temp;
    auto session = Session::open(temp.state(), "chat");
    REQUIRE(session.has_value());
    REQUIRE(session->create("vendor/model", "").has_value());
    REQUIRE(session
                ->commit(Message{.role = "user", .content = "one"},
                         Message{.role = "assistant", .content = "reply one"})
                .has_value());
    REQUIRE(session
                ->commit(Message{.role = "user", .content = "two"},
                         Message{.role = "assistant", .content = "reply two"})
                .has_value());
    const auto& history = session->data().history;
    REQUIRE(history.size() == 4);
    CHECK(history.at(0).content == "one");
    CHECK(history.at(1).content == "reply one");
    CHECK(history.at(2).content == "two");
    CHECK(history.at(3).content == "reply two");
}

TEST_CASE("a corrupt session file is reported without echoing its content") {
    const TempDir temp;
    REQUIRE(orcli::ensure_private_dir(temp.state() / "sessions").has_value());
    const fs::path file = temp.state() / "sessions" / "broken.json";
    REQUIRE(orcli::atomic_write(file, "{\"v\":1,\"model_id\":\"secret prompt text").has_value());
    const auto opened = Session::open(temp.state(), "broken");
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().code == Exit::config);
    CHECK(opened.error().message.find(file.string()) != std::string::npos);
    CHECK(opened.error().message.find("secret") == std::string::npos);
}

TEST_CASE("a session file with the wrong shape is corrupt") {
    const TempDir temp;
    REQUIRE(orcli::ensure_private_dir(temp.state() / "sessions").has_value());
    const fs::path file = temp.state() / "sessions" / "shape.json";
    REQUIRE(orcli::atomic_write(file, R"({"v":1,"model_id":"m","system_prompt":"s",)"
                                      R"("session_uuid":"u","history":[{"role":"user"}]})")
                .has_value());
    const auto opened = Session::open(temp.state(), "shape");
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().code == Exit::config);
}

TEST_CASE("a session holding invalid UTF-8 is saved and reopened") {
    const TempDir temp;
    {
        auto session = Session::open(temp.state(), "bytes");
        REQUIRE(session.has_value());
        REQUIRE(session->create("vendor/model", "prompt \xff").has_value());
        REQUIRE(session
                    ->commit(Message{.role = "user", .content = "from a file \xfe\xff"},
                             Message{.role = "assistant", .content = "reply"})
                    .has_value());
    }
    const auto reopened = Session::open(temp.state(), "bytes");
    REQUIRE(reopened.has_value());
    CHECK(reopened->exists());
    REQUIRE(reopened->data().history.size() == 2);
    CHECK(reopened->data().history.at(1).content == "reply");
    CHECK(reopened->data().system_prompt.starts_with("prompt "));
}
