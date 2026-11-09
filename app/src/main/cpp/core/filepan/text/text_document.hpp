// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A text file being viewed or edited: its contents as UTF-8 with '\n' line breaks, an index of
// where each line starts, and what's needed to write it back the way it came (encoding, byte
// order mark, line endings). Plain C++, without ICU: the codecs here are the few a text file on a
// phone is likely to be in.
namespace filepan::text {

enum class TextEncoding : std::uint8_t {
    utf8,
    utf8Bom,
    utf16le,  // With its byte order mark; there is no guessing UTF-16 without one.
    utf16be,
    // Anything that isn't valid UTF-8: every byte is a character, so nothing is lost on the way
    // back. Latin-1 text reads right, and so does most of what Windows wrote.
    windows1252,
};

enum class LineEnding : std::uint8_t { lf, crlf, cr };

// "UTF-8", "UTF-16 LE", "Windows-1252"...
const char *encodingName(TextEncoding encoding);
const char *lineEndingName(LineEnding ending);

class TextDocument {
public:
    enum class DecodeError : std::uint8_t {
        binary,  // NUL bytes near the start: not text.
    };

    TextDocument() : TextDocument(std::string()) {}
    explicit TextDocument(std::string utf8, TextEncoding encoding = TextEncoding::utf8,
                          LineEnding ending = LineEnding::lf);

    // The encoding comes from a byte order mark, else UTF-8 if the bytes are valid UTF-8, else
    // Windows-1252. The line ending is the first one in the file (LF when there is none); others
    // are read as line breaks too, and written back as that one.
    static std::expected<TextDocument, DecodeError> decode(std::span<const std::uint8_t> bytes);
    // The file's bytes: the text in its encoding and line ending, with its byte order mark.
    [[nodiscard]] std::vector<std::uint8_t> encode() const;

    [[nodiscard]] const std::string &text() const { return text_; }
    [[nodiscard]] std::uint32_t size() const { return static_cast<std::uint32_t>(text_.size()); }
    [[nodiscard]] TextEncoding encoding() const { return encoding_; }
    [[nodiscard]] LineEnding lineEnding() const { return ending_; }
    void setLineEnding(LineEnding ending) { ending_ = ending; }
    void setEncoding(TextEncoding encoding) { encoding_ = encoding; }
    // Whether encode() keeps every character: Windows-1252 has no room for most of Unicode, and
    // writes those it can't hold as '?'.
    [[nodiscard]] bool canEncode() const;

    // Lines are split at '\n', which isn't part of any line: "a\n" has the lines "a" and "".
    [[nodiscard]] std::size_t lineCount() const { return lineStarts_.size(); }
    [[nodiscard]] std::uint32_t lineStart(std::size_t line) const { return lineStarts_[line]; }
    // Where the line's text ends: at its '\n', or the end of the text.
    [[nodiscard]] std::uint32_t lineEnd(std::size_t line) const;
    [[nodiscard]] std::string_view line(std::size_t line) const;
    // The line holding the byte at offset (offset may be size()).
    [[nodiscard]] std::size_t lineOf(std::uint32_t offset) const;

    // Replaces the bytes [start, end) with text, which may hold line breaks ('\r' and "\r\n"
    // become '\n'). Offsets are clamped to the text; the caller keeps them on UTF-8 boundaries.
    void replace(std::uint32_t start, std::uint32_t end, std::string_view text);

private:
    void indexLines();

    std::string text_;
    TextEncoding encoding_;
    LineEnding ending_;
    std::vector<std::uint32_t> lineStarts_;
};

// '\r\n' and lone '\r' to '\n'.
std::string normalizeLineBreaks(std::string_view text);

}  // namespace filepan::text
