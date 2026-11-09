// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/vfs/security_key.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace filepan::vfs {

namespace {

using securitykey::CtapTransport;
using securitykey::PresentedKey;
using securitykey::KeyError;
using Kind = KeyError::Kind;

constexpr int kMaxAttempts = 3;

}  // namespace

SecurityKeySession::State SecurityKeySession::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

void SecurityKeySession::setOnChange(std::function<void()> onChange) {
    std::lock_guard lock(mutex_);
    onChange_ = std::move(onChange);
}

void SecurityKeySession::notify() {
    std::function<void()> onChange;
    {
        std::lock_guard lock(mutex_);
        onChange = onChange_;
    }
    if (onChange) {
        onChange();
    }
}

void SecurityKeySession::setState(State state) {
    {
        std::lock_guard lock(mutex_);
        state_ = state;
    }
    notify();
}

void SecurityKeySession::submitPin(std::string pin) {
    std::lock_guard lock(mutex_);
    pin_ = std::move(pin);
    condition_.notify_all();
}

void SecurityKeySession::cancel() {
    {
        std::lock_guard lock(mutex_);
        cancelled_ = true;
        // Unblocks a transport waiting for a touch.
        if (active_) {
            active_->close();
        }
        condition_.notify_all();
    }
}

bool SecurityKeySession::isCancelled() const {
    std::lock_guard lock(mutex_);
    return cancelled_;
}

void SecurityKeySession::offerTransport(std::unique_ptr<PresentedKey> transport) {
    std::unique_lock lock(mutex_);
    if (cancelled_ || state_.step != Step::kWaitingForKey) {
        lock.unlock();
        transport->close();
        return;
    }
    transports_.push_back(std::move(transport));
    condition_.notify_all();
}

void SecurityKeySession::offerProblem(Problem problem) {
    {
        std::lock_guard lock(mutex_);
        if (state_.step != Step::kWaitingForKey) {
            return;
        }
        state_.problem = problem;
    }
    notify();
}

std::unique_ptr<PresentedKey> SecurityKeySession::awaitTransport(
    std::chrono::steady_clock::duration timeout) {
    std::unique_ptr<PresentedKey> transport;
    {
        std::unique_lock lock(mutex_);
        if (!condition_.wait_for(lock, timeout,
                                 [&] { return cancelled_ || !transports_.empty(); })
            || cancelled_) {
            return nullptr;
        }
        transport = std::move(transports_.front());
        transports_.pop_front();
        active_ = transport.get();
        // A FIDO key over USB always wants a touch; a card only if its touch policy says so, which
        // it reports when it gets there.
        state_ = State{transport->isUserPresent() || kind_ == Kind::kOpenPgpCard ? Step::kWorking
                                                                                 : Step::kTouch};
    }
    transport->onUserPresenceNeeded = [this] {
        {
            std::lock_guard lock(mutex_);
            if (state_.step == Step::kTouch) {
                return;
            }
            state_ = State{Step::kTouch};
        }
        notify();
    };
    notify();
    return transport;
}

void SecurityKeySession::releaseTransport() {
    std::lock_guard lock(mutex_);
    active_ = nullptr;
}

void SecurityKeySession::retryTransport(Problem problem) {
    {
        std::lock_guard lock(mutex_);
        active_ = nullptr;
    }
    setState(State{Step::kWaitingForKey, problem});
}

std::optional<std::string> SecurityKeySession::awaitPin(int retries, bool wrong,
                                                        std::chrono::steady_clock::duration timeout) {
    State previous;
    {
        std::lock_guard lock(mutex_);
        previous = state_;
        state_ = State{Step::kPin, Problem::kNone, retries, wrong};
        pin_.reset();
    }
    notify();
    std::optional<std::string> pin;
    {
        std::unique_lock lock(mutex_);
        if (condition_.wait_for(lock, timeout, [&] { return cancelled_ || pin_.has_value(); })
            && !cancelled_) {
            pin = std::move(pin_);
            pin_.reset();
        }
    }
    setState(previous.step == Step::kWaitingForKey ? previous : State{Step::kWorking});
    return pin;
}

void SecurityKeySession::finish() {
    std::deque<std::unique_ptr<PresentedKey>> unused;
    {
        std::lock_guard lock(mutex_);
        unused = std::move(transports_);
        transports_.clear();
        active_ = nullptr;
        state_ = State{Step::kDone};
    }
    for (auto &transport : unused) {
        transport->close();
    }
    unused.clear();
    notify();
}

