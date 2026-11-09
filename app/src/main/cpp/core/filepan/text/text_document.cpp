// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/text/text_document.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>

namespace filepan::text {

namespace {

// Bytes 0x80-0x9F in Windows-1252; the five it leaves undefined keep their C1 control.
constexpr std::array<char32_t, 32> kWindows1252High = {
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
        0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
        0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

// How far into a file to look for NUL bytes.
constexpr std::size_t kSniffBytes = 8192;

void appendUtf8(std::string &out, char32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// The code point at index, advancing past it; U+FFFD (one byte) for anything malformed. ok, if
// given, is cleared on a malformed sequence.
char32_t nextUtf8(std::string_view s, std::size_t &index, bool *ok = nullptr) {
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(s[i]); };
    const unsigned char lead = byte(index);
    std::size_t length = 0;
    char32_t c = 0;
    char32_t minimum = 0;
    if (lead < 0x80) {
        ++index;
        return lead;
    } else if (lead >= 0xC2 && lead < 0xE0) {
        length = 2, c = lead & 0x1F, minimum = 0x80;
    } else if (lead >= 0xE0 && lead < 0xF0) {
        length = 3, c = lead & 0x0F, minimum = 0x800;
    } else if (lead >= 0xF0 && lead < 0xF5) {
        length = 4, c = lead & 0x07, minimum = 0x10000;
    }
    if (length == 0 || index + length > s.size()) {
        ++index;
        if (ok) *ok = false;
        return 0xFFFD;
    }
    for (std::size_t i = 1; i < length; ++i) {
        const unsigned char next = byte(index + i);
        if ((next & 0xC0) != 0x80) {
            ++index;
            if (ok) *ok = false;
            return 0xFFFD;
        }
        c = (c << 6) | (next & 0x3F);
    }
    if (c < minimum || c > 0x10FFFF || (c >= 0xD800 && c < 0xE000)) {
        ++index;
        if (ok) *ok = false;
        return 0xFFFD;
    }
    index += length;
    return c;
}

bool validUtf8(std::string_view s) {
    bool ok = true;
    for (std::size_t i = 0; i < s.size() && ok;) {
        nextUtf8(s, i, &ok);
    }
    return ok;
}

std::string decodeUtf16(std::span<const std::uint8_t> bytes, bool bigEndian) {
    std::string out;
    out.reserve(bytes.size());
    const auto unit = [&](std::size_t i) -> char16_t {
        return bigEndian ? static_cast<char16_t>(bytes[i] << 8 | bytes[i + 1])
                         : static_cast<char16_t>(bytes[i + 1] << 8 | bytes[i]);
    };
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        char32_t c = unit(i);
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < bytes.size()) {
            const char16_t trail = unit(i + 2);
            if (trail >= 0xDC00 && trail < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (trail - 0xDC00);
                i += 2;
            }
        }
        appendUtf8(out, c >= 0xD800 && c < 0xE000 ? 0xFFFD : c);
    }
    return out;
}

std::string decodeWindows1252(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve(bytes.size() + bytes.size() / 8);
    for (const std::uint8_t b : bytes) {
        appendUtf8(out, b >= 0x80 && b < 0xA0 ? kWindows1252High[b - 0x80] : b);
    }
    return out;
}

// The Windows-1252 byte for c, or -1.
int windows1252Byte(char32_t c) {
    if (c < 0x80 || (c >= 0xA0 && c < 0x100)) {
        return static_cast<int>(c);
    }
    for (std::size_t i = 0; i < kWindows1252High.size(); ++i) {
        if (kWindows1252High[i] == c) {
            return static_cast<int>(0x80 + i);
        }
    }
    return -1;
}

LineEnding detectLineEnding(std::string_view text) {
    const std::size_t at = text.find_first_of("\r\n");
    if (at == std::string_view::npos || text[at] == '\n') {
        return LineEnding::lf;
    }
    return at + 1 < text.size() && text[at + 1] == '\n' ? LineEnding::crlf : LineEnding::cr;
}

}  // namespace

const char *encodingName(TextEncoding encoding) {
    switch (encoding) {
    case TextEncoding::utf8:
        return "UTF-8";
    case TextEncoding::utf8Bom:
        return "UTF-8 BOM";
    case TextEncoding::utf16le:
        return "UTF-16 LE";
    case TextEncoding::utf16be:
        return "UTF-16 BE";
    case TextEncoding::windows1252:
        return "Windows-1252";
    }
    return "";
}

const char *lineEndingName(LineEnding ending) {
    switch (ending) {
    case LineEnding::lf:
        return "LF";
    case LineEnding::crlf:
        return "CRLF";
    case LineEnding::cr:
        return "CR";
    }
    return "";
}

std::string normalizeLineBreaks(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\r') {
            out += text[i];
        } else {
            out += '\n';
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
        }
    }
    return out;
}

TextDocument::TextDocument(std::string utf8, TextEncoding encoding, LineEnding ending)
        : text_(utf8.find('\r') == std::string::npos ? std::move(utf8) : normalizeLineBreaks(utf8)),
          encoding_(encoding),
          ending_(ending) {
    indexLines();
}

