// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>

#include "filepan/securitykey/assertion.hpp"
#include "filepan/securitykey/openpgp_card.hpp"
#include "filepan/securitykey/transport.hpp"

// Signing in to an SFTP server with a key whose private half lives on a security key the user
// plugs in or taps: an "sk-" key on its FIDO applet, or the authentication key of its OpenPGP card.
// The signing thread waits on a SecurityKeySession while the platform looks for the key
// (SecurityKeySource) and the UI shows what's going on and asks for the PIN.
namespace filepan::vfs {

struct SecurityKeyPrompt {
    std::string serverId;
    // As in HostKeyPrompt.
    std::size_t hop = 0;
    bool isJumpHost = false;
    std::string host;
    int port = 22;
    std::string username;
    // The SSH key needs the key's PIN (or built-in verification), not only a touch. Always, for
    // signing with an OpenPGP card; never for reading its key.
    bool userVerification = false;
};

// One signing, shared by the signing thread, the platform and the UI. Thread-safe.
class SecurityKeySession {
public:
    // Which applet on the key signs: what the platform opens and offers.
    enum class Kind : std::uint8_t { kFido, kOpenPgpCard };
    enum class Step : std::uint8_t {
        kWaitingForKey,  // Plug it in or hold it to the phone.
        kWorking,
        kTouch,          // Waiting for a touch on the key.
        kPin,
        kDone,           // Signed, failed or cancelled: the prompt closes.
    };
    // Why a previous attempt didn't work, while waiting for the key again.
    enum class Problem : std::uint8_t { kNone, kConnectionLost, kWrongKey, kNotASecurityKey };

    struct State {
        Step step = Step::kWaitingForKey;
        Problem problem = Problem::kNone;
        // For kPin: attempts left before the key locks, or -1 if unknown; and whether the last
        // PIN was wrong.
        int pinRetries = -1;
        bool pinWrong = false;
    };

    static constexpr auto kKeyTimeout = std::chrono::seconds(60);
    static constexpr auto kPinTimeout = std::chrono::seconds(120);

    explicit SecurityKeySession(Kind kind = Kind::kFido) : kind_(kind) {}

    [[nodiscard]] Kind kind() const { return kind_; }

    [[nodiscard]] State state() const;
    // Called on whichever thread changes the state, never with the session locked. Set it
    // before the session is shared.
    void setOnChange(std::function<void()> onChange);

    // --- UI ---
    void submitPin(std::string pin);
    // Gives up, stopping an exchange with the key in progress.
    void cancel();
    [[nodiscard]] bool isCancelled() const;

    // --- Platform ---
    // A connected key, opened as kind() says (a CtapTransport for kFido, an OpenPgpCard for
    // kOpenPgpCard); closed if no longer wanted.
    void offerTransport(std::unique_ptr<securitykey::PresentedKey> transport);
    // Something was presented that isn't a usable key.
    void offerProblem(Problem problem);

    // --- Signing thread ---
    // The next key presented, of the type kind() says; nullptr if cancelled or none came in time.
    std::unique_ptr<securitykey::PresentedKey> awaitTransport(
        std::chrono::steady_clock::duration timeout = kKeyTimeout);
    // Waits for the key again after a failed attempt.
    void retryTransport(Problem problem);
    // Done with the transport awaitTransport() gave.
    void releaseTransport();
    // nullopt if cancelled or none came in time.
    std::optional<std::string> awaitPin(int retries, bool wrong,
                                        std::chrono::steady_clock::duration timeout = kPinTimeout);
    // Closes keys offered but never used, and closes the prompt.
    void finish();

private:
    void setState(State state);
    void notify();

    const Kind kind_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    State state_;
    std::function<void()> onChange_;
    bool cancelled_ = false;
    std::deque<std::unique_ptr<securitykey::PresentedKey>> transports_;
    std::optional<std::string> pin_;
    // The one in use, so cancel() can stop it.
    securitykey::PresentedKey *active_ = nullptr;
};

// Finds the key the user presents: on Android, a USB security key plugged in (once the user lets
// the app use it) or an NFC one held to the phone.
class SecurityKeySource {
public:
    virtual ~SecurityKeySource() = default;
    // Starts looking, offering what turns up to the session, from any thread, until stop().
    virtual void start(std::shared_ptr<SecurityKeySession> session) = 0;
    virtual void stop() = 0;
};

// Signs with whichever FIDO key the user presents through a kFido session, trying again (up to three
// times) when the key is lost midway or turns out to be the wrong one. Keeps the PIN for a
// second tap over NFC.
securitykey::KeyResult<securitykey::SignResult> signWithPresentedKey(
    SecurityKeySession &session, const securitykey::SignRequest &request);

// PINs of OpenPGP cards that worked, by card serial, kept in memory for a while after their last
// use as gpg-agent does, so that each login doesn't ask again. Thread-safe.
class CardPinCache {
public:
    static constexpr auto kLifetime = std::chrono::minutes(10);

    explicit CardPinCache(std::chrono::steady_clock::duration lifetime = kLifetime)
        : lifetime_(lifetime) {}

    // Counts as a use.
    std::optional<std::string> find(const std::string &serial);
    void remember(const std::string &serial, std::string pin);
    void forget(const std::string &serial);

private:
    struct Entry {
        std::string pin;
        std::chrono::steady_clock::time_point used;
    };

    std::chrono::steady_clock::duration lifetime_;
    std::mutex mutex_;
    std::map<std::string, Entry> pins_;
};

// Signs as ssh-agent would with the authentication key of the OpenPGP card the user presents
// through a kOpenPgpCard session, which must be publicKey (an SSH public key blob): the SSH
// signature blob. Uses the card's remembered PIN, or asks. Tries again (up to three times) when
// the card is lost midway or turns out to be the wrong one, keeping the PIN typed for a second
// tap over NFC.
securitykey::KeyResult<securitykey::Bytes> signWithPresentedCard(
    SecurityKeySession &session, std::span<const std::uint8_t> publicKey,
    std::span<const std::uint8_t> data, std::uint32_t flags, CardPinCache &pins);

struct PresentedCardKey {
    // The authentication key's SSH public key blob.
    securitykey::Bytes publicKey;
    // As OpenPgpCard::serial().
    std::string serial;
};

// Reads the authentication key of the OpenPGP card the user presents through a kOpenPgpCard
// session, without its PIN. kUnsupported if it has none SSH can use.
securitykey::KeyResult<PresentedCardKey> readPresentedCardKey(SecurityKeySession &session);

}  // namespace filepan::vfs
