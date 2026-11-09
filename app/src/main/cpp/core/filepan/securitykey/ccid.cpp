// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/securitykey/ccid.hpp"

#include <chrono>

#include "filepan/securitykey/usbfs.hpp"

namespace filepan::securitykey {

namespace {

using Kind = KeyError::Kind;
using Clock = std::chrono::steady_clock;

// How long a card may stay silent; each request for more time starts it again.
constexpr auto kTimeout = std::chrono::seconds(30);
// Polled in short slices so that closing takes effect soon.
constexpr int kPollMillis = 100;
// Longer than any short-APDU response, with room to spare.
constexpr std::size_t kMaxMessage = 64 * 1024;

// bStatus: bmCommandStatus in its top two bits, bmICCStatus in its lowest two.
constexpr int kCommandFailed = 1;
constexpr int kTimeExtension = 2;

std::uint32_t readLength(std::span<const std::uint8_t> header) {
    return std::uint32_t{header[1]} | (std::uint32_t{header[2]} << 8)
        | (std::uint32_t{header[3]} << 16) | (std::uint32_t{header[4]} << 24);
}

}  // namespace

UsbfsBulkPipe::~UsbfsBulkPipe() {
    if (onClose_) {
        onClose_();
    }
}

bool UsbfsBulkPipe::write(std::span<const std::uint8_t> data) {
    // usbfs takes a mutable buffer either way; an OUT transfer only reads it.
    const std::span<std::uint8_t> buffer(const_cast<std::uint8_t *>(data.data()), data.size());
    return usbfsTransfer(fd_, outEndpoint_, buffer, 1000) == static_cast<int>(data.size());
}

int UsbfsBulkPipe::read(std::span<std::uint8_t> packet, int timeoutMillis) {
    return usbfsTransfer(fd_, inEndpoint_, packet, timeoutMillis);
}

KeyResult<std::unique_ptr<CcidReader>> CcidReader::open(std::unique_ptr<BulkPipe> pipe) {
    if (pipe->packetSize() == 0) {
        return keyFailure(Kind::kIo, "Not a smart card reader");
    }
    std::unique_ptr<CcidReader> reader(new CcidReader(std::move(pipe)));
    // The answer to reset, which says nothing a short-APDU reader needs.
    if (!reader->command(kPcToRdrIccPowerOn, {})) {
        return keyFailure(reader->closed_ ? Kind::kClosed : Kind::kIo, "The card didn't power on");
    }
    return reader;
}

CcidReader::~CcidReader() {
    // Best effort, closed or not: the key may be gone already. Its answer isn't waited for.
    const std::uint8_t header[kHeaderSize]
        = {kPcToRdrIccPowerOff, 0, 0, 0, 0, 0, ++sequence_, 0, 0, 0};
    pipe_->write(header);
}

std::optional<Bytes> CcidReader::transceive(std::span<const std::uint8_t> apdu) {
    return command(kPcToRdrXfrBlock, apdu);
}

std::optional<Bytes> CcidReader::command(std::uint8_t type, std::span<const std::uint8_t> data) {
    if (closed_) {
        return std::nullopt;
    }
    const std::uint8_t sequence = ++sequence_;
    const auto size = static_cast<std::uint32_t>(data.size());
    // bSlot 0; for XfrBlock, bBWI 0 and wLevelParameter 0 (a whole APDU); for IccPowerOn,
    // bPowerSelect 0 (automatic voltage).
    Bytes message{type,
                  static_cast<std::uint8_t>(size),
                  static_cast<std::uint8_t>(size >> 8),
                  static_cast<std::uint8_t>(size >> 16),
                  static_cast<std::uint8_t>(size >> 24),
                  0,
                  sequence,
                  0,
                  0,
                  0};
    message.insert(message.end(), data.begin(), data.end());
    if (!pipe_->write(message)) {
        return std::nullopt;
    }
    while (true) {
        auto answer = receive();
        if (!answer) {
            return std::nullopt;
        }
        // Left over from an exchange that was given up on.
        if ((*answer)[6] != sequence) {
            continue;
        }
        const int status = (*answer)[7] >> 6;
        if (status == kTimeExtension) {
            continue;
        }
        if (status == kCommandFailed || (*answer)[0] != kRdrToPcDataBlock) {
            return std::nullopt;
        }
        answer->erase(answer->begin(), answer->begin() + kHeaderSize);
        return answer;
    }
}

std::optional<Bytes> CcidReader::receive() {
    Bytes message;
    Bytes packet(pipe_->packetSize());
    auto deadline = Clock::now() + kTimeout;
    while (true) {
        if (closed_) {
            return std::nullopt;
        }
        if (Clock::now() >= deadline) {
            return std::nullopt;
        }
        const int got = pipe_->read(packet, kPollMillis);
        if (got < 0) {
            return std::nullopt;
        }
        if (got == 0) {
            continue;
        }
        message.insert(message.end(), packet.begin(), packet.begin() + got);
        if (message.size() < kHeaderSize) {
            continue;
        }
        const std::uint32_t length = readLength(message);
        if (length > kMaxMessage) {
            return std::nullopt;
        }
        if (message.size() >= kHeaderSize + length) {
            message.resize(kHeaderSize + length);
            return message;
        }
        // A long message keeps coming: it isn't waiting on the card.
        deadline = Clock::now() + kTimeout;
    }
}

}  // namespace filepan::securitykey
