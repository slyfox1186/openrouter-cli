#ifndef ORCLI_CREDENTIALS_HPP
#define ORCLI_CREDENTIALS_HPP

#include "common.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace orcli {

enum class KeyKind : std::uint8_t {
    inference,  // a normal API key: chat completions, /key, /generation
    management, // a Management key: /keys, /credits, /activity; cannot call completions
};

// Owns plaintext key material and clears it (OPENSSL_cleanse) whenever it is released, so no
// key outlives its use in freed heap memory. Not copyable; clone() makes an explicit copy.
class SecretString {
public:
    SecretString() = default;
    explicit SecretString(std::string value);
    SecretString(const SecretString&) = delete;
    SecretString& operator=(const SecretString&) = delete;
    SecretString(SecretString&& other) noexcept;
    SecretString& operator=(SecretString&& other) noexcept;
    ~SecretString();

    [[nodiscard]] std::string_view view() const { return value_; }
    [[nodiscard]] bool empty() const { return value_.empty(); }
    [[nodiscard]] SecretString clone() const;

private:
    std::string value_;
};

// Overwrites the bytes of a std::string that held key material, then empties it.
void cleanse(std::string& text);

inline constexpr std::chrono::seconds default_creds_timeout{30};

// The program used for encryption: systemd-creds, or a fake in test builds.
struct CredsTool {
    std::filesystem::path program;
    std::chrono::seconds timeout{default_creds_timeout};
};

// Finds `systemd-creds` on PATH and returns its absolute path.
// Exit::config when no usable program exists.
Result<CredsTool> find_creds_tool();

// "openrouter-cli-api-key" / "openrouter-cli-management-key": embedded with --name so a
// credential file swapped for the other kind fails to decrypt.
std::string_view credential_name(KeyKind kind);

// state_dir/credentials/api-key.cred or management-key.cred.
std::filesystem::path credential_file(const std::filesystem::path& state_dir, KeyKind kind);

// `systemd-creds --user --no-ask-password --name=N encrypt - -`: plaintext on stdin,
// ciphertext from stdout. Exit::config on spawn failure, non-zero exit, timeout or oversize
// output; messages are our own text plus the exit status (child stderr is discarded).
Result<std::string> encrypt_key(const CredsTool& tool, KeyKind kind, const SecretString& key);

// `systemd-creds --user --no-ask-password --newline=no --name=N decrypt - -`.
Result<SecretString> decrypt_key(const CredsTool& tool, KeyKind kind, std::string_view ciphertext);

// False only when no credential file exists. Exit::config when the path is a symlink, directory
// or anything else that is not a regular file, or when it cannot be inspected (for example a
// permission error), so a broken state directory is never mistaken for "not stored".
Result<bool> key_stored(const std::filesystem::path& state_dir, KeyKind kind);

// Reads and decrypts the stored key. Exit::config when none is stored (the message names
// `openrouter-cli login` or `auth set`), when the file fails check_private_file, or when
// decryption fails.
Result<SecretString> load_key(const CredsTool& tool, const std::filesystem::path& state_dir,
                              KeyKind kind);

struct StoreOutcome {
    // False when the new file was renamed into place but the directory fsync failed: the key
    // is stored and in use, but its survival across a power loss is unconfirmed.
    bool durable = true;
};

using VerifyKey = std::function<Result<void>(const SecretString&)>;

// Under state_dir/locks/credentials.lock: Exit::refused when a key of this kind is stored and
// replace is false; encrypts; decrypts the ciphertext again and compares; calls verify with
// the plaintext; only then replaces the credential file (fsutil::atomic_replace). Every error
// before the rename leaves any existing credential file untouched.
Result<StoreOutcome> store_key(const CredsTool& tool, const std::filesystem::path& state_dir,
                               KeyKind kind, const SecretString& key, bool replace,
                               const VerifyKey& verify);

// Deletes the Management key and, when include_inference is true, the API key, all under one
// acquisition of the credentials lock. Returns the kinds actually removed (empty when none was
// stored; a missing credentials directory creates nothing). Never decrypts and never needs the
// credential tool. Exit::config when the directory cannot be inspected or a file cannot be
// removed; the message then also names any key already removed.
Result<std::vector<KeyKind>> remove_keys(const std::filesystem::path& state_dir,
                                         bool include_inference);

} // namespace orcli

#endif // ORCLI_CREDENTIALS_HPP
