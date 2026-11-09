// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/securitykey/openpgp_card.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>

#include <mbedtls/md.h>

#include "filepan/securitykey/ssh_wire.hpp"

namespace filepan::securitykey {

namespace {

using Kind = KeyError::Kind;

constexpr std::uint8_t kOpenPgpAid[] = {0xD2, 0x76, 0x00, 0x01, 0x24, 0x01};

constexpr int kInsSelect = 0xA4;
constexpr int kInsGetData = 0xCA;
constexpr int kInsVerify = 0x20;
constexpr int kInsGenerateKeyPair = 0x47;
constexpr int kInsInternalAuthenticate = 0x88;
// VERIFY's P2 for PW1 in the mode that unlocks the authentication (and decryption) key.
constexpr int kPw1Other = 0x82;

constexpr int kSwPinBlocked = 0x6983;
constexpr int kSwConditionsNotSatisfied = 0x6985;
constexpr int kSwSecurityStatusNotSatisfied = 0x6982;

constexpr std::uint8_t kOidP256[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07};
constexpr std::uint8_t kOidP384[] = {0x2B, 0x81, 0x04, 0x00, 0x22};
constexpr std::uint8_t kOidEd25519[] = {0x2B, 0x06, 0x01, 0x04, 0x01, 0xDA, 0x47, 0x0F, 0x01};

// DER DigestInfo prefixes (RFC 8017 §9.2): the card adds PKCS #1 v1.5 padding around them.
constexpr std::uint8_t kDigestInfoSha1[]
    = {0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2B, 0x0E, 0x03, 0x02, 0x1A, 0x05, 0x00, 0x04, 0x14};
constexpr std::uint8_t kDigestInfoSha256[]
    = {0x30, 0x31, 0x30, 0x0D, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
       0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};
constexpr std::uint8_t kDigestInfoSha512[]
    = {0x30, 0x51, 0x30, 0x0D, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
       0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40};

// OpenPGP's hash algorithm ids.
constexpr int kHashSha256 = 8;
constexpr int kHashSha512 = 10;

std::string hex(std::span<const std::uint8_t> bytes) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    for (const std::uint8_t byte : bytes) {
        out += kDigits[byte >> 4];
        out += kDigits[byte & 0xF];
    }
    return out;
}

std::string statusText(int status) {
    return hex(
        std::array{static_cast<std::uint8_t>(status >> 8), static_cast<std::uint8_t>(status)});
}

Bytes digest(mbedtls_md_type_t type, std::span<const std::uint8_t> data) {
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(type);
    Bytes out(mbedtls_md_get_size(info));
    mbedtls_md(info, data.data(), data.size(), out.data());
    return out;
}

// Finds a data object in BER-TLV, looking inside constructed ones.
std::optional<std::span<const std::uint8_t>> findTlv(std::span<const std::uint8_t> data,
                                                     std::uint32_t wanted, int depth = 0) {
    std::size_t at = 0;
    while (at < data.size()) {
        const std::uint8_t first = data[at++];
        if (first == 0x00 || first == 0xFF) {
            continue;  // Padding between objects.
        }
        std::uint32_t tag = first;
        if ((first & 0x1F) == 0x1F) {
            int more = 0;
            do {
                if (at >= data.size() || ++more > 2) {
                    return std::nullopt;
                }
                tag = (tag << 8) | data[at];
            } while (data[at++] & 0x80);
        }
        if (at >= data.size()) {
            return std::nullopt;
        }
        std::size_t length = data[at++];
        if (length == 0x81 || length == 0x82) {
            const std::size_t bytes = length - 0x80;
            if (at + bytes > data.size()) {
                return std::nullopt;
            }
            length = 0;
            for (std::size_t i = 0; i < bytes; ++i) {
                length = (length << 8) | data[at++];
            }
        } else if (length > 0x7F) {
            return std::nullopt;
        }
        if (length > data.size() - at) {
            return std::nullopt;
        }
        const auto value = data.subspan(at, length);
        if (tag == wanted) {
            return value;
        }
        if ((first & 0x20) != 0 && depth < 4) {
            if (auto found = findTlv(value, wanted, depth + 1)) {
                return found;
            }
        }
        at += length;
    }
    return std::nullopt;
}

bool isZero(std::span<const std::uint8_t> bytes) {
    return std::ranges::all_of(bytes, [](std::uint8_t b) { return b == 0; });
}

bool hasOid(std::span<const std::uint8_t> attributes, std::span<const std::uint8_t> oid) {
    // The OID may be followed by one byte for the import format.
    const auto rest = attributes.subspan(1);
    return rest.size() >= oid.size() && rest.size() <= oid.size() + 1
        && std::ranges::equal(rest.first(oid.size()), oid);
}

OpenPgpCard::Algorithm parseAlgorithm(std::span<const std::uint8_t> attributes) {
    using Algorithm = OpenPgpCard::Algorithm;
    if (attributes.empty()) {
        return Algorithm::kNone;
    }
    switch (attributes[0]) {
    case 0x01: return Algorithm::kRsa;
    case 0x13:
        if (hasOid(attributes, kOidP256)) {
            return Algorithm::kEcdsaP256;
        }
        if (hasOid(attributes, kOidP384)) {
            return Algorithm::kEcdsaP384;
        }
        return Algorithm::kOther;
    case 0x16: return hasOid(attributes, kOidEd25519) ? Algorithm::kEd25519 : Algorithm::kOther;
    default: return Algorithm::kOther;
    }
}

const char *ecdsaName(OpenPgpCard::Algorithm algorithm) {
    return algorithm == OpenPgpCard::Algorithm::kEcdsaP256 ? "ecdsa-sha2-nistp256"
                                                           : "ecdsa-sha2-nistp384";
}

const char *curveName(OpenPgpCard::Algorithm algorithm) {
    return algorithm == OpenPgpCard::Algorithm::kEcdsaP256 ? "nistp256" : "nistp384";
}

}  // namespace

