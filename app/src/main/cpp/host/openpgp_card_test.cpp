// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// The OpenPGP card client against a software card, over ISO-DEP and over a CCID reader.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <thread>

#include <libssh/libssh.h>

#include "filepan/securitykey/ccid.hpp"
#include "filepan/securitykey/openpgp_card.hpp"
#include "software_openpgp_card.hpp"
#include "test_util.hpp"

namespace {

using namespace filepan::securitykey;
using filepan::test::CardLink;
using filepan::test::CardOptions;
using filepan::test::SoftwareOpenPgpCard;
using Algorithm = OpenPgpCard::Algorithm;
using Kind = KeyError::Kind;

std::string hex(std::span<const std::uint8_t> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t byte : bytes) {
        out += kDigits[byte >> 4];
        out += kDigits[byte & 0xF];
    }
    return out;
}

// What an SSH client asks an agent to sign: long enough that a short APDU can't carry it.
Bytes sessionData(std::size_t size = 300) {
    Bytes data(size);
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<std::uint8_t>(i * 7);
    }
    return data;
}

std::unique_ptr<OpenPgpCard> openCard(SoftwareOpenPgpCard &card, bool extendedLength = false,
                                      bool userPresent = true) {
    auto opened = OpenPgpCard::open(std::make_unique<CardLink>(card, extendedLength), userPresent);
    EXPECT(opened.has_value());
    return std::move(*opened);
}

void testSignsWithEachKeyType() {
    const struct {
        Algorithm algorithm;
        std::uint32_t flags;
        const char *type;
        const char *signatureType;
    } cases[] = {
        {Algorithm::kEd25519, 0, "ssh-ed25519", "ssh-ed25519"},
        {Algorithm::kEcdsaP256, 0, "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp256"},
        {Algorithm::kEcdsaP384, 0, "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp384"},
        {Algorithm::kRsa, agent_flags::kRsaSha2_256, "ssh-rsa", "rsa-sha2-256"},
        {Algorithm::kRsa, agent_flags::kRsaSha2_512, "ssh-rsa", "rsa-sha2-512"},
        {Algorithm::kRsa, 0, "ssh-rsa", "ssh-rsa"},
    };
    for (const auto &c : cases) {
        SoftwareOpenPgpCard card(CardOptions{.algorithm = c.algorithm});
        auto client = openCard(card);
        EXPECT(client->algorithm() == c.algorithm);
        EXPECT(client->serial() == "000612345678");
        EXPECT(client->pinRetries() == 3);
        EXPECT(!client->touchRequired());
        auto key = client->authenticationKey();
        EXPECT(key.has_value());
        EXPECT(*key == card.publicBlob());
        EXPECT(sshKeyType(*key) == c.type);
        EXPECT(client->verifyPin("123456").has_value());
        const Bytes data = sessionData();
        auto signature = client->signSsh(data, c.flags);
        EXPECT(signature.has_value());
        EXPECT(sshKeyType(*signature) == c.signatureType);
        EXPECT(card.verifiesSsh(data, *signature));
        EXPECT(!card.verifiesSsh(sessionData(299), *signature));
    }
}

void testChainsLongCommandsAndCollectsLongResponses() {
    SoftwareOpenPgpCard card(CardOptions{.algorithm = Algorithm::kRsa});
    auto client = openCard(card);
    // The 2048-bit key's public key object and signature are longer than one short response.
    EXPECT(client->authenticationKey().has_value());
    EXPECT(card.getResponses > 0);
    EXPECT(client->verifyPin("123456").has_value());
    EXPECT(client->signSsh(sessionData(), agent_flags::kRsaSha2_256).has_value());
    EXPECT(card.extendedCommands == 0);

    // Ed25519 signs the data itself, which needs chaining.
    SoftwareOpenPgpCard ed(CardOptions{});
    auto edClient = openCard(ed);
    EXPECT(edClient->verifyPin("123456").has_value());
    EXPECT(edClient->signSsh(sessionData(600), 0).has_value());
    EXPECT(ed.chainedBlocks == 2);
    EXPECT(ed.extendedCommands == 0);

    // Or extended length, where the link has it.
    SoftwareOpenPgpCard extended(CardOptions{});
    auto extendedClient = openCard(extended, true);
    EXPECT(extendedClient->verifyPin("123456").has_value());
    auto signature = extendedClient->signSsh(sessionData(600), 0);
    EXPECT(signature.has_value());
    EXPECT(extended.verifiesSsh(sessionData(600), *signature));
    EXPECT(extended.extendedCommands == 1);
    EXPECT(extended.chainedBlocks == 0);
}

