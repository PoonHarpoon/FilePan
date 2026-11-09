// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include "filepan/securitykey/transport.hpp"

// A security key over USB, speaking CTAPHID.
namespace filepan::securitykey {

// The key's HID interrupt endpoints, one report at a time.
class HidPipe {
public:
    virtual ~HidPipe() = default;

    [[nodiscard]] virtual std::size_t packetSize() const = 0;
    // Whether the whole report was written.
    virtual bool write(std::span<const std::uint8_t> packet) = 0;
    // Waits up to timeoutMillis for a report: its length, 0 on timeout, or -1 on failure.
    virtual int read(std::span<std::uint8_t> packet, int timeoutMillis) = 0;
};

// A HidPipe on a Linux usbfs file descriptor (on Android, UsbDeviceConnection's), whose
// interface is already claimed. The descriptor isn't closed: onClose releases it.
class UsbfsHidPipe final : public HidPipe {
public:
    UsbfsHidPipe(int fd, std::uint8_t inEndpoint, std::uint8_t outEndpoint, std::size_t packetSize,
                 std::function<void()> onClose)
        : fd_(fd), inEndpoint_(inEndpoint), outEndpoint_(outEndpoint), packetSize_(packetSize),
          onClose_(std::move(onClose)) {}
    ~UsbfsHidPipe() override;

    [[nodiscard]] std::size_t packetSize() const override { return packetSize_; }
    bool write(std::span<const std::uint8_t> packet) override;
    int read(std::span<std::uint8_t> packet, int timeoutMillis) override;

private:
    int fd_;
    std::uint8_t inEndpoint_;
    std::uint8_t outEndpoint_;
    std::size_t packetSize_;
    std::function<void()> onClose_;
};

class CtapHidTransport final : public CtapTransport {
public:
    // Opens a channel with CTAPHID_INIT.
    static KeyResult<std::unique_ptr<CtapHidTransport>> open(std::unique_ptr<HidPipe> pipe);

    [[nodiscard]] bool mayHaveCtap2() const override { return (capabilities_ & kCapabilityCbor) != 0; }
    [[nodiscard]] bool isUserPresent() const override { return false; }
    KeyResult<Bytes> sendCbor(int command, std::span<const std::uint8_t> data) override;
    KeyResult<ApduResponse> sendApdu(int ins, int p1, int p2,
                                     std::span<const std::uint8_t> data) override;
    void close() override { closed_ = true; }
    [[nodiscard]] bool isClosed() const override { return closed_; }

    static constexpr std::uint32_t kBroadcastChannel = 0xFFFFFFFF;
    static constexpr int kCommandMsg = 0x03;
    static constexpr int kCommandInit = 0x06;
    static constexpr int kCommandCbor = 0x10;
    static constexpr int kCommandCancel = 0x11;
    static constexpr int kCommandKeepalive = 0x3B;
    static constexpr int kCommandError = 0x3F;
    static constexpr int kCapabilityCbor = 0x04;
    static constexpr int kKeepaliveUserPresenceNeeded = 2;

private:
    explicit CtapHidTransport(std::unique_ptr<HidPipe> pipe) : pipe_(std::move(pipe)) {}

    KeyResult<void> initialize();
    KeyResult<Bytes> transact(int command, std::span<const std::uint8_t> data);
    KeyResult<void> sendMessage(int command, std::span<const std::uint8_t> data);
    KeyResult<Bytes> receiveMessage(int command);
    KeyResult<void> readPacket(std::span<std::uint8_t> packet,
                               std::chrono::steady_clock::time_point deadline);
    KeyResult<void> writePacket(std::span<const std::uint8_t> packet);
    void putChannel(std::span<std::uint8_t> packet) const;

    std::unique_ptr<HidPipe> pipe_;
    std::uint32_t channel_ = kBroadcastChannel;
    int capabilities_ = 0;
    std::atomic<bool> closed_ = false;
    bool cancelSent_ = false;
};

}  // namespace filepan::securitykey
