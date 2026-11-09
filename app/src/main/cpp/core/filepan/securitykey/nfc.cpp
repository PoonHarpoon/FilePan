// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/securitykey/nfc.hpp"

#include <chrono>
#include <string>
#include <thread>

namespace filepan::securitykey {

namespace {

using Kind = KeyError::Kind;

constexpr std::uint8_t kFidoAid[] = {0xA0, 0x00, 0x00, 0x06, 0x47, 0x2F, 0x00, 0x01};

}  // namespace

KeyResult<std::unique_ptr<NfcTransport>> NfcTransport::open(std::unique_ptr<ApduChannel> channel) {
    std::unique_ptr<NfcTransport> transport(new NfcTransport(std::move(channel)));
    auto response = transport->session_.transmit(0x00, 0xA4, 0x04, 0x00, kFidoAid);
    if (!response) {
        return std::unexpected(std::move(response.error()));
    }
    if (response->status != iso7816::kSwNoError) {
        return keyFailure(Kind::kIo, "Not a security key");
    }
    return transport;
}

KeyResult<Bytes> NfcTransport::sendCbor(int command, std::span<const std::uint8_t> data) {
    Bytes message;
    message.reserve(1 + data.size());
    message.push_back(static_cast<std::uint8_t>(command));
    message.insert(message.end(), data.begin(), data.end());
    auto response = session_.transmit(0x80, 0x10, 0x00, 0x00, message);
    // The key is still working, e.g. waiting for its user verification.
    while (response && response->status == kSwKeepalive) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        response = session_.transmit(0x80, 0x11, 0x00, 0x00, {});
    }
    if (!response) {
        return std::unexpected(std::move(response.error()));
    }
    if (response->status != iso7816::kSwNoError) {
        return keyFailure(Kind::kIo, "NFC error " + std::to_string(response->status));
    }
    return std::move(response->data);
}

KeyResult<ApduResponse> NfcTransport::sendApdu(int ins, int p1, int p2,
                                               std::span<const std::uint8_t> data) {
    return session_.transmit(0x00, ins, p1, p2, data);
}

}  // namespace filepan::securitykey
