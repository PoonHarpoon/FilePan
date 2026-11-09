// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "filepan/runtime/platform.hpp"

// Saved secrets (SFTP passwords, private keys and key passphrases). The settings file never holds
// one as entered: each is sealed by the platform's key store (Platform::protectSecret, an AES-GCM
// key in the Android Keystore) and written as the sealed blob in base64. A secret the platform
// can't seal isn't saved at all: a password or passphrase is asked for when connecting instead,
// and a private key has to be given again.
namespace filepan::app {

class SecretCodec {
public:
    virtual ~SecretCodec() = default;
    // A text-safe sealed form of secret, or nullopt if it can't be sealed.
    virtual std::optional<std::string> protect(const std::string &secret) = 0;
    // The secret back, or nullopt (not ours, tampered with, or the key is gone).
    virtual std::optional<std::string> unprotect(std::string_view blob) = 0;
};

// Platform::protectSecret's blobs in base64. It remembers what it sealed and opened, so saving
// the settings again doesn't reseal every secret (each seal has a fresh IV, so the blobs would
// differ every time). UI thread only, like the Platform.
class PlatformSecretCodec final : public SecretCodec {
public:
    explicit PlatformSecretCodec(runtime::Platform &platform) : platform_(platform) {}

    std::optional<std::string> protect(const std::string &secret) override;
    std::optional<std::string> unprotect(std::string_view blob) override;

private:
    runtime::Platform &platform_;
    std::unordered_map<std::string, std::string> sealed_;  // Secret -> blob.
};

}  // namespace filepan::app
