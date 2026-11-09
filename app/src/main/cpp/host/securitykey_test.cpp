// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// The security key client against an authenticator in software, written from the
// authenticator's side of CTAP 2.1 (and U2F), directly and over CTAPHID and NFC framing.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>

#include "filepan/securitykey/assertion.hpp"
#include "filepan/securitykey/cbor.hpp"
#include "filepan/securitykey/ctap.hpp"
#include "filepan/securitykey/ctaphid.hpp"
#include "filepan/securitykey/nfc.hpp"
#include "filepan/securitykey/pin_protocol.hpp"
#include "software_authenticator.hpp"
#include "test_util.hpp"

using namespace filepan::securitykey;
using namespace filepan::test;

namespace {

std::string hex(std::span<const std::uint8_t> bytes) {
    std::string out;
    for (const auto byte : bytes) {
        char digits[3];
        std::snprintf(digits, sizeof(digits), "%02x", byte);
        out += digits;
    }
    return out;
}

Bytes unhex(std::string_view text) {
    Bytes out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoi(std::string(text.substr(i, 2)), nullptr, 16)));
    }
    return out;
}

// The authenticator's side of CTAPHID, in front of a SoftwareAuthenticator.
class FakeHidPipe final : public HidPipe {
public:
    explicit FakeHidPipe(SoftwareAuthenticator &authenticator) : authenticator_(authenticator) {
        // Reports for the client to read pass through here; touches are announced in
        // keepalives on the way.
        authenticator_.onUserPresenceNeeded = [this] {
            Bytes packet(64, 0);
            putChannel(packet, kChannel);
            packet[4] = CtapHidTransport::kCommandKeepalive | 0x80;
            packet[6] = 1;
            packet[7] = CtapHidTransport::kKeepaliveUserPresenceNeeded;
            reports_.push_back(packet);
        };
    }

    static constexpr std::uint32_t kChannel = 0x01020304;
    int keepalivesForUser = 0;
    int cancels = 0;

    [[nodiscard]] std::size_t packetSize() const override { return 64; }

    bool write(std::span<const std::uint8_t> packet) override {
        EXPECT(packet.size() == 64);
        const std::uint32_t channel = (std::uint32_t{packet[0]} << 24) | (std::uint32_t{packet[1]} << 16)
                                      | (std::uint32_t{packet[2]} << 8) | packet[3];
        if (packet[4] & 0x80) {
            if (packet[4] == (CtapHidTransport::kCommandCancel | 0x80)) {
                ++cancels;
                return true;
            }
            command_ = packet[4] & 0x7F;
            channel_ = channel;
            length_ = (std::size_t{packet[5]} << 8) | packet[6];
            message_.assign(packet.begin() + 7, packet.begin() + 7 + std::min<std::size_t>(length_, 57));
            sequence_ = 0;
        } else {
            EXPECT(channel == channel_ && packet[4] == sequence_++);
            const std::size_t chunk = std::min<std::size_t>(length_ - message_.size(), 59);
            message_.insert(message_.end(), packet.begin() + 5, packet.begin() + 5 + chunk);
        }
        if (message_.size() == length_) {
            process();
        }
        return true;
    }

    int read(std::span<std::uint8_t> packet, int) override {
        if (reports_.empty()) {
            return 0;
        }
        std::ranges::copy(reports_.front(), packet.begin());
        reports_.pop_front();
        return 64;
    }

private:
    static void putChannel(Bytes &packet, std::uint32_t channel) {
        for (int i = 0; i < 4; ++i) {
            packet[i] = static_cast<std::uint8_t>(channel >> (24 - 8 * i));
        }
    }

