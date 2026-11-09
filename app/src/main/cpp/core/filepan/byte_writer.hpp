// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <vector>

namespace filepan {

// Appends little-endian values to a growable buffer. Used for the batched records handed to
// Kotlin, which reads them back with a little-endian ByteBuffer.
class ByteWriter {
public:
    void reserve(std::size_t size) { buffer_.reserve(size); }
    void clear() { buffer_.clear(); }

    [[nodiscard]] const std::uint8_t *data() const { return buffer_.data(); }
    [[nodiscard]] std::size_t size() const { return buffer_.size(); }
    [[nodiscard]] bool empty() const { return buffer_.empty(); }

    template <typename T>
        requires std::is_integral_v<T>
    void put(T value) {
        static_assert(std::endian::native == std::endian::little, "Only little-endian is supported");
        const auto offset = buffer_.size();
        buffer_.resize(offset + sizeof(T));
        std::memcpy(buffer_.data() + offset, &value, sizeof(T));
    }

    void putBytes(std::string_view bytes) {
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    }

private:
    std::vector<std::uint8_t> buffer_;
};

}  // namespace filepan
