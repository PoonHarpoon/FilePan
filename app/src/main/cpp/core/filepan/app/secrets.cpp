// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/secrets.hpp"

#include "filepan/base64.hpp"

namespace filepan::app {

std::optional<std::string> PlatformSecretCodec::protect(const std::string &secret) {
    if (const auto found = sealed_.find(secret); found != sealed_.end()) {
        return found->second;
    }
    const auto blob = platform_.protectSecret(
            {reinterpret_cast<const std::uint8_t *>(secret.data()), secret.size()});
    if (!blob) {
        return std::nullopt;
    }
    std::string text = base64Encode(*blob);
    sealed_.emplace(secret, text);
    return text;
}

std::optional<std::string> PlatformSecretCodec::unprotect(std::string_view blob) {
    const auto bytes = base64Decode(blob);
    if (!bytes) {
        return std::nullopt;
    }
    const auto plain = platform_.unprotectSecret(*bytes);
    if (!plain) {
        return std::nullopt;
    }
    std::string secret(plain->begin(), plain->end());
    // Saving it again writes the same blob.
    sealed_.emplace(secret, std::string(blob));
    return secret;
}

}  // namespace filepan::app
