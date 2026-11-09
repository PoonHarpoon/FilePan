// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "filepan/securitykey/cbor.hpp"

// SSH's wire encoding (RFC 4251 §5), for key and signature blobs and agent messages.
namespace filepan::securitykey::ssh_wire {

inline void appendU32(Bytes &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

inline void appendString(Bytes &out, std::span<const std::uint8_t> value) {
    appendU32(out, static_cast<std::uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

inline void appendString(Bytes &out, std::string_view value) {
    appendString(out,
                 std::span(reinterpret_cast<const std::uint8_t *>(value.data()), value.size()));
}

// A non-negative integer from its big-endian bytes.
inline void appendMpint(Bytes &out, std::span<const std::uint8_t> magnitude) {
    while (!magnitude.empty() && magnitude.front() == 0) {
        magnitude = magnitude.subspan(1);
    }
    const bool pad = !magnitude.empty() && (magnitude.front() & 0x80) != 0;
    appendU32(out, static_cast<std::uint32_t>(magnitude.size() + (pad ? 1 : 0)));
    if (pad) {
        out.push_back(0);
    }
    out.insert(out.end(), magnitude.begin(), magnitude.end());
}

// Reads from the front of a buffer; every read fails once one has.
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}

    std::optional<std::uint8_t> u8() {
        if (data_.empty()) {
            return std::nullopt;
        }
        const std::uint8_t value = data_.front();
        data_ = data_.subspan(1);
        return value;
    }

    std::optional<std::uint32_t> u32() {
        if (data_.size() < 4) {
            data_ = {};
            return std::nullopt;
        }
        const std::uint32_t value = (std::uint32_t{data_[0]} << 24)
                                    | (std::uint32_t{data_[1]} << 16)
                                    | (std::uint32_t{data_[2]} << 8) | data_[3];
        data_ = data_.subspan(4);
        return value;
    }

    std::optional<std::span<const std::uint8_t>> string() {
        const auto size = u32();
        if (!size || *size > data_.size()) {
            data_ = {};
            return std::nullopt;
        }
        const auto value = data_.first(*size);
        data_ = data_.subspan(*size);
        return value;
    }

    [[nodiscard]] bool empty() const { return data_.empty(); }

private:
    std::span<const std::uint8_t> data_;
};

}  // namespace filepan::securitykey::ssh_wire
