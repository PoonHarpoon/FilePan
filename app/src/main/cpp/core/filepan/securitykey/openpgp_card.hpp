// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "filepan/securitykey/iso7816.hpp"
#include "filepan/securitykey/transport.hpp"

// The OpenPGP card applet (OpenPGP Smart Card Application 3.4) on a YubiKey or another card,
// for logging in to SSH with its authentication key as gpg-agent does.
namespace filepan::securitykey {

// What an ssh-agent sign request's flags may ask of an RSA key (draft-miller-ssh-agent §5.3).
namespace agent_flags {
inline constexpr std::uint32_t kRsaSha2_256 = 0x02;
inline constexpr std::uint32_t kRsaSha2_512 = 0x04;
}  // namespace agent_flags

class OpenPgpCard final : public PresentedKey {
public:
    enum class Algorithm : std::uint8_t { kNone, kRsa, kEcdsaP256, kEcdsaP384, kEd25519, kOther };

    // Selects the applet and reads what logging in needs. userPresent: being tapped counts as a
    // touch (NFC). Fails with kIo if the card has no OpenPGP applet.
    static KeyResult<std::unique_ptr<OpenPgpCard>> open(std::unique_ptr<ApduChannel> channel,
                                                        bool userPresent);

    [[nodiscard]] bool isUserPresent() const override { return userPresent_; }
    void close() override { session_.close(); }
    [[nodiscard]] bool isClosed() const override { return session_.isClosed(); }

    // The manufacturer and serial number as twelve hex digits, as in gpg's "cardno:".
    [[nodiscard]] const std::string &serial() const { return serial_; }
    [[nodiscard]] Algorithm algorithm() const { return algorithm_; }
    // PIN (PW1) tries left when the card was opened.
    [[nodiscard]] int pinRetries() const { return pinRetries_; }
    // The authentication key wants a touch for every use (its UIF is on).
    [[nodiscard]] bool touchRequired() const { return touchRequired_; }

    // The authentication key as an SSH public key blob. kUnsupported if the card has none, or
    // one SSH can't use (e.g. a Brainpool curve).
    KeyResult<Bytes> authenticationKey();
    // Unlocks the authentication key. kPinWrong with the tries left, or kPinBlocked.
    KeyResult<void> verifyPin(std::string_view pin);
    // Signs data as ssh-agent would with the authentication key: the SSH signature blob. flags
    // are the agent request's (agent_flags), choosing the hash for RSA.
    KeyResult<Bytes> signSsh(std::span<const std::uint8_t> data, std::uint32_t flags);

private:
    OpenPgpCard(std::unique_ptr<ApduChannel> channel, bool userPresent)
        : session_(std::move(channel)), userPresent_(userPresent) {}

    KeyResult<void> readState();
    KeyResult<Bytes> getData(int tag);

    ApduSession session_;
    bool userPresent_;
    std::string serial_;
    Algorithm algorithm_ = Algorithm::kNone;
    int pinRetries_ = -1;
    bool touchRequired_ = false;
    // From the KDF-DO: the PIN is hashed before VERIFY.
    int kdfHash_ = 0;
    std::uint32_t kdfCount_ = 0;
    Bytes kdfSalt_;
};

// OpenPGP's iterated and salted S2K (RFC 4880 §3.7.1.3) with SHA-256 or SHA-512, as the card's
// KDF-DO asks for PINs: count is the number of bytes hashed. hashAlgorithm is OpenPGP's id (8 or
// 10); empty for another.
Bytes iteratedSaltedS2k(int hashAlgorithm, std::span<const std::uint8_t> salt, std::uint32_t count,
                        std::string_view secret);

// The key's SSH type name ("ssh-ed25519", "ecdsa-sha2-nistp256", "ssh-rsa", ...) from its blob,
// or empty.
std::string sshKeyType(std::span<const std::uint8_t> blob);

}  // namespace filepan::securitykey