void testReportsWrongAndBlockedPins() {
    SoftwareOpenPgpCard card(CardOptions{});
    auto client = openCard(card);
    auto wrong = client->verifyPin("654321");
    EXPECT(!wrong && wrong.error().kind == Kind::kPinWrong && wrong.error().status == 2);
    EXPECT(client->pinRetries() == 2);
    EXPECT(client->verifyPin("123456").has_value());
    EXPECT(card.pinRetries == 3);
    // VERIFY is case 3: no Le.
    EXPECT(!card.verifyHadLe);
    EXPECT(card.verifyData.back() == Bytes({'1', '2', '3', '4', '5', '6'}));

    EXPECT(client->verifyPin("000000").error().status == 2);
    EXPECT(client->verifyPin("000000").error().status == 1);
    auto blocked = client->verifyPin("000000");
    EXPECT(!blocked && blocked.error().kind == Kind::kPinBlocked);
    auto stillBlocked = client->verifyPin("123456");
    EXPECT(!stillBlocked && stillBlocked.error().kind == Kind::kPinBlocked);
}

void testHashesThePinWhenTheCardHasAKdf() {
    SoftwareOpenPgpCard card(CardOptions{.kdf = true});
    auto client = openCard(card);
    EXPECT(client->verifyPin("123456").has_value());
    // hashlib: sha256(((salt + pin) * n)[:100000]) with salt 01..08.
    EXPECT(hex(card.verifyData.back())
           == "8fc77436ded3825377da921acc4fb1e0c619e426b926c30f06fbb428c6ac9701");
    EXPECT(!client->verifyPin("12345678").has_value());
}

void testIteratedSaltedS2kMatchesKnownAnswers() {
    const std::uint8_t salt[] = {1, 2, 3, 4, 5, 6, 7, 8};
    EXPECT(hex(iteratedSaltedS2k(8, salt, 100000, "123456"))
           == "8fc77436ded3825377da921acc4fb1e0c619e426b926c30f06fbb428c6ac9701");
    EXPECT(hex(iteratedSaltedS2k(10, salt, 100000, "123456"))
           == "186d660008735555e233e307a33bea690644c7e0c4c0c14cea8dfb083f957daba1b48661088782001802"
              "9d8b1a6877866746777abf"
              "7a139a607c9a13f2cd67b3");
    // A count below the salt and secret's length hashes them once.
    EXPECT(hex(iteratedSaltedS2k(8, salt, 3, "123456"))
           == "1ae0f1043e1492a1acffc2319de52fa4713cf4bfd7123563345550f0da8eee9a");
    EXPECT(iteratedSaltedS2k(2, salt, 3, "123456").empty());
}

void testReportsKeysSshCantUse() {
    SoftwareOpenPgpCard none(CardOptions{.hasKey = false});
    auto noneClient = openCard(none);
    EXPECT(noneClient->algorithm() == Algorithm::kNone);
    auto missing = noneClient->authenticationKey();
    EXPECT(!missing && missing.error().kind == Kind::kUnsupported);

    SoftwareOpenPgpCard brainpool(
        CardOptions{.algorithm = Algorithm::kEcdsaP256, .brainpool = true});
    auto brainpoolClient = openCard(brainpool);
    EXPECT(brainpoolClient->algorithm() == Algorithm::kOther);
    auto unusable = brainpoolClient->authenticationKey();
    EXPECT(!unusable && unusable.error().kind == Kind::kUnsupported);
    EXPECT(!brainpoolClient->signSsh(sessionData(), 0).has_value());
}

