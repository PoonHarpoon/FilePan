// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "filepan/app/settings.hpp"
#include "filepan/error.hpp"

// Settings backups: a file the user saves anywhere and restores on this or another device.
//
//   filepan-backup 1
//   backup-kdf pbkdf2-sha256 <iterations> <salt>     with a password only
//   backup-check <sealed>                            with a password only
//   known-host <line>                                each known_hosts line, percent-encoded
//   filepan-settings 1                               the settings file's own format (settings.hpp)
//   ...
//
// The settings part leaves out the open tabs, which belong to the device. Secrets (SFTP
// passwords, private keys and passphrases) go in only with a password: each "secret" line is
// sealed with AES-256-GCM under a key derived from it with PBKDF2-HMAC-SHA256, and backup-check
// seals a known text so a wrong password shows at once. Without a password there are no secret
// lines; the servers come back without them. Blobs are base64 of nonce (12) || ciphertext ||
// tag (16).
namespace filepan::app {

inline constexpr int kBackupVersion = 1;
// OWASP's figure for PBKDF2-HMAC-SHA256 (2023). A backup with fewer than kBackupMinIterations or
// more than kBackupMaxIterations isn't opened: it'd be weak, or stall the device.
inline constexpr std::uint32_t kBackupIterations = 600'000;
inline constexpr std::uint32_t kBackupMinIterations = 1'000;
inline constexpr std::uint32_t kBackupMaxIterations = 10'000'000;
// A suggested file name's extension.
inline constexpr const char *kBackupExtension = ".filepan";

using BackupKey = std::array<std::uint8_t, 32>;

// PBKDF2-HMAC-SHA256 of password, 32 bytes.
Result<BackupKey> deriveBackupKey(std::string_view password, std::span<const std::uint8_t> salt,
                                  std::uint32_t iterations);

struct BackupOptions {
    // With one, secrets go in, sealed; without, they're left out.
    std::optional<std::string> password;
    std::uint32_t iterations = kBackupIterations;
};

// A backup of settings and the known_hosts lines. Slow with a password (the key derivation):
// call it off the UI thread. Fails only if there's no randomness to be had.
Result<std::string> writeBackup(const Settings &settings, const std::vector<std::string> &knownHosts,
                                const BackupOptions &options = {});

struct BackupFile {
    // Secrets are in, and a password opens them.
    bool encrypted = false;
    std::vector<std::uint8_t> salt;
    std::uint32_t iterations = 0;
    std::string check;
    std::vector<std::string> knownHosts;
    // From the "filepan-settings" line on.
    std::string settingsText;
};

// EINVAL if it isn't a backup, ENOTSUP if it's from a newer FilePan.
Result<BackupFile> readBackup(std::string_view text);

struct RestoredBackup {
    Settings settings;
    std::vector<std::string> knownHosts;
};

// The backup's settings. With the password, its secrets too (EACCES if it's wrong); without one,
// the servers come back without them. Slow with a password: off the UI thread.
Result<RestoredBackup> openBackup(const BackupFile &file, std::optional<std::string_view> password = std::nullopt);

}  // namespace filepan::app