KeyResult<std::unique_ptr<OpenPgpCard>> OpenPgpCard::open(std::unique_ptr<ApduChannel> channel,
                                                          bool userPresent) {
    std::unique_ptr<OpenPgpCard> card(new OpenPgpCard(std::move(channel), userPresent));
    auto selected = card->session_.transmit(0x00, kInsSelect, 0x04, 0x00, kOpenPgpAid);
    if (!selected) {
        return std::unexpected(std::move(selected.error()));
    }
    if (selected->status != iso7816::kSwNoError) {
        return keyFailure(Kind::kIo, "Not an OpenPGP card");
    }
    if (auto read = card->readState(); !read) {
        return std::unexpected(std::move(read.error()));
    }
    return card;
}

KeyResult<Bytes> OpenPgpCard::getData(int tag) {
    auto response = session_.transmit(0x00, kInsGetData, tag >> 8, tag & 0xFF, {});
    if (!response) {
        return std::unexpected(std::move(response.error()));
    }
    if (response->status != iso7816::kSwNoError) {
        return keyFailure(Kind::kIo, "GET DATA failed: " + statusText(response->status));
    }
    return std::move(response->data);
}

KeyResult<void> OpenPgpCard::readState() {
    auto related = getData(0x6E);
    if (!related) {
        return std::unexpected(std::move(related.error()));
    }
    const auto aid = findTlv(*related, 0x4F);
    if (!aid || aid->size() < 14) {
        return keyFailure(Kind::kIo, "The card didn't say its serial number");
    }
    serial_ = hex(aid->subspan(8, 6));
    algorithm_ = parseAlgorithm(findTlv(*related, 0xC3).value_or(std::span<const std::uint8_t>{}));
    // The fingerprints of the signature, decryption and authentication keys; all zero for none.
    if (const auto fingerprints = findTlv(*related, 0xC5);
        fingerprints && fingerprints->size() >= 60 && isZero(fingerprints->subspan(40, 20))) {
        algorithm_ = Algorithm::kNone;
    }
    std::optional<Bytes> pinStatus;
    if (const auto status = findTlv(*related, 0xC4)) {
        pinStatus.emplace(status->begin(), status->end());
    } else if (auto fetched = getData(0xC4)) {
        pinStatus = std::move(*fetched);
    }
    if (pinStatus && pinStatus->size() >= 5) {
        pinRetries_ = (*pinStatus)[4];
    }
    // The authentication key's user interaction flag; cards before 3.x don't have one.
    if (auto uif = getData(0xD8); uif && !uif->empty()) {
        touchRequired_ = (*uif)[0] != 0;
    }
    if (auto kdf = getData(0xF9)) {
        std::span<const std::uint8_t> objects = *kdf;
        if (const auto wrapped = findTlv(objects, 0xF9)) {
            objects = *wrapped;
        }
        const auto algorithm = findTlv(objects, 0x81);
        // 3: iterated and salted S2K; 0: the PIN goes as it is.
        if (algorithm && algorithm->size() == 1 && (*algorithm)[0] == 0x03) {
            const auto hash = findTlv(objects, 0x82);
            const auto count = findTlv(objects, 0x83);
            const auto salt = findTlv(objects, 0x84);
            if (!hash || hash->size() != 1 || !count || count->size() != 4 || !salt) {
                return keyFailure(Kind::kUnsupported, "The card's PIN hashing (KDF) is malformed");
            }
            kdfHash_ = (*hash)[0];
            if (kdfHash_ != kHashSha256 && kdfHash_ != kHashSha512) {
                return keyFailure(Kind::kUnsupported,
                                  "The card hashes its PIN in a way FilePan can't");
            }
            kdfCount_ = (std::uint32_t{(*count)[0]} << 24) | (std::uint32_t{(*count)[1]} << 16)
                | (std::uint32_t{(*count)[2]} << 8) | (*count)[3];
            kdfSalt_.assign(salt->begin(), salt->end());
        }
    }
    return {};
}