    void respond(std::uint32_t channel, int command, const Bytes &data) {
        Bytes packet(64, 0);
        putChannel(packet, channel);
        packet[4] = static_cast<std::uint8_t>(command | 0x80);
        packet[5] = static_cast<std::uint8_t>(data.size() >> 8);
        packet[6] = static_cast<std::uint8_t>(data.size());
        std::size_t offset = std::min<std::size_t>(data.size(), 57);
        std::copy_n(data.begin(), offset, packet.begin() + 7);
        reports_.push_back(packet);
        std::uint8_t sequence = 0;
        while (offset < data.size()) {
            Bytes next(64, 0);
            putChannel(next, channel);
            next[4] = sequence++;
            const std::size_t chunk = std::min<std::size_t>(data.size() - offset, 59);
            std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(offset), chunk, next.begin() + 5);
            offset += chunk;
            reports_.push_back(next);
        }
    }

    void process() {
        switch (command_) {
        case CtapHidTransport::kCommandInit: {
            EXPECT(channel_ == CtapHidTransport::kBroadcastChannel);
            // Another client's reply first, which must be skipped.
            Bytes other(17, 0xEE);
            respond(channel_, CtapHidTransport::kCommandInit, other);
            Bytes reply = message_;
            const Bytes channel = bigEndian32(kChannel);
            reply.insert(reply.end(), channel.begin(), channel.end());
            reply.insert(reply.end(), {2, 5, 4, 0});
            reply.push_back(authenticator_.mayHaveCtap2() ? CtapHidTransport::kCapabilityCbor : 0);
            respond(channel_, CtapHidTransport::kCommandInit, reply);
            break;
        }
        case CtapHidTransport::kCommandCbor: {
            EXPECT(channel_ == kChannel);
            // A packet for someone else's channel in between.
            Bytes stray(64, 0);
            putChannel(stray, 0x0A0B0C0D);
            stray[4] = CtapHidTransport::kCommandCbor | 0x80;
            reports_.push_back(stray);
            auto response = authenticator_.sendCbor(message_[0], std::span(message_).subspan(1));
            EXPECT(response);
            respond(channel_, CtapHidTransport::kCommandCbor, *response);
            break;
        }
        case CtapHidTransport::kCommandMsg: {
            EXPECT(channel_ == kChannel);
            // Extended length: CLA INS P1 P2 00 Lc1 Lc2 data 00 00.
            const std::size_t length = (std::size_t{message_[5]} << 8) | message_[6];
            auto response = authenticator_.sendApdu(message_[1], message_[2], message_[3],
                                                    std::span(message_).subspan(7, length));
            EXPECT(response);
            Bytes reply = response->data;
            reply.push_back(static_cast<std::uint8_t>(response->status >> 8));
            reply.push_back(static_cast<std::uint8_t>(response->status));
            respond(channel_, CtapHidTransport::kCommandMsg, reply);
            break;
        }
        default:
            EXPECT(false);
        }
    }

    SoftwareAuthenticator &authenticator_;
    std::deque<Bytes> reports_;
    int command_ = 0;
    std::uint32_t channel_ = 0;
    std::size_t length_ = 0;
    Bytes message_;
    std::uint8_t sequence_ = 0;
};

// An NFC tag's side of ISO 7816 in front of a SoftwareAuthenticator: short APDUs only, so long
// requests come chained, and long responses go out in 61xx parts.
class FakeApduChannel final : public ApduChannel {
public:
    explicit FakeApduChannel(SoftwareAuthenticator &authenticator) : authenticator_(authenticator) {}

    int chainedBlocks = 0;
    int getResponses = 0;
    int keepalives = 0;
    bool selected = false;

    [[nodiscard]] bool supportsExtendedLength() const override { return false; }
    void close() override {}

