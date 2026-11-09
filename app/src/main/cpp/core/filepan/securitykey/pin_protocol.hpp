// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "filepan/securitykey/cbor.hpp"
#include "filepan/securitykey/transport.hpp"

// CTAP2's PIN/UV auth protocols, which protect the PIN and the token it unlocks, and the few
// crypto primitives they rest on.
namespace filepan::securitykey {

Bytes sha256(std::span<const std::uint8_t> data);
Bytes hmacSha256(std::span<const std::uint8_t> key, std::span<const std::uint8_t> message);
// From the kernel's random source.
void fillRandom(std::span<std::uint8_t> out);

// A P-256 key pair for ECDH: the private scalar and the public point's coordinates, 32 bytes
// each, big-endian.
struct P256KeyPair {
    Bytes privateKey;
    Bytes x;
    Bytes y;
};

P256KeyPair generateP256KeyPair();
// The x coordinate of the shared point, or nullopt if the peer's point isn't on the curve.
std::optional<Bytes> p256Ecdh(std::span<const std::uint8_t> privateKey,
                              std::span<const std::uint8_t> peerX,
                              std::span<const std::uint8_t> peerY);

// A P-256 key as COSE_Key for ECDH-ES+HKDF-256, as CTAP2 uses for key agreement.
CborValue encodeCoseKey(std::span<const std::uint8_t> x, std::span<const std::uint8_t> y);
// The coordinates, or nullopt for anything but a P-256 EC2 key.
std::optional<std::pair<Bytes, Bytes>> decodeCoseKey(const CborValue &key);

class PinUvAuthProtocol {
public:
    static const PinUvAuthProtocol &one();
    static const PinUvAuthProtocol &two();

    struct Encapsulation {
        CborValue platformKey;
        Bytes sharedSecret;
    };

    [[nodiscard]] int version() const { return version_; }

    // Agrees on a shared secret with the authenticator's COSE key.
    [[nodiscard]] KeyResult<Encapsulation> encapsulate(const CborValue &peerKey) const;

    [[nodiscard]] Bytes kdf(std::span<const std::uint8_t> z) const;
    [[nodiscard]] Bytes encrypt(std::span<const std::uint8_t> key,
                                std::span<const std::uint8_t> plaintext) const;
    [[nodiscard]] KeyResult<Bytes> decrypt(std::span<const std::uint8_t> key,
                                           std::span<const std::uint8_t> ciphertext) const;
    [[nodiscard]] Bytes authenticate(std::span<const std::uint8_t> key,
                                     std::span<const std::uint8_t> message) const;

private:
    explicit constexpr PinUvAuthProtocol(int version) : version_(version) {}

    int version_;
};

}  // namespace filepan::securitykey
