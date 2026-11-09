// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/securitykey/pin_protocol.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <string_view>

#include <mbedtls/aes.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <sys/random.h>

namespace filepan::securitykey {

namespace {

int mbedtlsRandom(void *, unsigned char *buffer, std::size_t size) {
    fillRandom({buffer, size});
    return 0;
}

// The protocols only ever encrypt whole blocks, so there's no padding.
Bytes aesCbc(int mode, std::span<const std::uint8_t> key, std::span<const std::uint8_t> iv,
             std::span<const std::uint8_t> data) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    if (mode == MBEDTLS_AES_ENCRYPT) {
        mbedtls_aes_setkey_enc(&aes, key.data(), static_cast<unsigned>(key.size() * 8));
    } else {
        mbedtls_aes_setkey_dec(&aes, key.data(), static_cast<unsigned>(key.size() * 8));
    }
    unsigned char ivCopy[16];
    std::ranges::copy(iv.first(16), ivCopy);
    Bytes out(data.size());
    mbedtls_aes_crypt_cbc(&aes, mode, data.size(), ivCopy, data.data(), out.data());
    mbedtls_aes_free(&aes);
    return out;
}

// HKDF-SHA-256 with a zero salt and 32 bytes of output, which is one HMAC round.
Bytes hkdfSha256(std::span<const std::uint8_t> ikm, std::string_view info) {
    const Bytes salt(32, 0);
    const Bytes prk = hmacSha256(salt, ikm);
    Bytes message(info.begin(), info.end());
    message.push_back(1);
    return hmacSha256(prk, message);
}

std::span<const std::uint8_t> aesKey(std::span<const std::uint8_t> key) {
    return key.size() == 64 ? key.subspan(32) : key;
}

Bytes concat(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) {
    Bytes out(a.begin(), a.end());
    out.insert(out.end(), b.begin(), b.end());
    return out;
}

}  // namespace

Bytes sha256(std::span<const std::uint8_t> data) {
    Bytes hash(32);
    mbedtls_sha256(data.data(), data.size(), hash.data(), 0);
    return hash;
}

Bytes hmacSha256(std::span<const std::uint8_t> key, std::span<const std::uint8_t> message) {
    Bytes mac(32);
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key.data(), key.size(),
                    message.data(), message.size(), mac.data());
    return mac;
}

void fillRandom(std::span<std::uint8_t> out) {
    std::size_t filled = 0;
    while (filled < out.size()) {
        const ssize_t got = ::getrandom(out.data() + filled, out.size() - filled, 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            // Without randomness there's nothing safe to do.
            std::abort();
        }
        filled += static_cast<std::size_t>(got);
    }
}

P256KeyPair generateP256KeyPair() {
    mbedtls_ecp_group group;
    mbedtls_ecp_group_init(&group);
    mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
    mbedtls_mpi d;
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point q;
    mbedtls_ecp_point_init(&q);
    mbedtls_ecp_gen_keypair(&group, &d, &q, mbedtlsRandom, nullptr);
    P256KeyPair pair{Bytes(32), {}, {}};
    mbedtls_mpi_write_binary(&d, pair.privateKey.data(), 32);
    unsigned char point[65];
    std::size_t length = 0;
    mbedtls_ecp_point_write_binary(&group, &q, MBEDTLS_ECP_PF_UNCOMPRESSED, &length, point,
                                   sizeof(point));
    pair.x.assign(point + 1, point + 33);
    pair.y.assign(point + 33, point + 65);
    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&group);
    return pair;
}

std::optional<Bytes> p256Ecdh(std::span<const std::uint8_t> privateKey,
                              std::span<const std::uint8_t> peerX,
                              std::span<const std::uint8_t> peerY) {
    if (privateKey.size() != 32 || peerX.size() != 32 || peerY.size() != 32) {
        return std::nullopt;
    }
    mbedtls_ecp_group group;
    mbedtls_ecp_group_init(&group);
    mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
    mbedtls_mpi d;
    mbedtls_mpi_init(&d);
    mbedtls_mpi z;
    mbedtls_mpi_init(&z);
    mbedtls_ecp_point peer;
    mbedtls_ecp_point_init(&peer);
    unsigned char point[65];
    point[0] = 0x04;
    std::ranges::copy(peerX, point + 1);
    std::ranges::copy(peerY, point + 33);
    std::optional<Bytes> shared;
    if (mbedtls_ecp_point_read_binary(&group, &peer, point, sizeof(point)) == 0
        && mbedtls_ecp_check_pubkey(&group, &peer) == 0
        && mbedtls_mpi_read_binary(&d, privateKey.data(), privateKey.size()) == 0
        && mbedtls_ecdh_compute_shared(&group, &z, &peer, &d, mbedtlsRandom, nullptr) == 0) {
        shared.emplace(32);
        mbedtls_mpi_write_binary(&z, shared->data(), 32);
    }
    mbedtls_ecp_point_free(&peer);
    mbedtls_mpi_free(&z);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&group);
    return shared;
}

