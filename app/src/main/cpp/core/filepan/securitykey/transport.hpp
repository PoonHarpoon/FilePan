// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <utility>

#include "filepan/securitykey/cbor.hpp"

// A connection to a FIDO authenticator, over USB or NFC, and how talking to one fails.
namespace filepan::securitykey {

// CTAP2 status codes the client looks at.
namespace ctap_status {
inline constexpr int kInvalidCommand = 0x01;
inline constexpr int kInvalidParameter = 0x02;
inline constexpr int kOperationDenied = 0x27;
inline constexpr int kInvalidOption = 0x2C;
inline constexpr int kNoCredentials = 0x2E;
inline constexpr int kUserActionTimeout = 0x2F;
inline constexpr int kPinInvalid = 0x31;
inline constexpr int kPinBlocked = 0x32;
inline constexpr int kPinAuthInvalid = 0x33;
inline constexpr int kPinAuthBlocked = 0x34;
inline constexpr int kPinNotSet = 0x35;
inline constexpr int kPuatRequired = 0x36;
inline constexpr int kUvBlocked = 0x3C;
inline constexpr int kUvInvalid = 0x3F;
inline constexpr int kUnauthorizedPermission = 0x40;
}  // namespace ctap_status

struct KeyError {
    enum class Kind : std::uint8_t {
        kIo,           // The exchange failed: the key left, or answered nonsense.
        kClosed,       // The transport was closed, e.g. cancelled from the prompt.
        kCtap,         // The key answered with a CTAP2 error status.
        // What the user can act on, as SecurityKeySigning reports it.
        kCancelled,    // The user gave up: no PIN, no touch, or the key denied it.
        kWrongKey,     // The key doesn't hold this SSH key.
        kPinRequired,  // User verification is required, but the key has no PIN.
        kPinBlocked,
        kUnsupported,  // E.g. Ed25519 or user verification over U2F.
        kPinWrong,     // An OpenPGP card refused the PIN; status is the tries left.
    };

    Kind kind = Kind::kIo;
    int status = 0;  // For kCtap and kPinWrong.
    std::string message;
};

template <typename T>
using KeyResult = std::expected<T, KeyError>;

inline std::unexpected<KeyError> keyFailure(KeyError::Kind kind, std::string message = {}) {
    return std::unexpected(KeyError{kind, 0, std::move(message)});
}

inline std::unexpected<KeyError> ctapFailure(int status) {
    return std::unexpected(KeyError{KeyError::Kind::kCtap, status, {}});
}

struct ApduResponse {
    Bytes data;
    int status = 0;  // SW1 SW2.
};

// A security key or card the user presented, open and talking. CtapTransport is the FIDO applet;
// OpenPgpCard the OpenPGP one.
class PresentedKey {
public:
    virtual ~PresentedKey() = default;

    // Whether touching or tapping the key has already happened (NFC).
    [[nodiscard]] virtual bool isUserPresent() const = 0;
    // Stops an exchange in progress, from any thread; everything after fails with kClosed.
    virtual void close() = 0;
    // Whether close() was called.
    [[nodiscard]] virtual bool isClosed() const = 0;

    // Called on the exchanging thread while the key waits for a touch.
    std::function<void()> onUserPresenceNeeded;
};

class CtapTransport : public PresentedKey {
public:
    // Whether the authenticator may speak CTAP2; if false it only speaks U2F. Over NFC this
    // can't be known up front, so CTAP2 is tried first.
    [[nodiscard]] virtual bool mayHaveCtap2() const = 0;
    // Sends a CTAP2 command; the response starts with its status byte.
    virtual KeyResult<Bytes> sendCbor(int command, std::span<const std::uint8_t> data) = 0;
    // Sends a U2F (CTAP1) request APDU.
    virtual KeyResult<ApduResponse> sendApdu(int ins, int p1, int p2,
                                             std::span<const std::uint8_t> data) = 0;
};

}  // namespace filepan::securitykey
