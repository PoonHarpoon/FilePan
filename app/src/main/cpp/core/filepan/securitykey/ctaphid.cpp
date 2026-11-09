// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/securitykey/ctaphid.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "filepan/securitykey/pin_protocol.hpp"
#include "filepan/securitykey/usbfs.hpp"

namespace filepan::securitykey {

namespace {

using Kind = KeyError::Kind;
using Clock = std::chrono::steady_clock;

constexpr auto kTimeout = std::chrono::seconds(30);
// Polled in short slices so that closing takes effect soon.
constexpr int kPollMillis = 100;
constexpr std::size_t kMaxMessage = 7609;

std::uint32_t readChannel(std::span<const std::uint8_t> packet) {
    return (std::uint32_t{packet[0]} << 24) | (std::uint32_t{packet[1]} << 16)
           | (std::uint32_t{packet[2]} << 8) | packet[3];
}

std::string hexByte(int value) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    return {'0', 'x', kDigits[(value >> 4) & 0xF], kDigits[value & 0xF]};
}

}  // namespace

UsbfsHidPipe::~UsbfsHidPipe() {
    if (onClose_) {
        onClose_();
    }
}

bool UsbfsHidPipe::write(std::span<const std::uint8_t> packet) {
    // usbfs takes a mutable buffer either way; an OUT transfer only reads it.
    const std::span<std::uint8_t> data(const_cast<std::uint8_t *>(packet.data()), packet.size());
    return usbfsTransfer(fd_, outEndpoint_, data, 1000) == static_cast<int>(packet.size());
}

int UsbfsHidPipe::read(std::span<std::uint8_t> packet, int timeoutMillis) {
    return usbfsTransfer(fd_, inEndpoint_, packet, timeoutMillis);
}

KeyResult<std::unique_ptr<CtapHidTransport>> CtapHidTransport::open(std::unique_ptr<HidPipe> pipe) {
    if (pipe->packetSize() < 64) {
        return keyFailure(Kind::kIo, "Not a security key");
    }
    std::unique_ptr<CtapHidTransport> transport(new CtapHidTransport(std::move(pipe)));
    if (auto initialized = transport->initialize(); !initialized) {
        return std::unexpected(std::move(initialized.error()));
    }
    return transport;
}

KeyResult<void> CtapHidTransport::initialize() {
    std::uint8_t nonce[8];
    fillRandom(nonce);
    if (auto sent = sendMessage(kCommandInit, nonce); !sent) {
        return sent;
    }
    while (true) {
        auto response = receiveMessage(kCommandInit);
        if (!response) {
            return std::unexpected(std::move(response.error()));
        }
        // Another client's reply on the broadcast channel.
        if (response->size() < 17 || !std::ranges::equal(std::span(*response).first(8), nonce)) {
            continue;
        }
        channel_ = readChannel(std::span(*response).subspan(8, 4));
        capabilities_ = (*response)[16];
        return {};
    }
}

KeyResult<Bytes> CtapHidTransport::sendCbor(int command, std::span<const std::uint8_t> data) {
    Bytes message;
    message.reserve(1 + data.size());
    message.push_back(static_cast<std::uint8_t>(command));
    message.insert(message.end(), data.begin(), data.end());
    return transact(kCommandCbor, message);
}

KeyResult<ApduResponse> CtapHidTransport::sendApdu(int ins, int p1, int p2,
                                                   std::span<const std::uint8_t> data) {
    // Extended length encoding, as U2F over HID uses.
    Bytes apdu{0,
               static_cast<std::uint8_t>(ins),
               static_cast<std::uint8_t>(p1),
               static_cast<std::uint8_t>(p2),
               0,
               static_cast<std::uint8_t>(data.size() >> 8),
               static_cast<std::uint8_t>(data.size())};
    apdu.insert(apdu.end(), data.begin(), data.end());
    apdu.push_back(0);
    apdu.push_back(0);
    auto response = transact(kCommandMsg, apdu);
    if (!response) {
        return std::unexpected(std::move(response.error()));
    }
    if (response->size() < 2) {
        return keyFailure(Kind::kIo, "Malformed APDU response");
    }
    const std::size_t end = response->size() - 2;
    ApduResponse result;
    result.status = ((*response)[end] << 8) | (*response)[end + 1];
    response->resize(end);
    result.data = std::move(*response);
    return result;
}

KeyResult<Bytes> CtapHidTransport::transact(int command, std::span<const std::uint8_t> data) {
    if (auto sent = sendMessage(command, data); !sent) {
        return std::unexpected(std::move(sent.error()));
    }
    return receiveMessage(command);
}

