// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Small Unicode helpers shared by the text system's parts.
namespace filepan::text {

// Default_Ignorable_Code_Point (Unicode 16), which fonts needn't cover: HarfBuzz hides them.
constexpr bool isDefaultIgnorable(char32_t c) {
    if (c < 0x00AD) {
        return false;
    }
    return c == 0x00AD || c == 0x034F || c == 0x061C || (c >= 0x115F && c <= 0x1160) ||
           (c >= 0x17B4 && c <= 0x17B5) || (c >= 0x180B && c <= 0x180F) ||
           (c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) ||
           (c >= 0x2060 && c <= 0x206F) || c == 0x3164 || (c >= 0xFE00 && c <= 0xFE0F) ||
           c == 0xFEFF || c == 0xFFA0 || (c >= 0xFFF0 && c <= 0xFFF8) ||
           (c >= 0x1BCA0 && c <= 0x1BCA3) || (c >= 0x1D173 && c <= 0x1D17A) ||
           (c >= 0xE0000 && c <= 0xE0FFF);
}

constexpr bool isControl(char32_t c) {
    return c < 0x20 || (c >= 0x7F && c < 0xA0);
}

// Characters that may be right-to-left or need bidi processing. Below this, all text is
// left-to-right and the bidi algorithm can be skipped.
constexpr bool mayNeedBidi(char16_t c) {
    return (c >= 0x0590 && c <= 0x08FF) || (c >= 0x200E && c <= 0x200F) ||
           (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069) ||
           (c >= 0xD800 && c <= 0xDFFF) || (c >= 0xFB1D && c <= 0xFDFF) ||
           (c >= 0xFE70 && c <= 0xFEFF);
}

// Decodes UTF-8 into UTF-16, recording for each UTF-16 unit the byte offset of the character
// it belongs to (plus one entry for the end). Invalid sequences become U+FFFD.
void utf8ToUtf16(std::string_view utf8, std::u16string &out, std::vector<std::uint32_t> &byteOf);

// The code point at index, and the index after it.
inline char32_t codePointAt(std::u16string_view text, std::size_t &index) {
    const char16_t lead = text[index++];
    if (lead >= 0xD800 && lead < 0xDC00 && index < text.size()) {
        const char16_t trail = text[index];
        if (trail >= 0xDC00 && trail < 0xE000) {
            ++index;
            return 0x10000 + ((static_cast<char32_t>(lead) - 0xD800) << 10) + (trail - 0xDC00);
        }
    }
    return lead;
}

}  // namespace filepan::text
