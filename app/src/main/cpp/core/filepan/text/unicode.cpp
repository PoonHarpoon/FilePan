// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/text/unicode.hpp"

namespace filepan::text {

void utf8ToUtf16(std::string_view utf8, std::u16string &out, std::vector<std::uint32_t> &byteOf) {
    out.clear();
    byteOf.clear();
    out.reserve(utf8.size());
    byteOf.reserve(utf8.size() + 1);
    const auto *bytes = reinterpret_cast<const unsigned char *>(utf8.data());
    const std::size_t size = utf8.size();
    std::size_t i = 0;
    while (i < size) {
        const auto start = static_cast<std::uint32_t>(i);
        const unsigned char lead = bytes[i];
        if (lead < 0x80) {
            out.push_back(lead);
            byteOf.push_back(start);
            ++i;
            continue;
        }
        char32_t c = 0xFFFD;
        std::size_t length = 1;
        char32_t min = 0;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            c = lead & 0x1F;
            min = 0x80;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            c = lead & 0x0F;
            min = 0x800;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            c = lead & 0x07;
            min = 0x10000;
        } else {
            length = 0;
        }
        bool valid = length != 0 && i + length <= size;
        for (std::size_t k = 1; valid && k < length; ++k) {
            const unsigned char continuation = bytes[i + k];
            if ((continuation & 0xC0) != 0x80) {
                valid = false;
            } else {
                c = (c << 6) | (continuation & 0x3F);
            }
        }
        if (valid && (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))) {
            valid = false;
        }
        if (!valid) {
            // Skip one byte and substitute, as the WHATWG decoder does for each bad byte.
            c = 0xFFFD;
            length = 1;
        }
        if (c >= 0x10000) {
            out.push_back(static_cast<char16_t>(0xD800 + ((c - 0x10000) >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + ((c - 0x10000) & 0x3FF)));
            byteOf.push_back(start);
            byteOf.push_back(start);
        } else {
            out.push_back(static_cast<char16_t>(c));
            byteOf.push_back(start);
        }
        i += length;
    }
    byteOf.push_back(static_cast<std::uint32_t>(size));
}

}  // namespace filepan::text
