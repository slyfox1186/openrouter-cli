#include "credentials.hpp"

#include "common.hpp"
#include "fsutil.hpp"

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using orcli::Exit;
using orcli::KeyKind;
using orcli::SecretString;

namespace {

constexpr std::chrono::seconds short_timeout{2};
// Generous next to short_timeout: the check is that a hung tool is abandoned, not how quickly.
constexpr std::chrono::seconds hang_limit{10};
constexpr fs::perms private_dir_perms = fs::perms::owner_all;
constexpr fs::perms private_file_perms = fs::perms::owner_read | fs::perms::owner_write;
constexpr std::array<KeyKind, 2> both_kinds{KeyKind::management, KeyKind::inference};
constexpr std::string_view first_key = "sk-or-v1-first-0123456789abcdef0123456789abcdef0123456789";
constexpr std::string_view second_key = "sk-or-v1-second-fedcba9876543210fedcba9876543210fedcba98";

class TempDir {
public:
    TempDir() {
        std::random_device device;
        std::uniform_int_distribution<std::uint32_t> distribution;
        path_ = fs::temp_directory_path() /
                std::format("orcli-creds-{:08x}{:08x}", distribution(device), distribution(device));
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
    [[nodiscard]] fs::path state() const { return path_ / "state"; }

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

orcli::CredsTool fake_tool() {
    return orcli::CredsTool{.program = ORCLI_FAKE_CREDS_TOOL, .timeout = short_timeout};
}

std::string shell_quote(std::string_view value) {
    std::string quoted = "'";
    for (const char letter : value) {
        if (letter == '\'') {
            quoted += "'\\''";
        } else {
            quoted += letter;
        }
    }
    quoted += '\'';
    return quoted;
}

// A wrapper that turns on one FAKE_CREDS_* switch for the fake tool only, so tests never change
// this process's environment.
orcli::CredsTool fake_tool_with(const TempDir& temp, std::string_view setting) {
    const fs::path wrapper = temp.path() / std::format("fake-{}.sh", setting);
    {
        std::ofstream script(wrapper, std::ios::binary);
        script << std::format("#!/bin/sh\n{}=1\nexport {}\nexec {} \"$@\"\n", setting, setting,
                              shell_quote(ORCLI_FAKE_CREDS_TOOL));
    }
    fs::permissions(wrapper, fs::perms::owner_all);
    return orcli::CredsTool{.program = wrapper, .timeout = short_timeout};
}

std::string read_file(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

fs::perms perms_of(const fs::path& path) {
    return fs::symlink_status(path).permissions();
}

orcli::Result<void> accept_key(const SecretString& /*key*/) {
    return {};
}

// Whether key_stored succeeded and gave `expected`.
bool stored_state_is(const fs::path& state, KeyKind kind, bool expected) {
    const auto stored = orcli::key_stored(state, kind);
    return stored.has_value() && *stored == expected;
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

} // namespace

TEST_CASE("encrypt_key and decrypt_key round-trip through the credential tool") {
    const SecretString key{std::string{first_key}};
    const auto ciphertext = orcli::encrypt_key(fake_tool(), KeyKind::inference, key);
    REQUIRE(ciphertext.has_value());
    CHECK(ciphertext->starts_with("FAKECRED:openrouter-cli-api-key:"));
    CHECK_FALSE(contains(*ciphertext, first_key));

    const auto plain = orcli::decrypt_key(fake_tool(), KeyKind::inference, *ciphertext);
    REQUIRE(plain.has_value());
    CHECK(plain->view() == first_key);
}

TEST_CASE("a credential encrypted for one kind does not decrypt as the other") {
    const SecretString key{std::string{first_key}};
    const auto ciphertext = orcli::encrypt_key(fake_tool(), KeyKind::inference, key);
    REQUIRE(ciphertext.has_value());
    const auto plain = orcli::decrypt_key(fake_tool(), KeyKind::management, *ciphertext);
    REQUIRE_FALSE(plain.has_value());
    CHECK(plain.error().code == Exit::config);
    CHECK(plain.error().message == "systemd-creds decrypt failed (exit 1)");
}

TEST_CASE("a failing credential tool reports its exit status") {
    const TempDir temp;
    const SecretString key{std::string{first_key}};
    const auto result =
        orcli::encrypt_key(fake_tool_with(temp, "FAKE_CREDS_FAIL"), KeyKind::inference, key);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Exit::config);
    CHECK(result.error().message == "systemd-creds encrypt failed (exit 3)");
}

TEST_CASE("oversized credential tool output is refused") {
    const TempDir temp;
    const SecretString key{std::string{first_key}};
    const auto result =
        orcli::encrypt_key(fake_tool_with(temp, "FAKE_CREDS_HUGE"), KeyKind::inference, key);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Exit::config);
    CHECK(contains(result.error().message, "more than 64 KiB"));
}

TEST_CASE("a hung credential tool is killed at the timeout") {
    const TempDir temp;
    const SecretString key{std::string{first_key}};
    const auto started = std::chrono::steady_clock::now();
    const auto result =
        orcli::encrypt_key(fake_tool_with(temp, "FAKE_CREDS_HANG"), KeyKind::inference, key);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Exit::config);
    CHECK(result.error().message == "systemd-creds encrypt did not finish within 2 s");
    CHECK(elapsed >= short_timeout);
    CHECK(elapsed < hang_limit);
}

TEST_CASE("a missing credential tool is a configuration error") {
    const TempDir temp;
    const SecretString key{std::string{first_key}};
    const orcli::CredsTool missing{.program = temp.path() / "no-such-tool",
                                   .timeout = short_timeout};
    const auto result = orcli::encrypt_key(missing, KeyKind::inference, key);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Exit::config);
}