securitykey::KeyResult<securitykey::SignResult> signWithPresentedKey(
    SecurityKeySession &session, const securitykey::SignRequest &request) {
    // Over NFC, the key may need tapping again after the PIN is typed; keep the PIN for that.
    std::optional<std::string> pin;
    // Whether the kept PIN was handed out in this attempt; asked again, it was wrong.
    bool pinUsed = false;
    const securitykey::PinSource pinSource = [&](int retries) -> std::optional<std::string> {
        if (pin && !pinUsed) {
            pinUsed = true;
            return pin;
        }
        pin = session.awaitPin(retries, pinUsed);
        pinUsed = pin.has_value();
        return pin;
    };
    int attempt = 0;
    while (true) {
        auto transport = session.awaitTransport();
        if (!transport) {
            return securitykey::keyFailure(Kind::kCancelled);
        }
        // A kFido session only offers CtapTransports.
        auto result =
            securitykey::signWithSecurityKey(static_cast<CtapTransport &>(*transport), request, pinSource);
        session.releaseTransport();
        transport->close();
        transport.reset();
        if (result) {
            return result;
        }
        const KeyError &error = result.error();
        if (session.isCancelled()) {
            return securitykey::keyFailure(Kind::kCancelled);
        }
        if (error.kind == Kind::kWrongKey) {
            if (++attempt < kMaxAttempts) {
                session.retryTransport(SecurityKeySession::Problem::kWrongKey);
                continue;
            }
            return result;
        }
        if (error.kind != Kind::kIo || ++attempt >= kMaxAttempts) {
            return result;
        }
        // E.g. the key left the NFC field.
        pinUsed = false;
        session.retryTransport(SecurityKeySession::Problem::kConnectionLost);
    }
}

std::optional<std::string> CardPinCache::find(const std::string &serial) {
    std::lock_guard lock(mutex_);
    const auto found = pins_.find(serial);
    if (found == pins_.end()) {
        return std::nullopt;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - found->second.used >= lifetime_) {
        pins_.erase(found);
        return std::nullopt;
    }
    found->second.used = now;
    return found->second.pin;
}

void CardPinCache::remember(const std::string &serial, std::string pin) {
    std::lock_guard lock(mutex_);
    pins_[serial] = Entry{std::move(pin), std::chrono::steady_clock::now()};
}

void CardPinCache::forget(const std::string &serial) {
    std::lock_guard lock(mutex_);
    pins_.erase(serial);
}

securitykey::KeyResult<securitykey::Bytes> signWithPresentedCard(
    SecurityKeySession &session, std::span<const std::uint8_t> publicKey,
    std::span<const std::uint8_t> data, std::uint32_t flags, CardPinCache &pins) {
    // Over NFC, the card may need tapping again after the PIN is typed; keep the PIN for that.
    std::optional<std::string> entered;
    bool wrong = false;
    int attempt = 0;
    while (true) {
        auto presented = session.awaitTransport();
        if (!presented) {
            return securitykey::keyFailure(Kind::kCancelled);
        }
        // A kOpenPgpCard session only offers OpenPgpCards.
        auto &card = static_cast<securitykey::OpenPgpCard &>(*presented);
        auto result = [&]() -> securitykey::KeyResult<securitykey::Bytes> {
            auto key = card.authenticationKey();
            if (!key) {
                return key.error().kind == Kind::kUnsupported
                           ? securitykey::keyFailure(Kind::kWrongKey)
                           : std::unexpected(std::move(key.error()));
            }
            if (!std::ranges::equal(*key, publicKey)) {
                return securitykey::keyFailure(Kind::kWrongKey);
            }
            while (true) {
                const bool typed = entered.has_value();
                std::optional<std::string> pin = typed ? entered : pins.find(card.serial());
                if (!pin) {
                    pin = session.awaitPin(card.pinRetries(), wrong);
                    if (!pin) {
                        return securitykey::keyFailure(Kind::kCancelled);
                    }
                    entered = pin;
                }
                auto verified = card.verifyPin(*pin);
                if (verified) {
                    pins.remember(card.serial(), std::move(*pin));
                    break;
                }
                if (verified.error().kind != Kind::kPinWrong) {
                    return std::unexpected(std::move(verified.error()));
                }
                pins.forget(card.serial());
                // A remembered PIN that stopped working (it was changed) wasn't the user's typo.
                wrong = entered.has_value();
                entered.reset();
            }
            return card.signSsh(data, flags);
        }();
        session.releaseTransport();
        presented->close();
        presented.reset();
        if (result) {
            return result;
        }
        if (session.isCancelled()) {
            return securitykey::keyFailure(Kind::kCancelled);
        }
        const Kind kind = result.error().kind;
        if ((kind != Kind::kWrongKey && kind != Kind::kIo) || ++attempt >= kMaxAttempts) {
            return result;
        }
        // E.g. the card left the NFC field.
        session.retryTransport(kind == Kind::kWrongKey ? SecurityKeySession::Problem::kWrongKey
                                                       : SecurityKeySession::Problem::kConnectionLost);
    }
}

securitykey::KeyResult<PresentedCardKey> readPresentedCardKey(SecurityKeySession &session) {
    int attempt = 0;
    while (true) {
        auto presented = session.awaitTransport();
        if (!presented) {
            return securitykey::keyFailure(Kind::kCancelled);
        }
        auto &card = static_cast<securitykey::OpenPgpCard &>(*presented);
        auto key = card.authenticationKey();
        std::string serial = card.serial();
        session.releaseTransport();
        presented->close();
        presented.reset();
        if (key) {
            return PresentedCardKey{std::move(*key), std::move(serial)};
        }
        if (session.isCancelled()) {
            return securitykey::keyFailure(Kind::kCancelled);
        }
        if (key.error().kind != Kind::kIo || ++attempt >= kMaxAttempts) {
            return std::unexpected(std::move(key.error()));
        }
        session.retryTransport(SecurityKeySession::Problem::kConnectionLost);
    }
}

}  // namespace filepan::vfs