    std::optional<Bytes> transceive(std::span<const std::uint8_t> apdu) override {
        EXPECT(apdu.size() >= 5);
        const int cla = apdu[0];
        const int ins = apdu[1];
        Bytes data;
        if (apdu.size() > 5) {
            const std::size_t length = apdu[4];
            EXPECT(apdu.size() == 5 + length + 1);
            data.assign(apdu.begin() + 5, apdu.begin() + 5 + static_cast<std::ptrdiff_t>(length));
        }
        if (ins == 0xC0) {
            ++getResponses;
            return nextPart();
        }
        if (cla & 0x10) {
            ++chainedBlocks;
            chain_.insert(chain_.end(), data.begin(), data.end());
            return Bytes{0x90, 0x00};
        }
        data.insert(data.begin(), chain_.begin(), chain_.end());
        chain_.clear();
        if (ins == 0xA4) {
            selected = data == Bytes{0xA0, 0x00, 0x00, 0x06, 0x47, 0x2F, 0x00, 0x01};
            return selected ? Bytes{'F', 'I', 'D', 'O', '_', '2', '_', '0', 0x90, 0x00}
                            : Bytes{0x6A, 0x82};
        }
        EXPECT(selected);
        if (cla == 0x80 && ins == 0x10) {
            auto response = authenticator_.sendCbor(data[0], std::span(data).subspan(1));
            if (!response) {
                // A U2F-only key.
                return Bytes{0x6D, 0x00};
            }
            pending_ = std::move(*response);
            // Still working, once.
            ++keepalives;
            return Bytes{0x91, 0x00};
        }
        if (cla == 0x80 && ins == 0x11) {
            return nextPart();
        }
        if (cla == 0x00 && ins == U2f::kAuthenticate) {
            auto response = authenticator_.sendApdu(ins, apdu[2], apdu[3], data);
            EXPECT(response);
            pending_ = response->data;
            pendingStatus_ = response->status;
            auto part = nextPart();
            pendingStatus_ = 0x9000;
            return part;
        }
        return Bytes{0x6D, 0x00};
    }

private:
    Bytes nextPart() {
        const std::size_t chunk = std::min<std::size_t>(pending_.size(), 40);
        Bytes part(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(chunk));
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(chunk));
        if (pending_.empty()) {
            part.push_back(static_cast<std::uint8_t>(pendingStatus_ >> 8));
            part.push_back(static_cast<std::uint8_t>(pendingStatus_));
        } else {
            part.push_back(0x61);
            part.push_back(static_cast<std::uint8_t>(std::min<std::size_t>(pending_.size(), 255)));
        }
        return part;
    }

    SoftwareAuthenticator &authenticator_;
    Bytes chain_;
    Bytes pending_;
    int pendingStatus_ = 0x9000;
};

const Bytes kData = bytesOf("session id and userauth request");
const std::string kApplication = "ssh:";

std::optional<std::string> noPin(int) {
    EXPECT(!"Unexpected PIN prompt");
    return std::nullopt;
}

KeyResult<SignResult> sign(CtapTransport &transport, std::span<const std::uint8_t> keyHandle,
                           std::uint8_t flags = kUserPresenceRequired,
                           const PinSource &pinSource = noPin) {
    SignRequest request;
    request.algorithm = kAlgorithmEcdsa;
    request.data = kData;
    request.application = kApplication;
    request.keyHandle = keyHandle;
    request.flags = flags;
    return signWithSecurityKey(transport, request, pinSource);
}

// What sshd checks for an sk-ecdsa signature.
void expectVerifies(const SoftwareAuthenticator &authenticator, const SignResult &result) {
    EXPECT(authenticator.credentials.size() == 1);
    const auto &key = authenticator.credentials.begin()->second;
    const Bytes signedData = concat({sha256(bytesOf(kApplication)), Bytes{result.flags},
                                     bigEndian32(result.counter), sha256(kData)});
    EXPECT(ecdsaVerifies(key, signedData, result.signature));
}

void expectFails(const KeyResult<SignResult> &result, KeyError::Kind kind) {
    EXPECT(!result);
    EXPECT(result.error().kind == kind);
}

void testSignsWithCtap2() {
    SoftwareAuthenticator authenticator;
    const Bytes keyHandle = authenticator.addCredential();
    bool touched = false;
    authenticator.onUserPresenceNeeded = [&] { touched = true; };
    const auto first = sign(authenticator, keyHandle);
    EXPECT(first);
    EXPECT(first->flags == 0x01);
    EXPECT(first->counter == 1);
    EXPECT(touched);
    expectVerifies(authenticator, *first);
    EXPECT(sign(authenticator, keyHandle)->counter == 2);
}

void testSignsWithoutTouchWhenKeyAllows() {
    SoftwareAuthenticator authenticator;
    const Bytes keyHandle = authenticator.addCredential();
    const auto result = sign(authenticator, keyHandle, 0);
    EXPECT(result && result->flags == 0);
    expectVerifies(authenticator, *result);
}

void testVerifiesUserWithPinProtocolTwo() {
    SoftwareAuthenticator authenticator({.pin = "1234"});
    const Bytes keyHandle = authenticator.addCredential();
    std::vector<int> prompts;
    const auto result = sign(authenticator, keyHandle,
                             kUserPresenceRequired | kUserVerificationRequired, [&](int retries) {
                                 prompts.push_back(retries);
                                 return std::optional<std::string>(prompts.size() == 1 ? "0000"
                                                                                       : "1234");
                             });
    EXPECT(result);
    // The wrong PIN costs a retry.
    EXPECT((prompts == std::vector{8, 7}));
    EXPECT(result->flags == 0x05);
    expectVerifies(authenticator, *result);
}