TEST_CASE("store_key writes a private credential file that load_key decrypts") {
    const TempDir temp;
    const SecretString key{std::string{first_key}};
    bool verified = false;
    const orcli::VerifyKey verify = [&verified](const SecretString& candidate) {
        verified = candidate.view() == first_key;
        return orcli::Result<void>{};
    };
    const auto stored =
        orcli::store_key(fake_tool(), temp.state(), KeyKind::inference, key, false, verify);
    REQUIRE(stored.has_value());
    CHECK(stored->durable);
    CHECK(verified);

    const fs::path file = orcli::credential_file(temp.state(), KeyKind::inference);
    CHECK(file == temp.state() / "credentials" / "api-key.cred");
    CHECK(stored_state_is(temp.state(), KeyKind::inference, true));
    CHECK(stored_state_is(temp.state(), KeyKind::management, false));
    CHECK(perms_of(file) == private_file_perms);
    CHECK(perms_of(temp.state()) == private_dir_perms);
    CHECK(perms_of(temp.state() / "credentials") == private_dir_perms);
    CHECK(perms_of(temp.state() / "locks") == private_dir_perms);
    CHECK_FALSE(contains(read_file(file), first_key));

    const auto loaded = orcli::load_key(fake_tool(), temp.state(), KeyKind::inference);
    REQUIRE(loaded.has_value());
    CHECK(loaded->view() == first_key);
}

TEST_CASE("store_key refuses to replace a stored key unless asked") {
    const TempDir temp;
    const SecretString first{std::string{first_key}};
    const SecretString second{std::string{second_key}};
    REQUIRE(
        orcli::store_key(fake_tool(), temp.state(), KeyKind::management, first, false, accept_key)
            .has_value());
    const fs::path file = orcli::credential_file(temp.state(), KeyKind::management);
    CHECK(file.filename() == "management-key.cred");
    const std::string before = read_file(file);

    const auto refused =
        orcli::store_key(fake_tool(), temp.state(), KeyKind::management, second, false, accept_key);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == Exit::refused);
    CHECK(refused.error().message ==
          "a Management key is already stored; pass --replace to change it");
    CHECK(read_file(file) == before);

    REQUIRE(
        orcli::store_key(fake_tool(), temp.state(), KeyKind::management, second, true, accept_key)
            .has_value());
    const auto loaded = orcli::load_key(fake_tool(), temp.state(), KeyKind::management);
    REQUIRE(loaded.has_value());
    CHECK(loaded->view() == second_key);
}

TEST_CASE("a verification failure leaves the stored key untouched") {
    const TempDir temp;
    const SecretString first{std::string{first_key}};
    const SecretString second{std::string{second_key}};
    REQUIRE(
        orcli::store_key(fake_tool(), temp.state(), KeyKind::inference, first, false, accept_key)
            .has_value());
    const fs::path file = orcli::credential_file(temp.state(), KeyKind::inference);
    const std::string before = read_file(file);

    const orcli::VerifyKey reject = [](const SecretString& /*key*/) -> orcli::Result<void> {
        return orcli::fail(Exit::api, "the key was rejected");
    };
    const auto result =
        orcli::store_key(fake_tool(), temp.state(), KeyKind::inference, second, true, reject);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Exit::api);
    CHECK(result.error().message == "the key was rejected");
    CHECK(read_file(file) == before);
}