// A tag with only other applets on it.
class OtherTag final : public ApduChannel {
public:
    [[nodiscard]] bool supportsExtendedLength() const override { return false; }
    std::optional<Bytes> transceive(std::span<const std::uint8_t>) override {
        return Bytes{0x6A, 0x82};
    }
    void close() override {}
};

void testRejectsACardWithoutTheApplet() {
    auto opened = OpenPgpCard::open(std::make_unique<OtherTag>(), true);
    EXPECT(!opened && opened.error().kind == Kind::kIo);
}

void testAsksForATouchOnlyWhenTheKeyWantsOne() {
    for (const bool userPresent : {false, true}) {
        SoftwareOpenPgpCard card(CardOptions{.touch = true});
        auto client = openCard(card, false, userPresent);
        EXPECT(client->touchRequired());
        int touches = 0;
        client->onUserPresenceNeeded = [&] { ++touches; };
        EXPECT(client->verifyPin("123456").has_value());
        EXPECT(client->signSsh(sessionData(), 0).has_value());
        EXPECT(touches == (userPresent ? 0 : 1));
    }
    SoftwareOpenPgpCard card(CardOptions{});
    auto client = openCard(card, false, false);
    int touches = 0;
    client->onUserPresenceNeeded = [&] { ++touches; };
    EXPECT(client->verifyPin("123456").has_value());
    EXPECT(client->signSsh(sessionData(), 0).has_value());
    EXPECT(touches == 0);
}

void testSigningWithoutThePinFails() {
    SoftwareOpenPgpCard card(CardOptions{});
    auto client = openCard(card);
    auto signature = client->signSsh(sessionData(), 0);
    EXPECT(!signature && signature.error().kind == Kind::kIo);
}

void testClosingStopsExchanges() {
    SoftwareOpenPgpCard card(CardOptions{});
    auto client = openCard(card);
    client->close();
    EXPECT(client->isClosed());
    auto key = client->authenticationKey();
    EXPECT(!key && key.error().kind == Kind::kClosed);
}

// A YubiKey's CCID interface in front of a software card: 64-byte packets, and, if asked, a
// request for more time before each answer and a stale message before that.
class FakeCcidPipe final : public BulkPipe {
public:
    explicit FakeCcidPipe(SoftwareOpenPgpCard &card) : card_(card) {}

    int timeExtensions = 0;
    bool askForTime = false;
    bool sendStale = false;
    // Answers nothing, as a key waiting for a touch that never comes.
    std::atomic<bool> silent = false;
    bool poweredOn = false;
    bool poweredOff = false;

    [[nodiscard]] std::size_t packetSize() const override { return 64; }

    bool write(std::span<const std::uint8_t> message) override {
        EXPECT(message.size() >= 10);
        const std::uint32_t length = std::uint32_t{message[1]} | (std::uint32_t{message[2]} << 8)
            | (std::uint32_t{message[3]} << 16) | (std::uint32_t{message[4]} << 24);
        EXPECT(message.size() == 10 + length);
        EXPECT(message[5] == 0);
        const std::uint8_t sequence = message[6];
        const auto data = message.subspan(10);
        switch (message[0]) {
        case CcidReader::kPcToRdrIccPowerOn:
            poweredOn = true;
            queue(0x80, sequence, 0x00, Bytes{0x3B, 0xF8, 0x13, 0x00, 0x00, 0x81, 0x31});
            break;
        case CcidReader::kPcToRdrIccPowerOff:
            poweredOff = true;
            queue(0x81, sequence, 0x01, {});
            break;
        case CcidReader::kPcToRdrXfrBlock:
            EXPECT(poweredOn);
            if (silent) {
                break;
            }
            if (sendStale) {
                queue(0x80, static_cast<std::uint8_t>(sequence - 1), 0x00, Bytes{0x6F, 0x00});
            }
            if (askForTime) {
                ++timeExtensions;
                queue(0x80, sequence, 0x80, {});
            }
            queue(0x80, sequence, 0x00, card_.process(data));
            break;
        default: EXPECT(false);
        }
        return true;
    }

