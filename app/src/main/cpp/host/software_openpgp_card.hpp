// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

// An OpenPGP card in software, for tests: the applet's side of SELECT, GET DATA, VERIFY, reading
// the authentication key and INTERNAL AUTHENTICATE, as a YubiKey answers them.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/md.h>
#include <mbedtls/rsa.h>

#include "filepan/securitykey/iso7816.hpp"
#include "filepan/securitykey/openpgp_card.hpp"
#include "filepan/securitykey/pin_protocol.hpp"
#include "filepan/securitykey/ssh_wire.hpp"
#include "test_util.hpp"

// libssh's bundled Ed25519 (src/external/ed25519.c), which its static library carries.
extern "C" {
int crypto_sign_ed25519_keypair(std::uint8_t pk[32], std::uint8_t sk[64]);
int crypto_sign_ed25519(unsigned char *sm, std::uint64_t *smlen, const unsigned char *m,
                        std::uint64_t mlen, const std::uint8_t sk[64]);
int crypto_sign_ed25519_open(unsigned char *m, std::uint64_t *mlen, const unsigned char *sm,
                             std::uint64_t smlen, const std::uint8_t pk[32]);
}

namespace filepan::test {

using securitykey::Bytes;
namespace ssh_wire = securitykey::ssh_wire;

inline int cardRandom(void *, unsigned char *buffer, std::size_t size) {
    securitykey::fillRandom({buffer, size});
    return 0;
}

inline Bytes cardDigest(mbedtls_md_type_t type, std::span<const std::uint8_t> data) {
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(type);
    Bytes out(mbedtls_md_get_size(info));
    mbedtls_md(info, data.data(), data.size(), out.data());
    return out;
}

// A BER-TLV data object; tags up to two bytes.
inline Bytes tlv(std::uint32_t tag, std::span<const std::uint8_t> value) {
    Bytes out;
    if (tag > 0xFF) {
        out.push_back(static_cast<std::uint8_t>(tag >> 8));
    }
    out.push_back(static_cast<std::uint8_t>(tag));
    if (value.size() < 0x80) {
        out.push_back(static_cast<std::uint8_t>(value.size()));
    } else if (value.size() <= 0xFF) {
        out.push_back(0x81);
        out.push_back(static_cast<std::uint8_t>(value.size()));
    } else {
        out.push_back(0x82);
        out.push_back(static_cast<std::uint8_t>(value.size() >> 8));
        out.push_back(static_cast<std::uint8_t>(value.size()));
    }
    out.insert(out.end(), value.begin(), value.end());
    return out;
}

inline Bytes joined(std::initializer_list<std::span<const std::uint8_t>> parts) {
    Bytes out;
    for (const auto part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

// r and s, each the field's size, as the DER Mbed TLS reads and writes.
inline Bytes derSignature(std::span<const std::uint8_t> r, std::span<const std::uint8_t> s) {
    const auto integer = [](std::span<const std::uint8_t> value) {
        while (value.size() > 1 && value.front() == 0) {
            value = value.subspan(1);
        }
        Bytes magnitude(value.begin(), value.end());
        if (magnitude.front() & 0x80) {
            magnitude.insert(magnitude.begin(), 0);
        }
        return tlv(0x02, magnitude);
    };
    return tlv(0x30, joined({integer(r), integer(s)}));
}

struct CardOptions {
    using Algorithm = securitykey::OpenPgpCard::Algorithm;

    Algorithm algorithm = Algorithm::kEd25519;
    std::string pin = "123456";
    int pinRetries = 3;
    // The PIN goes through the KDF-DO's iterated and salted S2K.
    bool kdf = false;
    // The authentication key's UIF is on.
    bool touch = false;
    // Whether an authentication key has been generated or imported.
    bool hasKey = true;
    // Algorithm attributes for a Brainpool curve, which SSH has no name for.
    bool brainpool = false;
    std::uint32_t serial = 0x12345678;
};

class SoftwareOpenPgpCard {
public:
    using Algorithm = securitykey::OpenPgpCard::Algorithm;

    static constexpr std::uint8_t kSalt[] = {1, 2, 3, 4, 5, 6, 7, 8};
    static constexpr std::uint32_t kKdfCount = 100000;

    explicit SoftwareOpenPgpCard(CardOptions options) : options_(std::move(options)) {
        pinRetries = options_.pinRetries;
        mbedtls_rsa_init(&rsa_);
        mbedtls_ecp_keypair_init(&ecp_);
        switch (options_.algorithm) {
        case Algorithm::kRsa:
            EXPECT(mbedtls_rsa_gen_key(&rsa_, cardRandom, nullptr, 2048, 65537) == 0);
            break;
        case Algorithm::kEcdsaP256:
            EXPECT(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, &ecp_, cardRandom, nullptr) == 0);
            break;
        case Algorithm::kEcdsaP384:
            EXPECT(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP384R1, &ecp_, cardRandom, nullptr) == 0);
            break;
        default: EXPECT(crypto_sign_ed25519_keypair(edPublic_, edPrivate_) == 0); break;
        }
    }
    ~SoftwareOpenPgpCard() {
        mbedtls_rsa_free(&rsa_);
        mbedtls_ecp_keypair_free(&ecp_);
    }
    SoftwareOpenPgpCard(const SoftwareOpenPgpCard &) = delete;
    SoftwareOpenPgpCard &operator=(const SoftwareOpenPgpCard &) = delete;

    int pinRetries = 3;
    bool verified = false;
    int verifies = 0;
    int signatures = 0;
    int chainedBlocks = 0;
    int getResponses = 0;
    int extendedCommands = 0;
    // A VERIFY that came with an Le, which a case 3 command shouldn't have.
    bool verifyHadLe = false;
    std::vector<Bytes> verifyData;

    // The authentication key's SSH public key blob, built here rather than by the client.
    [[nodiscard]] Bytes publicBlob() const {
        Bytes blob;
        switch (options_.algorithm) {
        case Algorithm::kRsa: {
            const auto [n, e] = rsaPublic();
            ssh_wire::appendString(blob, "ssh-rsa");
            ssh_wire::appendMpint(blob, e);
            ssh_wire::appendMpint(blob, n);
            break;
        }
        case Algorithm::kEcdsaP256:
        case Algorithm::kEcdsaP384:
            ssh_wire::appendString(blob,
                                   options_.algorithm == Algorithm::kEcdsaP256
                                       ? "ecdsa-sha2-nistp256"
                                       : "ecdsa-sha2-nistp384");
            ssh_wire::appendString(
                blob, options_.algorithm == Algorithm::kEcdsaP256 ? "nistp256" : "nistp384");
            ssh_wire::appendString(blob, ecPoint());
            break;
        default:
            ssh_wire::appendString(blob, "ssh-ed25519");
            ssh_wire::appendString(blob, std::span<const std::uint8_t>(edPublic_, 32));
            break;
        }
        return blob;
    }

    // Whether signature is an SSH signature blob by this card's key over data.
    [[nodiscard]] bool verifiesSsh(std::span<const std::uint8_t> data,
                                   std::span<const std::uint8_t> blob) {
        ssh_wire::Reader reader(blob);
        const auto nameBytes = reader.string();
        const auto signature = reader.string();
        if (!nameBytes || !signature || !reader.empty()) {
            return false;
        }
        const std::string name(nameBytes->begin(), nameBytes->end());
        switch (options_.algorithm) {
        case Algorithm::kRsa: {
            mbedtls_md_type_t type = name == "rsa-sha2-512" ? MBEDTLS_MD_SHA512
                : name == "rsa-sha2-256"                    ? MBEDTLS_MD_SHA256
                : name == "ssh-rsa"                         ? MBEDTLS_MD_SHA1
                                                            : MBEDTLS_MD_NONE;
            if (type == MBEDTLS_MD_NONE) {
                return false;
            }
            const Bytes hash = cardDigest(type, data);
            return mbedtls_rsa_pkcs1_verify(&rsa_, type, static_cast<unsigned>(hash.size()),
                                            hash.data(), signature->data())
                == 0;
        }
        case Algorithm::kEcdsaP256:
        case Algorithm::kEcdsaP384: {
            const bool p256 = options_.algorithm == Algorithm::kEcdsaP256;
            if (name != (p256 ? "ecdsa-sha2-nistp256" : "ecdsa-sha2-nistp384")) {
                return false;
            }
            ssh_wire::Reader inner(*signature);
            const auto r = inner.string();
            const auto s = inner.string();
            if (!r || !s || !inner.empty()) {
                return false;
            }
            const Bytes hash = cardDigest(p256 ? MBEDTLS_MD_SHA256 : MBEDTLS_MD_SHA384, data);
            const Bytes der = derSignature(*r, *s);
            return mbedtls_ecdsa_read_signature(&ecp_, hash.data(), hash.size(), der.data(),
                                                der.size())
                == 0;
        }
        default: {
            if (name != "ssh-ed25519" || signature->size() != 64) {
                return false;
            }
            Bytes signedMessage = joined({*signature, data});
            Bytes opened(signedMessage.size());
            std::uint64_t openedSize = 0;
            return crypto_sign_ed25519_open(opened.data(), &openedSize, signedMessage.data(),
                                            signedMessage.size(), edPublic_)
                == 0;
        }
        }
    }

    // One command APDU in, the response with its status word out.
    Bytes process(std::span<const std::uint8_t> apdu) {
        EXPECT(apdu.size() >= 4);
        const int cla = apdu[0];
        const int ins = apdu[1];
        const int p1 = apdu[2];
        const int p2 = apdu[3];
        Bytes data;
        bool hasLe = false;
        if (apdu.size() == 5) {
            hasLe = true;
        } else if (apdu.size() > 5 && apdu[4] == 0) {
            ++extendedCommands;
            const std::size_t length = (std::size_t{apdu[5]} << 8) | apdu[6];
            EXPECT(apdu.size() == 7 + length || apdu.size() == 7 + length + 2);
            data.assign(apdu.begin() + 7, apdu.begin() + 7 + static_cast<std::ptrdiff_t>(length));
            hasLe = apdu.size() == 7 + length + 2;
        } else if (apdu.size() > 5) {
            const std::size_t length = apdu[4];
            EXPECT(apdu.size() == 5 + length || apdu.size() == 5 + length + 1);
            data.assign(apdu.begin() + 5, apdu.begin() + 5 + static_cast<std::ptrdiff_t>(length));
            hasLe = apdu.size() == 5 + length + 1;
        }
        if (ins == 0xC0) {
            ++getResponses;
            return nextPart();
        }
        pending_.clear();
        if (cla & 0x10) {
            ++chainedBlocks;
            chain_.insert(chain_.end(), data.begin(), data.end());
            return {0x90, 0x00};
        }
        data.insert(data.begin(), chain_.begin(), chain_.end());
        chain_.clear();
        if (ins == 0xA4) {
            static constexpr std::uint8_t kAid[] = {0xD2, 0x76, 0x00, 0x01, 0x24, 0x01};
            selected_ = std::ranges::equal(data, kAid);
            verified = false;
            return selected_ ? Bytes{0x90, 0x00} : Bytes{0x6A, 0x82};
        }
        if (!selected_) {
            return {0x6D, 0x00};
        }
        switch (ins) {
        case 0xCA: return respond(getData((p1 << 8) | p2));
        case 0x20: {
            EXPECT(p2 == 0x82);
            verifyHadLe = verifyHadLe || hasLe;
            return verify(data);
        }
        case 0x47: {
            EXPECT(p1 == 0x81 && data == Bytes({0xA4, 0x00}));
            if (!options_.hasKey) {
                return {0x6A, 0x88};
            }
            return respond(publicKeyObject());
        }
        case 0x88: return authenticate(data);
        default: return {0x6D, 0x00};
        }
    }

    // The S2K-hashed PIN, as gpg would put on the card.
    [[nodiscard]] Bytes hashedPin(std::string_view pin) const {
        return securitykey::iteratedSaltedS2k(8, kSalt, kKdfCount, pin);
    }

private:
    [[nodiscard]] std::pair<Bytes, Bytes> rsaPublic() const {
        Bytes n(mbedtls_rsa_get_len(&rsa_));
        Bytes e(4);
        EXPECT(mbedtls_rsa_export_raw(&rsa_, n.data(), n.size(), nullptr, 0, nullptr, 0, nullptr, 0,
                                      e.data(), e.size())
               == 0);
        return {n, e};
    }

    [[nodiscard]] Bytes ecPoint() const {
        Bytes point(1 + 2 * 48);
        std::size_t size = 0;
        EXPECT(mbedtls_ecp_write_public_key(&ecp_, MBEDTLS_ECP_PF_UNCOMPRESSED, &size, point.data(),
                                            point.size())
               == 0);
        point.resize(size);
        return point;
    }

    [[nodiscard]] Bytes algorithmAttributes() const {
        if (options_.brainpool) {
            return {0x13, 0x2B, 0x24, 0x03, 0x03, 0x02, 0x08, 0x01, 0x01, 0x07};
        }
        switch (options_.algorithm) {
        case Algorithm::kRsa: return {0x01, 0x08, 0x00, 0x00, 0x20, 0x00};
        case Algorithm::kEcdsaP256: return {0x13, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07};
        // With the import-format byte some cards add.
        case Algorithm::kEcdsaP384: return {0x13, 0x2B, 0x81, 0x04, 0x00, 0x22, 0xFF};
        default: return {0x16, 0x2B, 0x06, 0x01, 0x04, 0x01, 0xDA, 0x47, 0x0F, 0x01};
        }
    }

    [[nodiscard]] Bytes pinStatus() const {
        return {0x01, 0x7F, 0x7F, 0x7F, static_cast<std::uint8_t>(pinRetries), 0x00, 0x03};
    }

    std::optional<Bytes> getData(int tag) const {
        switch (tag) {
        case 0x6E: {
            const Bytes aid = {0xD2,
                               0x76,
                               0x00,
                               0x01,
                               0x24,
                               0x01,
                               0x03,
                               0x04,
                               0x00,
                               0x06,
                               static_cast<std::uint8_t>(options_.serial >> 24),
                               static_cast<std::uint8_t>(options_.serial >> 16),
                               static_cast<std::uint8_t>(options_.serial >> 8),
                               static_cast<std::uint8_t>(options_.serial),
                               0x00,
                               0x00};
            Bytes fingerprints(60, 0);
            if (options_.hasKey) {
                std::fill(fingerprints.begin() + 40, fingerprints.end(), 0xAB);
            }
            const Bytes ignored(10, 0x7F);
            const Bytes discretionary
                = joined({tlv(0xC0, ignored), tlv(0xC1, Bytes{0x01, 0x08, 0x00, 0x00, 0x20, 0x00}),
                          tlv(0xC2, Bytes{0x01, 0x08, 0x00, 0x00, 0x20, 0x00}),
                          tlv(0xC3, algorithmAttributes()), tlv(0xC4, pinStatus()),
                          tlv(0xC5, fingerprints), tlv(0xC6, Bytes(60, 0))});
            return tlv(
                0x6E,
                joined({tlv(0x4F, aid), tlv(0x5F52, Bytes{0x00, 0x73}), tlv(0x73, discretionary)}));
        }
        case 0xC4: return pinStatus();
        case 0xD8: return Bytes{static_cast<std::uint8_t>(options_.touch ? 0x01 : 0x00), 0x20};
        case 0xF9:
            if (!options_.kdf) {
                return Bytes{0x81, 0x01, 0x00};
            }
            {
                const Bytes count = {static_cast<std::uint8_t>(kKdfCount >> 24),
                                     static_cast<std::uint8_t>(kKdfCount >> 16),
                                     static_cast<std::uint8_t>(kKdfCount >> 8),
                                     static_cast<std::uint8_t>(kKdfCount)};
                // As a YubiKey returns it: wrapped in its own tag.
                return tlv(0xF9,
                           joined({tlv(0x81, Bytes{0x03}), tlv(0x82, Bytes{0x08}), tlv(0x83, count),
                                   tlv(0x84, kSalt), tlv(0x85, kSalt), tlv(0x86, kSalt)}));
            }
        default: return std::nullopt;
        }
    }

    Bytes publicKeyObject() const {
        switch (options_.algorithm) {
        case Algorithm::kRsa: {
            const auto [n, e] = rsaPublic();
            return tlv(0x7F49, joined({tlv(0x81, n), tlv(0x82, Bytes(e.begin() + 1, e.end()))}));
        }
        case Algorithm::kEcdsaP256:
        case Algorithm::kEcdsaP384: return tlv(0x7F49, tlv(0x86, ecPoint()));
        default: return tlv(0x7F49, tlv(0x86, std::span<const std::uint8_t>(edPublic_, 32)));
        }
    }

    Bytes verify(std::span<const std::uint8_t> data) {
        ++verifies;
        verifyData.emplace_back(data.begin(), data.end());
        if (pinRetries == 0) {
            return {0x69, 0x83};
        }
        const Bytes expected = options_.kdf ? hashedPin(options_.pin)
                                            : Bytes(options_.pin.begin(), options_.pin.end());
        if (!std::ranges::equal(data, expected)) {
            --pinRetries;
            verified = false;
            return {0x63, static_cast<std::uint8_t>(0xC0 | pinRetries)};
        }
        pinRetries = options_.pinRetries;
        verified = true;
        return {0x90, 0x00};
    }

    Bytes authenticate(std::span<const std::uint8_t> input) {
        if (!verified) {
            return {0x69, 0x82};
        }
        ++signatures;
        Bytes signature;
        switch (options_.algorithm) {
        case Algorithm::kRsa:
            signature.resize(mbedtls_rsa_get_len(&rsa_));
            EXPECT(mbedtls_rsa_pkcs1_sign(&rsa_, cardRandom, nullptr, MBEDTLS_MD_NONE,
                                          static_cast<unsigned>(input.size()), input.data(),
                                          signature.data())
                   == 0);
            break;
        case Algorithm::kEcdsaP256:
        case Algorithm::kEcdsaP384: {
            const std::size_t field = options_.algorithm == Algorithm::kEcdsaP256 ? 32 : 48;
            EXPECT(input.size() == field);
            unsigned char der[MBEDTLS_ECDSA_MAX_LEN];
            std::size_t derSize = 0;
            EXPECT(mbedtls_ecdsa_write_signature(
                       &ecp_, field == 32 ? MBEDTLS_MD_SHA256 : MBEDTLS_MD_SHA384, input.data(),
                       input.size(), der, sizeof der, &derSize, cardRandom, nullptr)
                   == 0);
            // 30 len 02 len r 02 len s, all short here.
            const auto integer = [&](std::size_t &at) {
                EXPECT(der[at] == 0x02);
                const std::size_t length = der[at + 1];
                std::span<const std::uint8_t> value(der + at + 2, length);
                at += 2 + length;
                while (value.size() > field) {
                    value = value.subspan(1);
                }
                Bytes padded(field - value.size(), 0);
                padded.insert(padded.end(), value.begin(), value.end());
                return padded;
            };
            std::size_t at = 2;
            const Bytes r = integer(at);
            const Bytes s = integer(at);
            signature = joined({r, s});
            break;
        }
        default: {
            Bytes signedMessage(64 + input.size());
            std::uint64_t size = 0;
            EXPECT(crypto_sign_ed25519(signedMessage.data(), &size, input.data(), input.size(),
                                       edPrivate_)
                   == 0);
            signature.assign(signedMessage.begin(), signedMessage.begin() + 64);
            break;
        }
        }
        return respond(signature);
    }

    // Short responses only: a long one goes out in 256-byte parts behind 61xx.
    Bytes respond(std::optional<Bytes> data) {
        if (!data) {
            return {0x6A, 0x88};
        }
        pending_ = std::move(*data);
        return nextPart();
    }

    Bytes nextPart() {
        const std::size_t take = std::min<std::size_t>(pending_.size(), 256);
        Bytes out(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(take));
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(take));
        if (pending_.empty()) {
            out.push_back(0x90);
            out.push_back(0x00);
        } else {
            out.push_back(0x61);
            out.push_back(static_cast<std::uint8_t>(std::min<std::size_t>(pending_.size(), 256)));
        }
        return out;
    }