KeyResult<Bytes> OpenPgpCard::authenticationKey() {
    switch (algorithm_) {
    case Algorithm::kNone:
        return keyFailure(Kind::kUnsupported, "The card has no authentication key");
    case Algorithm::kOther:
        return keyFailure(Kind::kUnsupported,
                          "The card's authentication key can't be used for SSH");
    default: break;
    }
    // Reads the public key of the authentication key (CRT A4) without generating one.
    constexpr std::uint8_t kAuthenticationKey[] = {0xA4, 0x00};
    auto response = session_.transmit(0x00, kInsGenerateKeyPair, 0x81, 0x00, kAuthenticationKey);
    if (!response) {
        return std::unexpected(std::move(response.error()));
    }
    const auto publicKey
        = response->status == iso7816::kSwNoError ? findTlv(response->data, 0x7F49) : std::nullopt;
    if (!publicKey) {
        return keyFailure(Kind::kUnsupported, "The card has no authentication key");
    }
    Bytes blob;
    if (algorithm_ == Algorithm::kRsa) {
        const auto modulus = findTlv(*publicKey, 0x81);
        const auto exponent = findTlv(*publicKey, 0x82);
        if (!modulus || !exponent) {
            return keyFailure(Kind::kIo, "Malformed RSA public key");
        }
        ssh_wire::appendString(blob, "ssh-rsa");
        ssh_wire::appendMpint(blob, *exponent);
        ssh_wire::appendMpint(blob, *modulus);
        return blob;
    }
    auto point = findTlv(*publicKey, 0x86);
    if (!point) {
        return keyFailure(Kind::kIo, "Malformed public key");
    }
    if (algorithm_ == Algorithm::kEd25519) {
        // Some cards prefix the native point with 0x40, as OpenPGP's own packets do.
        if (point->size() == 33 && (*point)[0] == 0x40) {
            point = point->subspan(1);
        }
        if (point->size() != 32) {
            return keyFailure(Kind::kIo, "Malformed Ed25519 public key");
        }
        ssh_wire::appendString(blob, "ssh-ed25519");
        ssh_wire::appendString(blob, *point);
        return blob;
    }
    const std::size_t fieldSize = algorithm_ == Algorithm::kEcdsaP256 ? 32 : 48;
    if (point->size() != 1 + 2 * fieldSize || (*point)[0] != 0x04) {
        return keyFailure(Kind::kIo, "Malformed ECDSA public key");
    }
    ssh_wire::appendString(blob, ecdsaName(algorithm_));
    ssh_wire::appendString(blob, curveName(algorithm_));
    ssh_wire::appendString(blob, *point);
    return blob;
}

KeyResult<void> OpenPgpCard::verifyPin(std::string_view pin) {
    Bytes data;
    if (kdfHash_ != 0) {
        data = iteratedSaltedS2k(kdfHash_, kdfSalt_, kdfCount_, pin);
    } else {
        data.assign(pin.begin(), pin.end());
    }
    auto response = session_.transmit(0x00, kInsVerify, 0x00, kPw1Other, data, false);
    std::ranges::fill(data, 0);
    if (!response) {
        return std::unexpected(std::move(response.error()));
    }
    const int status = response->status;
    if (status == iso7816::kSwNoError) {
        return {};
    }
    if ((status & 0xFFF0) == 0x63C0) {
        pinRetries_ = status & 0x0F;
        if (pinRetries_ == 0) {
            return keyFailure(Kind::kPinBlocked);
        }
        return std::unexpected(KeyError{Kind::kPinWrong, pinRetries_, {}});
    }
    if (status == kSwPinBlocked) {
        pinRetries_ = 0;
        return keyFailure(Kind::kPinBlocked);
    }
    // E.g. 6700 or 6A80, for a PIN of a length the card doesn't take: it doesn't count as a try.
    if ((status >> 8) == 0x67 || status == 0x6A80) {
        return std::unexpected(KeyError{Kind::kPinWrong, pinRetries_, {}});
    }
    return keyFailure(Kind::kIo, "VERIFY failed: " + statusText(status));
}