void CtapHidTransport::putChannel(std::span<std::uint8_t> packet) const {
    for (int i = 0; i < 4; ++i) {
        packet[i] = static_cast<std::uint8_t>(channel_ >> (24 - 8 * i));
    }
}

KeyResult<void> CtapHidTransport::sendMessage(int command, std::span<const std::uint8_t> data) {
    if (data.size() > kMaxMessage) {
        return keyFailure(Kind::kIo, "Message too long");
    }
    const std::size_t packetSize = pipe_->packetSize();
    std::vector<std::uint8_t> packet(packetSize, 0);
    putChannel(packet);
    packet[4] = static_cast<std::uint8_t>(command | 0x80);
    packet[5] = static_cast<std::uint8_t>(data.size() >> 8);
    packet[6] = static_cast<std::uint8_t>(data.size());
    std::size_t offset = std::min(data.size(), packetSize - 7);
    std::ranges::copy(data.first(offset), packet.begin() + 7);
    if (auto written = writePacket(packet); !written) {
        return written;
    }
    std::uint8_t sequence = 0;
    while (offset < data.size()) {
        std::ranges::fill(packet, 0);
        putChannel(packet);
        packet[4] = sequence++;
        const std::size_t length = std::min(data.size() - offset, packetSize - 5);
        std::ranges::copy(data.subspan(offset, length), packet.begin() + 5);
        offset += length;
        if (auto written = writePacket(packet); !written) {
            return written;
        }
    }
    return {};
}

KeyResult<Bytes> CtapHidTransport::receiveMessage(int command) {
    const auto deadline = Clock::now() + kTimeout;
    const std::size_t packetSize = pipe_->packetSize();
    std::vector<std::uint8_t> packet(packetSize, 0);
    while (true) {
        if (auto got = readPacket(packet, deadline); !got) {
            return std::unexpected(std::move(got.error()));
        }
        if (readChannel(packet) != channel_) {
            continue;
        }
        const int responseCommand = packet[4];
        if (responseCommand == (kCommandKeepalive | 0x80)) {
            if (packet[7] == kKeepaliveUserPresenceNeeded && onUserPresenceNeeded) {
                onUserPresenceNeeded();
            }
            continue;
        }
        if (responseCommand == (kCommandError | 0x80)) {
            return keyFailure(Kind::kIo, "CTAPHID error " + hexByte(packet[7]));
        }
        if (responseCommand != (command | 0x80)) {
            return keyFailure(Kind::kIo, "Unexpected CTAPHID command " + hexByte(responseCommand));
        }
        const std::size_t length = (std::size_t{packet[5]} << 8) | packet[6];
        Bytes message(length);
        std::size_t offset = std::min(length, packetSize - 7);
        std::copy_n(packet.begin() + 7, offset, message.begin());
        std::uint8_t sequence = 0;
        while (offset < length) {
            if (auto got = readPacket(packet, deadline); !got) {
                return std::unexpected(std::move(got.error()));
            }
            if (readChannel(packet) != channel_) {
                continue;
            }
            if (packet[4] != sequence++) {
                return keyFailure(Kind::kIo, "CTAPHID packet out of sequence");
            }
            const std::size_t chunk = std::min(length - offset, packetSize - 5);
            std::copy_n(packet.begin() + 5, chunk, message.begin() + static_cast<std::ptrdiff_t>(offset));
            offset += chunk;
        }
        return message;
    }
}

KeyResult<void> CtapHidTransport::readPacket(std::span<std::uint8_t> packet, Clock::time_point deadline) {
    while (true) {
        if (closed_) {
            // Stops a request waiting for a touch.
            if (!cancelSent_ && channel_ != kBroadcastChannel) {
                cancelSent_ = true;
                std::vector<std::uint8_t> cancel(pipe_->packetSize(), 0);
                putChannel(cancel);
                cancel[4] = kCommandCancel | 0x80;
                pipe_->write(cancel);
            }
            return keyFailure(Kind::kClosed, "Security key closed");
        }
        const int got = pipe_->read(packet, kPollMillis);
        if (got > 0) {
            return {};
        }
        if (got < 0) {
            return keyFailure(Kind::kIo, "Couldn't read from the security key");
        }
        if (Clock::now() > deadline) {
            return keyFailure(Kind::kIo, "Security key timed out");
        }
    }
}

KeyResult<void> CtapHidTransport::writePacket(std::span<const std::uint8_t> packet) {
    if (closed_) {
        return keyFailure(Kind::kClosed, "Security key closed");
    }
    if (!pipe_->write(packet)) {
        return keyFailure(Kind::kIo, "Couldn't write to the security key");
    }
    return {};
}

}  // namespace filepan::securitykey
