// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/securitykey/iso7816.hpp"

#include <algorithm>

namespace filepan::securitykey {

namespace {

using Kind = KeyError::Kind;

Bytes shortApdu(int cla, int ins, int p1, int p2, std::span<const std::uint8_t> data,
                bool expectData) {
    Bytes apdu{static_cast<std::uint8_t>(cla), static_cast<std::uint8_t>(ins),
               static_cast<std::uint8_t>(p1), static_cast<std::uint8_t>(p2)};
    if (!data.empty()) {
        apdu.push_back(static_cast<std::uint8_t>(data.size()));
        apdu.insert(apdu.end(), data.begin(), data.end());
    }
    if (expectData) {
        apdu.push_back(0);
    }
    return apdu;
}

}  // namespace

KeyResult<ApduResponse> ApduSession::transmit(int cla, int ins, int p1, int p2,
                                              std::span<const std::uint8_t> data, bool expectData) {
    KeyResult<ApduResponse> response;
    if (data.size() <= 255) {
        response = exchange(shortApdu(cla, ins, p1, p2, data, expectData));
    } else if (channel_->supportsExtendedLength()) {
        Bytes apdu{static_cast<std::uint8_t>(cla), static_cast<std::uint8_t>(ins),
                   static_cast<std::uint8_t>(p1), static_cast<std::uint8_t>(p2), 0,
                   static_cast<std::uint8_t>(data.size() >> 8),
                   static_cast<std::uint8_t>(data.size())};
        apdu.insert(apdu.end(), data.begin(), data.end());
        if (expectData) {
            apdu.push_back(0);
            apdu.push_back(0);
        }
        response = exchange(apdu);
    } else {
        std::size_t offset = 0;
        while (true) {
            const std::size_t end = std::min(offset + 255, data.size());
            const bool isLast = end == data.size();
            // CLA bit 0x10 marks every block but the last in a chain.
            const int blockCla = isLast ? cla : cla | 0x10;
            response = exchange(
                shortApdu(blockCla, ins, p1, p2, data.subspan(offset, end - offset), expectData));
            if (isLast || !response || response->status != iso7816::kSwNoError) {
                break;
            }
            offset = end;
        }
    }
    if (!response) {
        return response;
    }
    Bytes output;
    while ((response->status >> 8) == iso7816::kSw1MoreData) {
        output.insert(output.end(), response->data.begin(), response->data.end());
        const std::uint8_t getResponse[] = {0x00, 0xC0, 0x00, 0x00,
                                            static_cast<std::uint8_t>(response->status)};
        response = exchange(getResponse);
        if (!response) {
            return response;
        }
    }
    output.insert(output.end(), response->data.begin(), response->data.end());
    return ApduResponse{std::move(output), response->status};
}

void ApduSession::close() {
    closed_ = true;
    channel_->close();
}

KeyResult<ApduResponse> ApduSession::exchange(std::span<const std::uint8_t> apdu) {
    if (closed_) {
        return keyFailure(Kind::kClosed, "Security key closed");
    }
    auto response = channel_->transceive(apdu);
    if (closed_) {
        return keyFailure(Kind::kClosed, "Security key closed");
    }
    if (!response) {
        return keyFailure(Kind::kIo, "The security key was lost");
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

}  // namespace filepan::securitykey