    int read(std::span<std::uint8_t> packet, int timeoutMillis) override {
        if (packets_.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeoutMillis, 5)));
            return 0;
        }
        const Bytes next = std::move(packets_.front());
        packets_.pop_front();
        EXPECT(next.size() <= packet.size());
        std::ranges::copy(next, packet.begin());
        return static_cast<int>(next.size());
    }

private:
    void queue(std::uint8_t type, std::uint8_t sequence, std::uint8_t status,
               std::span<const std::uint8_t> data) {
        const auto length = static_cast<std::uint32_t>(data.size());
        Bytes message{type,
                      static_cast<std::uint8_t>(length),
                      static_cast<std::uint8_t>(length >> 8),
                      static_cast<std::uint8_t>(length >> 16),
                      static_cast<std::uint8_t>(length >> 24),
                      0,
                      sequence,
                      status,
                      0,
                      0};
        message.insert(message.end(), data.begin(), data.end());
        for (std::size_t at = 0; at < message.size(); at += 64) {
            const std::size_t end = std::min(at + 64, message.size());
            packets_.emplace_back(message.begin() + static_cast<std::ptrdiff_t>(at),
                                  message.begin() + static_cast<std::ptrdiff_t>(end));
        }
    }

    SoftwareOpenPgpCard &card_;
    std::deque<Bytes> packets_;
};

void testSignsOverCcid() {
    SoftwareOpenPgpCard card(CardOptions{.algorithm = Algorithm::kRsa});
    auto pipe = std::make_unique<FakeCcidPipe>(card);
    FakeCcidPipe *fake = pipe.get();
    fake->askForTime = true;
    fake->sendStale = true;
    {
        auto reader = CcidReader::open(std::move(pipe));
        EXPECT(reader.has_value());
        EXPECT(fake->poweredOn);
        auto opened = OpenPgpCard::open(std::move(*reader), false);
        EXPECT(opened.has_value());
        auto &client = **opened;
        auto key = client.authenticationKey();
        EXPECT(key && *key == card.publicBlob());
        EXPECT(client.verifyPin("123456").has_value());
        const Bytes data = sessionData();
        auto signature = client.signSsh(data, agent_flags::kRsaSha2_512);
        EXPECT(signature.has_value());
        EXPECT(card.verifiesSsh(data, *signature));
        EXPECT(fake->timeExtensions > 3);
        EXPECT(!fake->poweredOff);
    }
    // Closing the card powers it off.
    EXPECT(fake->poweredOff);
}

void testClosingCcidStopsAWait() {
    SoftwareOpenPgpCard card(CardOptions{});
    auto pipe = std::make_unique<FakeCcidPipe>(card);
    FakeCcidPipe *fake = pipe.get();
    auto reader = CcidReader::open(std::move(pipe));
    EXPECT(reader.has_value());
    auto opened = OpenPgpCard::open(std::move(*reader), false);
    EXPECT(opened.has_value());
    auto &client = **opened;
    EXPECT(client.verifyPin("123456").has_value());
    fake->silent = true;
    std::thread closer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        client.close();
    });
    const auto start = std::chrono::steady_clock::now();
    auto signature = client.signSsh(sessionData(), 0);
    closer.join();
    EXPECT(!signature && signature.error().kind == Kind::kClosed);
    EXPECT(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
}

}  // namespace

int main() {
    // The software card's Ed25519 keys come from libssh's random generator.
    EXPECT(ssh_init() == SSH_OK);
    testSignsWithEachKeyType();
    testChainsLongCommandsAndCollectsLongResponses();
    testReportsWrongAndBlockedPins();
    testHashesThePinWhenTheCardHasAKdf();
    testIteratedSaltedS2kMatchesKnownAnswers();
    testReportsKeysSshCantUse();
    testRejectsACardWithoutTheApplet();
    testAsksForATouchOnlyWhenTheKeyWantsOne();
    testSigningWithoutThePinFails();
    testClosingStopsExchanges();
    testSignsOverCcid();
    testClosingCcidStopsAWait();
    std::puts("openpgp_card_test passed");
    ssh_finalize();
    return 0;
}