CborValue encodeCoseKey(std::span<const std::uint8_t> x, std::span<const std::uint8_t> y) {
    return CborMap{
        {1, 2},     // kty: EC2
        {3, -25},   // alg: ECDH-ES+HKDF-256
        {-1, 1},    // crv: P-256
        {-2, Bytes(x.begin(), x.end())},
        {-3, Bytes(y.begin(), y.end())},
    };
}

std::optional<std::pair<Bytes, Bytes>> decodeCoseKey(const CborValue &key) {
    const auto *kty = key.find(1);
    const auto *crv = key.find(-1);
    const auto *x = key.find(-2);
    const auto *y = key.find(-3);
    if (!kty || !kty->integer() || *kty->integer() != 2 || !crv || !crv->integer()
        || *crv->integer() != 1 || !x || !x->bytes() || x->bytes()->size() != 32 || !y
        || !y->bytes() || y->bytes()->size() != 32) {
        return std::nullopt;
    }
    return std::pair{*x->bytes(), *y->bytes()};
}

const PinUvAuthProtocol &PinUvAuthProtocol::one() {
    static constexpr PinUvAuthProtocol protocol(1);
    return protocol;
}

const PinUvAuthProtocol &PinUvAuthProtocol::two() {
    static constexpr PinUvAuthProtocol protocol(2);
    return protocol;
}

KeyResult<PinUvAuthProtocol::Encapsulation> PinUvAuthProtocol::encapsulate(
    const CborValue &peerKey) const {
    const auto peer = decodeCoseKey(peerKey);
    if (!peer) {
        return keyFailure(KeyError::Kind::kIo, "Unsupported key agreement key");
    }
    const P256KeyPair own = generateP256KeyPair();
    const auto z = p256Ecdh(own.privateKey, peer->first, peer->second);
    if (!z) {
        return keyFailure(KeyError::Kind::kIo, "Invalid key agreement key");
    }
    return Encapsulation{encodeCoseKey(own.x, own.y), kdf(*z)};
}

Bytes PinUvAuthProtocol::kdf(std::span<const std::uint8_t> z) const {
    if (version_ == 1) {
        return sha256(z);
    }
    // The HMAC key, then the AES key.
    return concat(hkdfSha256(z, "CTAP2 HMAC key"), hkdfSha256(z, "CTAP2 AES key"));
}

Bytes PinUvAuthProtocol::encrypt(std::span<const std::uint8_t> key,
                                 std::span<const std::uint8_t> plaintext) const {
    if (version_ == 1) {
        return aesCbc(MBEDTLS_AES_ENCRYPT, key, Bytes(16, 0), plaintext);
    }
    Bytes iv(16);
    fillRandom(iv);
    return concat(iv, aesCbc(MBEDTLS_AES_ENCRYPT, aesKey(key), iv, plaintext));
}

KeyResult<Bytes> PinUvAuthProtocol::decrypt(std::span<const std::uint8_t> key,
                                            std::span<const std::uint8_t> ciphertext) const {
    if (version_ == 1) {
        if (ciphertext.size() % 16 != 0) {
            return keyFailure(KeyError::Kind::kIo, "Bad ciphertext length");
        }
        return aesCbc(MBEDTLS_AES_DECRYPT, key, Bytes(16, 0), ciphertext);
    }
    if (ciphertext.size() < 16 || ciphertext.size() % 16 != 0) {
        return keyFailure(KeyError::Kind::kIo, "Bad ciphertext length");
    }
    return aesCbc(MBEDTLS_AES_DECRYPT, aesKey(key), ciphertext.first(16), ciphertext.subspan(16));
}

Bytes PinUvAuthProtocol::authenticate(std::span<const std::uint8_t> key,
                                      std::span<const std::uint8_t> message) const {
    if (version_ == 1) {
        Bytes mac = hmacSha256(key, message);
        mac.resize(16);
        return mac;
    }
    // With the shared secret, this is its HMAC half; a token is used whole.
    return hmacSha256(key.first(std::min<std::size_t>(key.size(), 32)), message);
}

}  // namespace filepan::securitykey
