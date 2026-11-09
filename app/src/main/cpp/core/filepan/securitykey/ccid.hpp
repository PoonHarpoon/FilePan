// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>

#include "filepan/securitykey/iso7816.hpp"
#include "filepan/securitykey/transport.hpp"

// A smart card over USB: the card in a CCID reader, or a YubiKey's own CCID interface, which is
// where its OpenPGP applet listens.
namespace filepan::securitykey {

// The CCID interface's bulk endpoints, one packet at a time.
class BulkPipe {
public:
    virtual ~BulkPipe() = default;

    // The bulk-in endpoint's packet size.
    [[nodiscard]] virtual std::size_t packetSize() const = 0;
    // Whether all of data was written.
    virtual bool write(std::span<const std::uint8_t> data) = 0;
    // Waits up to timeoutMillis for one packet: its length, 0 on timeout, or -1 on failure.
    virtual int read(std::span<std::uint8_t> packet, int timeoutMillis) = 0;
};

// A BulkPipe on a Linux usbfs file descriptor (on Android, UsbDeviceConnection's), whose
// interface is already claimed. The descriptor isn't closed: onClose releases it.
class UsbfsBulkPipe final : public BulkPipe {
public:
    UsbfsBulkPipe(int fd, std::uint8_t inEndpoint, std::uint8_t outEndpoint, std::size_t packetSize,
                  std::function<void()> onClose)
        : fd_(fd), inEndpoint_(inEndpoint), outEndpoint_(outEndpoint), packetSize_(packetSize),
          onClose_(std::move(onClose)) {}
    ~UsbfsBulkPipe() override;

    [[nodiscard]] std::size_t packetSize() const override { return packetSize_; }
    bool write(std::span<const std::uint8_t> data) override;
    int read(std::span<std::uint8_t> packet, int timeoutMillis) override;

private:
    int fd_;
    std::uint8_t inEndpoint_;
    std::uint8_t outEndpoint_;
    std::size_t packetSize_;
    std::function<void()> onClose_;
};

// Exchanges APDUs with the card in slot 0 as CCID's short APDU level does: one command per
// XfrBlock, which ApduSession chains and collects. A card that's busy (waiting for a touch) asks
// for more time, and the reader keeps waiting.
class CcidReader final : public ApduChannel {
public:
    // Powers the card on.
    static KeyResult<std::unique_ptr<CcidReader>> open(std::unique_ptr<BulkPipe> pipe);
    // Powers the card off.
    ~CcidReader() override;

    [[nodiscard]] bool supportsExtendedLength() const override { return false; }
    std::optional<Bytes> transceive(std::span<const std::uint8_t> apdu) override;
    void close() override { closed_ = true; }

    static constexpr std::uint8_t kPcToRdrIccPowerOn = 0x62;
    static constexpr std::uint8_t kPcToRdrIccPowerOff = 0x63;
    static constexpr std::uint8_t kPcToRdrXfrBlock = 0x6F;
    static constexpr std::uint8_t kRdrToPcDataBlock = 0x80;
    static constexpr std::uint8_t kRdrToPcSlotStatus = 0x81;
    static constexpr std::size_t kHeaderSize = 10;

private:
    explicit CcidReader(std::unique_ptr<BulkPipe> pipe) : pipe_(std::move(pipe)) {}

    // Sends a message and waits for its answer's data; nullopt if it failed.
    std::optional<Bytes> command(std::uint8_t type, std::span<const std::uint8_t> data);
    // The next message from the reader, whole: its header and data.
    std::optional<Bytes> receive();

    std::unique_ptr<BulkPipe> pipe_;
    std::uint8_t sequence_ = 0;
    std::atomic<bool> closed_ = false;
};

}  // namespace filepan::securitykey