KeyResult<Bytes> OpenPgpCard::signSsh(std::span<const std::uint8_t> data, std::uint32_t flags) {
    Bytes input;
    const char *name = nullptr;
    switch (algorithm_) {
    case Algorithm::kEd25519:
        input.assign(data.begin(), data.end());
        name = "ssh-ed25519";
        break;
    case Algorithm::kEcdsaP256:
        input = digest(MBEDTLS_MD_SHA256, data);
        name = ecdsaName(algorithm_);
        break;
    case Algorithm::kEcdsaP384:
        input = digest(MBEDTLS_MD_SHA384, data);
        name = ecdsaName(algorithm_);
        break;
    case Algorithm::kRsa: {
        std::span<const std::uint8_t> prefix;
        Bytes hash;
        if (flags & agent_flags::kRsaSha2_512) {
            prefix = kDigestInfoSha512;
            hash = digest(MBEDTLS_MD_SHA512, data);
            name = "rsa-sha2-512";
        } else if (flags & agent_flags::kRsaSha2_256) {
            prefix = kDigestInfoSha256;
            hash = digest(MBEDTLS_MD_SHA256, data);
            name = "rsa-sha2-256";
        } else {
            prefix = kDigestInfoSha1;
            hash = digest(MBEDTLS_MD_SHA1, data);
            name = "ssh-rsa";
        }
        input.assign(prefix.begin(), prefix.end());
        input.insert(input.end(), hash.begin(), hash.end());
        break;
    }
    case Algorithm::kNone:
    case Algorithm::kOther:
        return keyFailure(Kind::kUnsupported,
                          "The card's authentication key can't be used for SSH");
    }
    if (touchRequired_ && !userPresent_ && onUserPresenceNeeded) {
        onUserPresenceNeeded();
    }
    auto response = session_.transmit(0x00, kInsInternalAuthenticate, 0x00, 0x00, input);
    if (!response) {
        return std::unexpected(std::move(response.error()));
    }
    switch (response->status) {
    case iso7816::kSwNoError: break;
    case kSwConditionsNotSatisfied:
        // A YubiKey's answer when nobody touched it in time.
        return keyFailure(Kind::kCancelled, "The key wasn't touched");
    case kSwSecurityStatusNotSatisfied:
        return keyFailure(Kind::kIo, "The card wants its PIN again");
    default:
        return keyFailure(Kind::kIo,
                          "INTERNAL AUTHENTICATE failed: " + statusText(response->status));
    }
    const Bytes &signature = response->data;
    Bytes blob;
    ssh_wire::appendString(blob, name);
    if (algorithm_ == Algorithm::kEcdsaP256 || algorithm_ == Algorithm::kEcdsaP384) {
        const std::size_t fieldSize = algorithm_ == Algorithm::kEcdsaP256 ? 32 : 48;
        if (signature.size() != 2 * fieldSize) {
            return keyFailure(Kind::kIo, "Malformed ECDSA signature");
        }
        Bytes inner;
        ssh_wire::appendMpint(inner, std::span(signature).first(fieldSize));
        ssh_wire::appendMpint(inner, std::span(signature).subspan(fieldSize));
        ssh_wire::appendString(blob, inner);
    } else {
        if (algorithm_ == Algorithm::kEd25519 && signature.size() != 64) {
            return keyFailure(Kind::kIo, "Malformed Ed25519 signature");
        }
        ssh_wire::appendString(blob, signature);
    }
    return blob;
}

Bytes iteratedSaltedS2k(int hashAlgorithm, std::span<const std::uint8_t> salt, std::uint32_t count,
                        std::string_view secret) {
    mbedtls_md_type_t type;
    switch (hashAlgorithm) {
    case kHashSha256: type = MBEDTLS_MD_SHA256; break;
    case kHashSha512: type = MBEDTLS_MD_SHA512; break;
    default: return {};
    }
    Bytes block(salt.begin(), salt.end());
    block.insert(block.end(), secret.begin(), secret.end());
    // At least the salt and secret once, however small the count.
    std::size_t remaining = std::max<std::size_t>(count, block.size());
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(type);
    mbedtls_md_context_t context;
    mbedtls_md_init(&context);
    mbedtls_md_setup(&context, info, 0);
    mbedtls_md_starts(&context);
    while (remaining > 0) {
        const std::size_t take = std::min(remaining, block.size());
        mbedtls_md_update(&context, block.data(), take);
        remaining -= take;
    }
    Bytes out(mbedtls_md_get_size(info));
    mbedtls_md_finish(&context, out.data());
    mbedtls_md_free(&context);
    std::ranges::fill(block, 0);
    return out;
}

std::string sshKeyType(std::span<const std::uint8_t> blob) {
    ssh_wire::Reader reader(blob);
    const auto name = reader.string();
    if (!name) {
        return {};
    }
    return {reinterpret_cast<const char *>(name->data()), name->size()};
}

}  // namespace filepan::securitykey