void testVerifiesUserWithPinProtocolOneWithoutPermissions() {
    // A CTAP 2.0 key.
    SoftwareAuthenticator authenticator(
        {.pin = "123456", .protocols = {1}, .supportsPermissions = false});
    const Bytes keyHandle = authenticator.addCredential();
    const auto result = sign(authenticator, keyHandle,
                             kUserPresenceRequired | kUserVerificationRequired,
                             [](int) { return std::optional<std::string>("123456"); });
    EXPECT(result && result->flags == 0x05);
    expectVerifies(authenticator, *result);
}

void testAsksForPinWhenKeyAlwaysRequiresIt() {
    SoftwareAuthenticator authenticator({.pin = "1234", .alwaysUv = true});
    const Bytes keyHandle = authenticator.addCredential();
    bool prompted = false;
    const auto result = sign(authenticator, keyHandle, kUserPresenceRequired, [&](int) {
        prompted = true;
        return std::optional<std::string>("1234");
    });
    EXPECT(result && prompted);
    expectVerifies(authenticator, *result);
}

void testReportsWrongKey() {
    SoftwareAuthenticator authenticator;
    authenticator.addCredential();
    expectFails(sign(authenticator, Bytes(64, 0)), KeyError::Kind::kWrongKey);
}

void testReportsCancelledPin() {
    SoftwareAuthenticator authenticator({.pin = "1234"});
    const Bytes keyHandle = authenticator.addCredential();
    expectFails(sign(authenticator, keyHandle, kUserVerificationRequired,
                     [](int) { return std::optional<std::string>(); }),
                KeyError::Kind::kCancelled);
}

void testReportsMissingPin() {
    // User verification required, but no PIN set and nothing built in.
    SoftwareAuthenticator authenticator;
    const Bytes keyHandle = authenticator.addCredential();
    expectFails(sign(authenticator, keyHandle, kUserVerificationRequired),
                KeyError::Kind::kPinRequired);
}

void testReportsBlockedPin() {
    SoftwareAuthenticator authenticator({.pin = "1234"});
    authenticator.pinRetries = 1;
    const Bytes keyHandle = authenticator.addCredential();
    expectFails(sign(authenticator, keyHandle, kUserVerificationRequired,
                     [](int) { return std::optional<std::string>("0000"); }),
                KeyError::Kind::kPinBlocked);
}

void testSignsWithU2f() {
    SoftwareAuthenticator authenticator({.ctap2 = false});
    const Bytes keyHandle = authenticator.addCredential();
    const auto result = sign(authenticator, keyHandle);
    EXPECT(result && result->flags == 0x01);
    expectVerifies(authenticator, *result);
    // It didn't try CTAP2 on a key that said it can't.
    EXPECT(authenticator.commands.empty());
}

void testFallsBackToU2fOverNfc() {
    SoftwareAuthenticator authenticator({.ctap2 = false, .ctap2FailsAsIo = true});
    const Bytes keyHandle = authenticator.addCredential();
    const auto result = sign(authenticator, keyHandle);
    EXPECT(result);
    expectVerifies(authenticator, *result);
    EXPECT((authenticator.commands == std::vector{ctap_command::kGetInfo}));
}

void testRejectsUserVerificationOverU2f() {
    SoftwareAuthenticator authenticator({.ctap2 = false});
    const Bytes keyHandle = authenticator.addCredential();
    expectFails(sign(authenticator, keyHandle, kUserVerificationRequired),
                KeyError::Kind::kUnsupported);
}

void testSignsOverCtapHid() {
    SoftwareAuthenticator authenticator({.pin = "1234"});
    // Long enough that requests and responses span several reports.
    const Bytes keyHandle = authenticator.addCredential(128);
    auto pipe = std::make_unique<FakeHidPipe>(authenticator);
    auto opened = CtapHidTransport::open(std::move(pipe));
    EXPECT(opened);
    auto &transport = **opened;
    EXPECT(transport.mayHaveCtap2());
    int touches = 0;
    transport.onUserPresenceNeeded = [&] { ++touches; };
    const auto result = sign(transport, keyHandle, kUserPresenceRequired | kUserVerificationRequired,
                             [](int) { return std::optional<std::string>("1234"); });
    EXPECT(result && result->flags == 0x05);
    EXPECT(touches == 1);
    expectVerifies(authenticator, *result);
}