std::expected<TextDocument, TextDocument::DecodeError> TextDocument::decode(std::span<const std::uint8_t> bytes) {
    const auto startsWith = [&](std::initializer_list<std::uint8_t> prefix) {
        return bytes.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), bytes.begin());
    };
    TextEncoding encoding;
    std::string text;
    if (startsWith({0xEF, 0xBB, 0xBF})) {
        encoding = TextEncoding::utf8Bom;
        bytes = bytes.subspan(3);
    } else if (startsWith({0xFF, 0xFE})) {
        encoding = TextEncoding::utf16le;
        text = decodeUtf16(bytes.subspan(2), false);
    } else if (startsWith({0xFE, 0xFF})) {
        encoding = TextEncoding::utf16be;
        text = decodeUtf16(bytes.subspan(2), true);
    } else {
        encoding = TextEncoding::utf8;
    }
    if (encoding == TextEncoding::utf8 || encoding == TextEncoding::utf8Bom) {
        const auto head = bytes.first(std::min(bytes.size(), kSniffBytes));
        if (std::memchr(head.data(), 0, head.size()) != nullptr) {
            return std::unexpected(DecodeError::binary);
        }
        const std::string_view raw(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        if (validUtf8(raw)) {
            text.assign(raw);
        } else {
            encoding = TextEncoding::windows1252;
            text = decodeWindows1252(bytes);
        }
    }
    const LineEnding ending = detectLineEnding(text);
    return TextDocument(std::move(text), encoding, ending);
}

bool TextDocument::canEncode() const {
    if (encoding_ != TextEncoding::windows1252) {
        return true;
    }
    for (std::size_t i = 0; i < text_.size();) {
        if (windows1252Byte(nextUtf8(text_, i)) < 0) {
            return false;
        }
    }
    return true;
}

std::vector<std::uint8_t> TextDocument::encode() const {
    std::string lines;
    if (ending_ == LineEnding::lf) {
        lines = text_;
    } else {
        lines.reserve(text_.size() + lineStarts_.size());
        for (const char c : text_) {
            if (c != '\n') {
                lines += c;
            } else {
                lines += ending_ == LineEnding::crlf ? "\r\n" : "\r";
            }
        }
    }
    std::vector<std::uint8_t> out;
    switch (encoding_) {
    case TextEncoding::utf8Bom:
        out = {0xEF, 0xBB, 0xBF};
        [[fallthrough]];
    case TextEncoding::utf8:
        out.insert(out.end(), lines.begin(), lines.end());
        break;
    case TextEncoding::utf16le:
    case TextEncoding::utf16be: {
        const bool big = encoding_ == TextEncoding::utf16be;
        const auto put = [&](char16_t unit) {
            out.push_back(static_cast<std::uint8_t>(big ? unit >> 8 : unit & 0xFF));
            out.push_back(static_cast<std::uint8_t>(big ? unit & 0xFF : unit >> 8));
        };
        out.reserve(2 + lines.size() * 2);
        put(0xFEFF);
        for (std::size_t i = 0; i < lines.size();) {
            const char32_t c = nextUtf8(lines, i);
            if (c >= 0x10000) {
                put(static_cast<char16_t>(0xD800 + ((c - 0x10000) >> 10)));
                put(static_cast<char16_t>(0xDC00 + ((c - 0x10000) & 0x3FF)));
            } else {
                put(static_cast<char16_t>(c));
            }
        }
        break;
    }
    case TextEncoding::windows1252:
        out.reserve(lines.size());
        for (std::size_t i = 0; i < lines.size();) {
            const int b = windows1252Byte(nextUtf8(lines, i));
            out.push_back(static_cast<std::uint8_t>(b < 0 ? '?' : b));
        }
        break;
    }
    return out;
}

std::uint32_t TextDocument::lineEnd(std::size_t line) const {
    return line + 1 < lineStarts_.size() ? lineStarts_[line + 1] - 1 : size();
}

std::string_view TextDocument::line(std::size_t line) const {
    const std::uint32_t start = lineStarts_[line];
    return std::string_view(text_).substr(start, lineEnd(line) - start);
}

std::size_t TextDocument::lineOf(std::uint32_t offset) const {
    const auto after = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), offset);
    return static_cast<std::size_t>(after - lineStarts_.begin()) - 1;
}

void TextDocument::replace(std::uint32_t start, std::uint32_t end, std::string_view with) {
    start = std::min(start, size());
    end = std::clamp(end, start, size());
    const std::string normalized = with.find('\r') == std::string_view::npos ? std::string() : normalizeLineBreaks(with);
    const std::string_view inserted = normalized.empty() ? with : std::string_view(normalized);

    // Line starts after a removed '\n' go; the ones after the edit shift; inserted '\n's add some.
    const auto first = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), start);
    const auto last = std::upper_bound(first, lineStarts_.end(), end);
    const std::size_t at = static_cast<std::size_t>(first - lineStarts_.begin());
    lineStarts_.erase(first, last);
    const std::int64_t delta = static_cast<std::int64_t>(inserted.size()) - (end - start);
    for (std::size_t i = at; i < lineStarts_.size(); ++i) {
        lineStarts_[i] = static_cast<std::uint32_t>(lineStarts_[i] + delta);
    }
    std::vector<std::uint32_t> added;
    for (std::size_t i = 0; i < inserted.size(); ++i) {
        if (inserted[i] == '\n') {
            added.push_back(static_cast<std::uint32_t>(start + i + 1));
        }
    }
    lineStarts_.insert(lineStarts_.begin() + static_cast<std::ptrdiff_t>(at), added.begin(), added.end());
    text_.replace(start, end - start, inserted);
}

void TextDocument::indexLines() {
    lineStarts_.assign(1, 0);
    for (std::size_t i = 0; i < text_.size(); ++i) {
        if (text_[i] == '\n') {
            lineStarts_.push_back(static_cast<std::uint32_t>(i + 1));
        }
    }
}

}  // namespace filepan::text
