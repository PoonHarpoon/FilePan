// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/backup.hpp"

#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/platform_util.h>
#include <sys/random.h>

#include <cerrno>
#include <charconv>

#include "filepan/app/format.hpp"
#include "filepan/app/secrets.hpp"
#include "filepan/base64.hpp"

namespace filepan::app {

namespace {

constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kNonceBytes = 12;
constexpr std::size_t kTagBytes = 16;
// What backup-check seals.
constexpr std::string_view kCheckText = "filepan-backup";
// Associated data, so a secret's blob can't pass for the check or the other way round.
constexpr std::string_view kSecretContext = "filepan-backup secret";
constexpr std::string_view kCheckContext = "filepan-backup check";

bool randomBytes(std::span<std::uint8_t> out) {
    std::size_t filled = 0;
    while (filled < out.size()) {
        const ssize_t got = ::getrandom(out.data() + filled, out.size() - filled, 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        filled += static_cast<std::size_t>(got);
    }
    return true;
}

std::span<const unsigned char> bytesOf(std::string_view text) {
    return {reinterpret_cast<const unsigned char *>(text.data()), text.size()};
}

// AES-256-GCM under one key, as the blobs above.
class Sealer {
public:
    explicit Sealer(const BackupKey &key) {
        mbedtls_gcm_init(&gcm_);
        ok_ = mbedtls_gcm_setkey(&gcm_, MBEDTLS_CIPHER_ID_AES, key.data(), 256) == 0;
    }
    ~Sealer() { mbedtls_gcm_free(&gcm_); }
    Sealer(const Sealer &) = delete;
    Sealer &operator=(const Sealer &) = delete;

    std::optional<std::string> seal(std::string_view plain, std::string_view context) {
        std::vector<std::uint8_t> blob(kNonceBytes + plain.size() + kTagBytes);
        if (!ok_ || !randomBytes({blob.data(), kNonceBytes})) {
            return std::nullopt;
        }
        const auto ad = bytesOf(context);
        const auto in = bytesOf(plain);
        const int failed = mbedtls_gcm_crypt_and_tag(&gcm_, MBEDTLS_GCM_ENCRYPT, in.size(), blob.data(), kNonceBytes,
                                                     ad.data(), ad.size(), in.data(), blob.data() + kNonceBytes,
                                                     kTagBytes, blob.data() + kNonceBytes + in.size());
        if (failed != 0) {
            return std::nullopt;
        }
        return base64Encode(blob);
    }

    std::optional<std::string> open(std::string_view sealed, std::string_view context) {
        auto blob = base64Decode(sealed);
        if (!ok_ || !blob || blob->size() < kNonceBytes + kTagBytes) {
            return std::nullopt;
        }
        const std::size_t length = blob->size() - kNonceBytes - kTagBytes;
        std::string plain(length, '\0');
        const auto ad = bytesOf(context);
        const int failed = mbedtls_gcm_auth_decrypt(
                &gcm_, length, blob->data(), kNonceBytes, ad.data(), ad.size(), blob->data() + kNonceBytes + length,
                kTagBytes, blob->data() + kNonceBytes, reinterpret_cast<unsigned char *>(plain.data()));
        if (failed != 0) {
            mbedtls_platform_zeroize(plain.data(), plain.size());
            return std::nullopt;
        }
        return plain;
    }

private:
    mbedtls_gcm_context gcm_;
    bool ok_ = false;
};

// The settings format's SecretCodec, sealing with the backup's key; and the check.
class PasswordSecretCodec final : public SecretCodec {
public:
    explicit PasswordSecretCodec(const BackupKey &key) : sealer_(key) {}
    std::optional<std::string> protect(const std::string &secret) override {
        return sealer_.seal(secret, kSecretContext);
    }
    std::optional<std::string> unprotect(std::string_view blob) override { return sealer_.open(blob, kSecretContext); }
    std::optional<std::string> sealCheck() { return sealer_.seal(kCheckText, kCheckContext); }
    bool opensCheck(std::string_view sealed) { return sealer_.open(sealed, kCheckContext) == kCheckText; }

private:
    Sealer sealer_;
};

std::vector<std::string_view> fields(std::string_view line) {
    std::vector<std::string_view> out;
    while (!line.empty()) {
        const std::size_t space = line.find(' ');
        if (space != 0) {
            out.push_back(line.substr(0, space));
        }
        if (space == std::string_view::npos) {
            break;
        }
        line.remove_prefix(space + 1);
    }
    return out;
}

std::optional<std::uint32_t> parseCount(std::string_view text) {
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

Result<BackupKey> deriveBackupKey(std::string_view password, std::span<const std::uint8_t> salt,
                                  std::uint32_t iterations) {
    BackupKey key{};
    const auto pass = bytesOf(password);
    if (mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, pass.data(), pass.size(), salt.data(), salt.size(),
                                      iterations, static_cast<std::uint32_t>(key.size()), key.data()) != 0) {
        return fail(EINVAL, "Couldn't derive the backup key");
    }
    return key;
}


Result<std::string> writeBackup(const Settings &settings, const std::vector<std::string> &knownHosts,
                                const BackupOptions &options) {
    Settings saved = settings;
    // The open tabs belong to the device, and so do the grants to document trees.
    saved.tabs.clear();
    saved.activeTab = 0;
    saved.trees.clear();
    std::string out = "filepan-backup " + std::to_string(kBackupVersion) + '\n';
    std::optional<PasswordSecretCodec> codec;
    if (options.password) {
        std::array<std::uint8_t, kSaltBytes> salt{};
        if (!randomBytes(salt)) {
            return fail(EIO, "No randomness for the backup's key");
        }
        auto key = deriveBackupKey(*options.password, salt, options.iterations);
        if (!key) {
            return std::unexpected(key.error());
        }
        codec.emplace(*key);
        mbedtls_platform_zeroize(key->data(), key->size());
        auto check = codec->sealCheck();
        if (!check) {
            return fail(EIO, "Couldn't seal the backup");
        }
        out += "backup-kdf pbkdf2-sha256 " + std::to_string(options.iterations) + ' ' + base64Encode(salt) + '\n';
        out += "backup-check " + *check + '\n';
    }
    for (const std::string &line : knownHosts) {
        if (!line.empty()) {
            out += "known-host " + percentEncode(line) + '\n';
        }
    }
    out += serializeSettings(saved, codec ? &*codec : nullptr);
    return out;
}

Result<BackupFile> readBackup(std::string_view text) {
    BackupFile file;
    bool sawHeader = false;
    while (!text.empty()) {
        const std::size_t newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        if (line.starts_with("filepan-settings")) {
            if (!sawHeader) {
                break;
            }
            file.settingsText = std::string(text);
            return file;
        }
        text = newline == std::string_view::npos ? std::string_view() : text.substr(newline + 1);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const auto f = fields(line);
        if (f.empty()) {
            continue;
        }
        if (!sawHeader) {
            if (f.size() != 2 || f[0] != "filepan-backup") {
                break;
            }
            const auto version = parseCount(f[1]);
            if (!version) {
                break;
            }
            if (*version > static_cast<std::uint32_t>(kBackupVersion)) {
                return fail(ENOTSUP, "This backup is from a newer FilePan");
            }
            sawHeader = true;
        } else if (f[0] == "backup-kdf" && f.size() == 4) {
            if (f[1] != "pbkdf2-sha256") {
                return fail(ENOTSUP, "This backup is from a newer FilePan");
            }
            const auto iterations = parseCount(f[2]);
            auto salt = base64Decode(f[3]);
            if (!iterations || *iterations < kBackupMinIterations || *iterations > kBackupMaxIterations || !salt ||
                salt->empty()) {
                return fail(EINVAL, "This backup is damaged");
            }
            file.encrypted = true;
            file.iterations = *iterations;
            file.salt = std::move(*salt);
        } else if (f[0] == "backup-check" && f.size() == 2) {
            file.check = std::string(f[1]);
        } else if (f[0] == "known-host" && f.size() == 2) {
            if (auto decoded = percentDecode(f[1])) {
                file.knownHosts.push_back(std::move(*decoded));
            }
        }
        // Unknown lines are skipped, as in the settings.
    }
    return fail(EINVAL, "This isn't a FilePan backup");
}

Result<RestoredBackup> openBackup(const BackupFile &file, std::optional<std::string_view> password) {
    std::optional<PasswordSecretCodec> codec;
    if (file.encrypted && password) {
        auto key = deriveBackupKey(*password, file.salt, file.iterations);
        if (!key) {
            return std::unexpected(key.error());
        }
        codec.emplace(*key);
        mbedtls_platform_zeroize(key->data(), key->size());
        if (!codec->opensCheck(file.check)) {
            return fail(EACCES, "Wrong password");
        }
    }
    auto settings = parseSettings(file.settingsText, codec ? &*codec : nullptr);
    if (!settings) {
        return std::unexpected(settings.error());
    }
    // Restored on another device, or after a reinstall: the open tabs stay as they are.
    settings->tabs.clear();
    settings->activeTab = 0;
    return RestoredBackup{std::move(*settings), file.knownHosts};
}

}  // namespace filepan::app