void testSignsWithU2fOverCtapHid() {
    SoftwareAuthenticator authenticator({.ctap2 = false});
    const Bytes keyHandle = authenticator.addCredential();
    auto opened = CtapHidTransport::open(std::make_unique<FakeHidPipe>(authenticator));
    EXPECT(opened);
    EXPECT(!(*opened)->mayHaveCtap2());
    const auto result = sign(**opened, keyHandle);
    EXPECT(result && result->flags == 0x01);
    expectVerifies(authenticator, *result);
}

void testClosingCtapHidCancels() {
    SoftwareAuthenticator authenticator;
    const Bytes keyHandle = authenticator.addCredential();
    auto pipe = std::make_unique<FakeHidPipe>(authenticator);
    FakeHidPipe *fake = pipe.get();
    auto opened = CtapHidTransport::open(std::move(pipe));
    EXPECT(opened);
    (*opened)->close();
    expectFails(sign(**opened, keyHandle), KeyError::Kind::kClosed);
    // Nothing was waiting yet: closing before a request writes nothing.
    EXPECT(fake->cancels == 0);
}

void testSignsOverNfcWithChainingAndParts() {
    SoftwareAuthenticator authenticator({.pin = "1234"});
    // Long enough that getAssertion needs chaining in short APDUs.
    const Bytes keyHandle = authenticator.addCredential(300);
    auto channel = std::make_unique<FakeApduChannel>(authenticator);
    FakeApduChannel *fake = channel.get();
    auto opened = NfcTransport::open(std::move(channel));
    EXPECT(opened);
    EXPECT((*opened)->isUserPresent());
    const auto result = sign(**opened, keyHandle, kUserPresenceRequired | kUserVerificationRequired,
                             [](int) { return std::optional<std::string>("1234"); });
    EXPECT(result && result->flags == 0x05);
    expectVerifies(authenticator, *result);
    EXPECT(fake->chainedBlocks > 0);
    EXPECT(fake->getResponses > 0);
    EXPECT(fake->keepalives > 0);
}

void testFallsBackToU2fOverNfcFraming() {
    SoftwareAuthenticator authenticator({.ctap2 = false, .ctap2FailsAsIo = true});
    const Bytes keyHandle = authenticator.addCredential();
    auto opened = NfcTransport::open(std::make_unique<FakeApduChannel>(authenticator));
    EXPECT(opened);
    const auto result = sign(**opened, keyHandle);
    EXPECT(result);
    expectVerifies(authenticator, *result);
}

void testCborEncodesRfc8949Examples() {
    EXPECT(hex(cborEncode(-1)) == "20");
    EXPECT(hex(cborEncode(1000)) == "1903e8");
    EXPECT(hex(cborEncode(-1000)) == "3903e7");
    EXPECT(hex(cborEncode(std::int64_t{1000000000000})) == "1b000000e8d4a51000");
    EXPECT(hex(cborEncode("IETF")) == "6449455446");
    EXPECT(hex(cborEncode(Bytes{1, 2, 3, 4})) == "4401020304");
    EXPECT(hex(cborEncode(CborArray{1, 2, 3})) == "83010203");
    EXPECT(hex(cborEncode(CborMap{{"a", 1}, {"b", CborArray{2, 3}}})) == "a26161016162820203");
    EXPECT(hex(cborEncode(true)) == "f5");
}

void testCborSortsMapKeysCanonically() {
    // Shorter encodings first, then bytewise: 1, -1, "id", "type".
    const Bytes encoded = cborEncode(CborMap{{"type", 0}, {-1, 0}, {"id", 0}, {1, 0}});
    EXPECT(hex(encoded) == "a40100200062696400647479706500");
}

void testCborRoundTrips() {
    const CborValue value = CborMap{
        {1, "rp"}, {2, Bytes{9, 8}}, {3, CborArray{CborMap{{"up", false}}}}, {-25, true}};
    const auto decoded = cborDecode(cborEncode(value));
    EXPECT(decoded);
    EXPECT(*decoded->find(1)->text() == "rp");
    EXPECT((*decoded->find(2)->bytes() == Bytes{9, 8}));
    EXPECT(*decoded->find(3) == CborValue(CborArray{CborMap{{"up", false}}}));
    EXPECT(*decoded->find(-25)->boolean());
}

