// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "filepan/securitykey/transport.hpp"

// ISO 7816-4 APDUs to a smart card applet: over NFC (ISO-DEP) or a USB CCID reader.
namespace filepan::securitykey {

// A link to a card, one APDU at a time: on Android, IsoDep; over USB, a CcidReader.
class ApduChannel {
public:
    virtual ~ApduChannel() = default;

    [[nodiscard]] virtual bool supportsExtendedLength() const = 0;
    // The response APDU, status word included; nullopt if the card was lost.
    virtual std::optional<Bytes> transceive(std::span<const std::uint8_t> apdu) = 0;
    // Breaks a transceive in progress, from any thread.
    virtual void close() = 0;
};

namespace iso7816 {
inline constexpr int kSwNoError = 0x9000;
inline constexpr int kSw1MoreData = 0x61;
}  // namespace iso7816

// Sends commands over an ApduChannel: chains a long command, or sends it with extended length
// where the channel takes that, and collects a response that comes in parts (61xx).
class ApduSession {
public:
    explicit ApduSession(std::unique_ptr<ApduChannel> channel) : channel_(std::move(channel)) {}

    // expectData false sends no Le, for a command that only answers with a status (VERIFY).
    KeyResult<ApduResponse> transmit(int cla, int ins, int p1, int p2,
                                     std::span<const std::uint8_t> data, bool expectData = true);
    // Stops an exchange in progress, from any thread; everything after fails with kClosed.
    void close();
    [[nodiscard]] bool isClosed() const { return closed_; }

private:
    KeyResult<ApduResponse> exchange(std::span<const std::uint8_t> apdu);

    std::unique_ptr<ApduChannel> channel_;
    std::atomic<bool> closed_ = false;
};

}  // namespace filepan::securitykey