TEST_CASE("a failed round-trip check leaves the stored key untouched and skips verification") {
    const TempDir temp;
    const SecretString first{std::string{first_key}};
    const SecretString second{std::string{second_key}};
    REQUIRE(
        orcli::store_key(fake_tool(), temp.state(), KeyKind::inference, first, false, accept_key)
            .has_value());
    const fs::path file = orcli::credential_file(temp.state(), KeyKind::inference);
    const std::string before = read_file(file);

    bool verify_called = false;
    const orcli::VerifyKey verify = [&verify_called](const SecretString& /*key*/) {
        verify_called = true;
        return orcli::Result<void>{};
    };
    const auto result = orcli::store_key(fake_tool_with(temp, "FAKE_CREDS_CORRUPT"), temp.state(),
                                         KeyKind::inference, second, true, verify);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Exit::config);
    CHECK(contains(result.error().message, "round-trip"));
    CHECK_FALSE(verify_called);
    CHECK(read_file(file) == before);
}

TEST_CASE("load_key without a stored key names the command that stores one") {
    const TempDir temp;
    const auto inference = orcli::load_key(fake_tool(), temp.state(), KeyKind::inference);
    REQUIRE_FALSE(inference.has_value());
    CHECK(inference.error().code == Exit::config);
    CHECK(contains(inference.error().message, "openrouter-cli login"));

    const auto management = orcli::load_key(fake_tool(), temp.state(), KeyKind::management);
    REQUIRE_FALSE(management.has_value());
    CHECK(management.error().code == Exit::config);
    CHECK(contains(management.error().message, "openrouter-cli auth set --management"));
}

TEST_CASE("load_key reports a credential file that does not decrypt") {
    const TempDir temp;
    const fs::path dir = temp.state() / "credentials";
    REQUIRE(orcli::ensure_private_dir(dir).has_value());
    REQUIRE(orcli::atomic_write(dir / "api-key.cred", "not a credential").has_value());
    const auto loaded = orcli::load_key(fake_tool(), temp.state(), KeyKind::inference);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code == Exit::config);
    CHECK(loaded.error().message.starts_with("could not decrypt the stored API key"));
    CHECK(contains(loaded.error().message, "openrouter-cli logout"));
}

TEST_CASE("remove_keys removes the requested keys once") {
    const TempDir temp;
    const auto before_any = orcli::remove_keys(temp.state(), true);
    REQUIRE(before_any.has_value());
    CHECK(before_any->empty());
    CHECK_FALSE(fs::exists(temp.state()));

    const SecretString key{std::string{first_key}};
    for (const KeyKind kind : both_kinds) {
        REQUIRE(
            orcli::store_key(fake_tool(), temp.state(), kind, key, false, accept_key).has_value());
    }
    const auto management_only = orcli::remove_keys(temp.state(), false);
    REQUIRE(management_only.has_value());
    CHECK(*management_only == std::vector<KeyKind>{KeyKind::management});
    CHECK(stored_state_is(temp.state(), KeyKind::management, false));
    CHECK(stored_state_is(temp.state(), KeyKind::inference, true));

    const auto rest = orcli::remove_keys(temp.state(), true);
    REQUIRE(rest.has_value());
    CHECK(*rest == std::vector<KeyKind>{KeyKind::inference});

    const auto again = orcli::remove_keys(temp.state(), true);
    REQUIRE(again.has_value());
    CHECK(again->empty());
}

TEST_CASE("remove_keys works on corrupt credentials without the credential tool") {
    const TempDir temp;
    const fs::path dir = temp.state() / "credentials";
    REQUIRE(orcli::ensure_private_dir(dir).has_value());
    REQUIRE(orcli::atomic_write(dir / "management-key.cred", "garbage").has_value());
    REQUIRE(orcli::atomic_write(dir / "api-key.cred", "garbage").has_value());
    const auto removed = orcli::remove_keys(temp.state(), true);
    REQUIRE(removed.has_value());
    CHECK(*removed == std::vector<KeyKind>{KeyKind::management, KeyKind::inference});
    CHECK_FALSE(fs::exists(dir / "management-key.cred"));
    CHECK_FALSE(fs::exists(dir / "api-key.cred"));
}