    CardOptions options_;
    mutable mbedtls_rsa_context rsa_;
    mutable mbedtls_ecp_keypair ecp_;
    std::uint8_t edPublic_[32] = {};
    std::uint8_t edPrivate_[64] = {};
    bool selected_ = false;
    Bytes chain_;
    Bytes pending_;
};

// The card over ISO-DEP, as Android's IsoDep hands it over.
class CardLink final : public securitykey::ApduChannel {
public:
    explicit CardLink(SoftwareOpenPgpCard &card, bool extendedLength = false)
            : card_(card), extendedLength_(extendedLength) {}

    // Fails every transceive after this many, as a card pulled from the field does.
    int failAfter = -1;
    // Called before each transceive, on its thread.
    std::function<void()> beforeTransceive;

    [[nodiscard]] bool supportsExtendedLength() const override { return extendedLength_; }
    void close() override { closed_ = true; }

    std::optional<Bytes> transceive(std::span<const std::uint8_t> apdu) override {
        if (beforeTransceive) {
            beforeTransceive();
        }
        if (closed_ || failAfter == 0) {
            return std::nullopt;
        }
        if (failAfter > 0) {
            --failAfter;
        }
        return card_.process(apdu);
    }

private:
    SoftwareOpenPgpCard &card_;
    bool extendedLength_;
    std::atomic<bool> closed_ = false;
};

}  // namespace filepan::test
