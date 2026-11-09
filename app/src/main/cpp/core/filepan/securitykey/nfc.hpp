// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <memory>
#include <span>

#include "filepan/securitykey/iso7816.hpp"
#include "filepan/securitykey/transport.hpp"

// A security key over NFC, speaking ISO 7816 APDUs. Tapping it counts as user presence.
namespace filepan::securitykey {

class NfcTransport final : public CtapTransport {
public:
    // Selects the FIDO applet.
    static KeyResult<std::unique_ptr<NfcTransport>> open(std::unique_ptr<ApduChannel> channel);

    [[nodiscard]] bool mayHaveCtap2() const override { return true; }
    [[nodiscard]] bool isUserPresent() const override { return true; }
    KeyResult<Bytes> sendCbor(int command, std::span<const std::uint8_t> data) override;
    KeyResult<ApduResponse> sendApdu(int ins, int p1, int p2,
                                     std::span<const std::uint8_t> data) override;
    void close() override { session_.close(); }
    [[nodiscard]] bool isClosed() const override { return session_.isClosed(); }

    static constexpr int kSwKeepalive = 0x9100;

private:
    explicit NfcTransport(std::unique_ptr<ApduChannel> channel) : session_(std::move(channel)) {}

    ApduSession session_;
};

}  // namespace filepan::securitykey