TEST_CASE("an inaccessible credentials directory is an error, not an absent key") {
    const TempDir temp;
    const fs::path dir = temp.state() / "credentials";
    REQUIRE(orcli::ensure_private_dir(dir).has_value());
    REQUIRE(orcli::atomic_write(dir / "api-key.cred", "garbage").has_value());
    {
        const RestorePerms restore(dir, private_dir_perms);
        fs::permissions(dir, fs::perms::none);

        const auto stored = orcli::key_stored(temp.state(), KeyKind::inference);
        REQUIRE_FALSE(stored.has_value());
        CHECK(stored.error().code == Exit::config);
        CHECK(stored.error().message.starts_with("cannot inspect"));

        const auto removed = orcli::remove_keys(temp.state(), true);
        REQUIRE_FALSE(removed.has_value());
        CHECK(removed.error().code == Exit::config);
        CHECK(removed.error().message.starts_with("cannot remove"));
    }
    CHECK(fs::exists(dir / "api-key.cred"));
}

TEST_CASE("a credential path that is not a regular file is an error") {
    const TempDir temp;
    const fs::path dir = temp.state() / "credentials";
    REQUIRE(orcli::ensure_private_dir(dir / "api-key.cred" / "inner").has_value());
    REQUIRE(orcli::atomic_write(dir / "real", "x").has_value());
    fs::create_symlink(dir / "real", dir / "management-key.cred");

    for (const KeyKind kind : both_kinds) {
        const auto stored = orcli::key_stored(temp.state(), kind);
        REQUIRE_FALSE(stored.has_value());
        CHECK(stored.error().code == Exit::config);
        CHECK(stored.error().message.ends_with("is not a regular file"));
        const auto loaded = orcli::load_key(fake_tool(), temp.state(), kind);
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error().code == Exit::config);
    }

    const SecretString key{std::string{first_key}};
    const auto refused =
        orcli::store_key(fake_tool(), temp.state(), KeyKind::inference, key, true, accept_key);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == Exit::config);
    CHECK(fs::is_directory(dir / "api-key.cred" / "inner"));
}

TEST_CASE("a failed removal names the key already removed") {
    const TempDir temp;
    const fs::path dir = temp.state() / "credentials";
    REQUIRE(orcli::ensure_private_dir(dir / "api-key.cred" / "inner").has_value());
    REQUIRE(orcli::atomic_write(dir / "management-key.cred", "garbage").has_value());
    const auto removed = orcli::remove_keys(temp.state(), true);
    REQUIRE_FALSE(removed.has_value());
    CHECK(removed.error().code == Exit::config);
    CHECK(removed.error().message.ends_with("; the Management key was removed"));
    CHECK_FALSE(fs::exists(dir / "management-key.cred"));
}

TEST_CASE("a credentials path that is not a directory is an error") {
    const TempDir temp;
    REQUIRE(orcli::ensure_private_dir(temp.state()).has_value());
    REQUIRE(orcli::atomic_write(temp.state() / "credentials", "x").has_value());
    const auto removed = orcli::remove_keys(temp.state(), true);
    REQUIRE_FALSE(removed.has_value());
    CHECK(removed.error().code == Exit::config);
    CHECK(removed.error().message.ends_with("is not a directory"));
}

TEST_CASE("SecretString moves leave the source empty and clone copies") {
    // Array elements, because the moved-from state is exactly what this test reads.
    std::array<SecretString, 2> sources{SecretString{std::string{first_key}},
                                        SecretString{std::string{second_key}}};
    SecretString moved{std::move(sources.at(0))};
    CHECK(moved.view() == first_key);
    CHECK(sources.at(0).empty());

    moved = std::move(sources.at(1));
    CHECK(moved.view() == second_key);
    CHECK(sources.at(1).empty());

    const SecretString copy = moved.clone();
    CHECK(copy.view() == second_key);
    CHECK(moved.view() == second_key);
}

TEST_CASE("credential names differ per kind") {
    CHECK(orcli::credential_name(KeyKind::inference) == "openrouter-cli-api-key");
    CHECK(orcli::credential_name(KeyKind::management) == "openrouter-cli-management-key");
}

TEST_CASE("stored ciphertext is bounded before invoking the credential tool") {
    const TempDir temp;
    const auto directory = temp.state() / "credentials";
    REQUIRE(orcli::ensure_private_dir(directory).has_value());
    const auto path = directory / "api-key.cred";
    REQUIRE(orcli::atomic_write(path, "ciphertext").has_value());
    constexpr std::uintmax_t max_ciphertext_bytes = 64 * 1024;
    fs::resize_file(path, max_ciphertext_bytes + 1);
    const auto loaded = orcli::load_key(fake_tool(), temp.state(), KeyKind::inference);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().message.contains("byte limit"));
}
