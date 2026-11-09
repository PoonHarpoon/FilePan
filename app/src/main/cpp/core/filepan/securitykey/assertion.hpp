// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>

#include "filepan/securitykey/cbor.hpp"
#include "filepan/securitykey/transport.hpp"

// Signs SSH authentication data with a security key the way OpenSSH's middleware does, so that
// keys enrolled with `ssh-keygen -t ecdsa-sk` or `ed25519-sk` work.
namespace filepan::securitykey {

// As libssh's SSH_SK_ECDSA and SSH_SK_ED25519.
inline constexpr std::uint32_t kAlgorithmEcdsa = 0x00;
inline constexpr std::uint32_t kAlgorithmEd25519 = 0x01;
// As libssh's SSH_SK_USER_PRESENCE_REQD and SSH_SK_USER_VERIFICATION_REQD.
inline constexpr std::uint8_t kUserPresenceRequired = 0x01;
inline constexpr std::uint8_t kUserVerificationRequired = 0x04;

struct SignRequest {
    std::uint32_t algorithm = kAlgorithmEcdsa;
    // The client data; its SHA-256 is the client data hash (or U2F challenge).
    std::span<const std::uint8_t> data;
    // The relying party ID, usually "ssh:".
    std::string application;
    std::span<const std::uint8_t> keyHandle;
    std::uint8_t flags = kUserPresenceRequired;
};

struct SignResult {
    // From the authenticator data.
    std::uint8_t flags = 0;
    std::uint32_t counter = 0;
    // DER for ECDSA; the raw 64 bytes for Ed25519.
    Bytes signature;
};

// Asks the user for the security key's PIN, given the attempts left before it locks (-1 if
// unknown). nullopt: the user gave up.
using PinSource = std::function<std::optional<std::string>(int retries)>;

KeyResult<SignResult> signWithSecurityKey(CtapTransport &transport, const SignRequest &request,
                                          const PinSource &pinSource);

}  // namespace filepan::securitykey