void testCborRejectsMalformedInput() {
    Bytes truncated = cborEncode(CborArray{1, 2, 3});
    truncated.pop_back();
    EXPECT(!cborDecode(truncated));
    EXPECT(!cborDecode(Bytes{0x01, 0x02}));  // Trailing data.
    // A billion-element array that isn't there.
    EXPECT(!cborDecode(Bytes{0x9A, 0x3B, 0x9A, 0xCA, 0x00}));
    Bytes deep(20, 0x81);
    deep.push_back(0x01);
    EXPECT(!cborDecode(deep));
}

// Known answers computed with OpenSSL, since the software authenticator shares this code and
// wouldn't notice a mistake in it.
void testPinProtocolTwoDerivesKeysWithHkdf() {
    Bytes z(32);
    for (int i = 0; i < 32; ++i) {
        z[i] = static_cast<std::uint8_t>(i + 1);
    }
    // openssl kdf -keylen 32 -kdfopt digest:SHA256 -kdfopt hexkey:<z> -kdfopt hexsalt:<32 zero
    // bytes> -kdfopt info:"CTAP2 HMAC key" (and "CTAP2 AES key") HKDF
    EXPECT(hex(PinUvAuthProtocol::two().kdf(z))
           == "9eb685ea1bd795e05cbfbe28f0ef46f1b89ae3a73161c4d64d1181082e82db67"
              "e56b82398cbb09e2a1b546808a716befa7907d17976b72193c6f909413c73184");
}

void testPinProtocolOneEncryptsWithZeroIv() {
    Bytes z(32);
    for (int i = 0; i < 32; ++i) {
        z[i] = static_cast<std::uint8_t>(i + 1);
    }
    const Bytes key = PinUvAuthProtocol::one().kdf(z);
    EXPECT(hex(key) == "ae216c2ef5247a3782c135efa279a3e4cdc61094270f5d2be58c6204b7a612c9");
    // openssl enc -aes-256-cbc -K <key> -iv 0 -nopad
    EXPECT(hex(PinUvAuthProtocol::one().encrypt(key, unhex("00112233445566778899aabbccddeeff")))
           == "06c031a2b5f961a19471c0102bc173dc");
}

void testPinProtocolTwoEncryptsWithAesHalfAndRandomIv() {
    Bytes z(32);
    for (int i = 0; i < 32; ++i) {
        z[i] = static_cast<std::uint8_t>(i + 1);
    }
    const auto &two = PinUvAuthProtocol::two();
    const Bytes sharedSecret = two.kdf(z);
    const Bytes plaintext = unhex("00112233445566778899aabbccddeeff");
    const Bytes ciphertext = two.encrypt(sharedSecret, plaintext);
    EXPECT(ciphertext.size() == 32);
    EXPECT(*two.decrypt(sharedSecret, ciphertext) == plaintext);
    // Authenticating with the shared secret uses its HMAC half, in full.
    EXPECT(two.authenticate(sharedSecret, plaintext)
           == hmacSha256(std::span(sharedSecret).first(32), plaintext));
}

}  // namespace

int main() {
    testSignsWithCtap2();
    testSignsWithoutTouchWhenKeyAllows();
    testVerifiesUserWithPinProtocolTwo();
    testVerifiesUserWithPinProtocolOneWithoutPermissions();
    testAsksForPinWhenKeyAlwaysRequiresIt();
    testReportsWrongKey();
    testReportsCancelledPin();
    testReportsMissingPin();
    testReportsBlockedPin();
    testSignsWithU2f();
    testFallsBackToU2fOverNfc();
    testRejectsUserVerificationOverU2f();
    testSignsOverCtapHid();
    testSignsWithU2fOverCtapHid();
    testClosingCtapHidCancels();
    testSignsOverNfcWithChainingAndParts();
    testFallsBackToU2fOverNfcFraming();
    testCborEncodesRfc8949Examples();
    testCborSortsMapKeysCanonically();
    testCborRoundTrips();
    testCborRejectsMalformedInput();
    testPinProtocolTwoDerivesKeysWithHkdf();
    testPinProtocolOneEncryptsWithZeroIv();
    testPinProtocolTwoEncryptsWithAesHalfAndRandomIv();
    std::puts("securitykey_test passed");
    return 0;
}
