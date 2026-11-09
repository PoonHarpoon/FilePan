// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "filepan/text/text_document.hpp"
#include "test_util.hpp"

using namespace filepan::text;

namespace {

std::vector<std::uint8_t> bytes(std::string_view s) { return {s.begin(), s.end()}; }

TextDocument decoded(std::string_view s) {
    auto document = TextDocument::decode(bytes(s));
    EXPECT(document.has_value());
    return std::move(*document);
}

void testDecodeAndEncode() {
    // UTF-8, as is.
    TextDocument plain = decoded("h\xC3\xA9llo\nworld\n");
    EXPECT(plain.encoding() == TextEncoding::utf8 && plain.lineEnding() == LineEnding::lf);
    EXPECT(plain.text() == "h\xC3\xA9llo\nworld\n" && plain.encode() == bytes("h\xC3\xA9llo\nworld\n"));
    // A byte order mark is kept, and CRLF lines read as '\n' and go back as CRLF.
    TextDocument windows = decoded("\xEF\xBB\xBF" "a\r\nb\r\n");
    EXPECT(windows.encoding() == TextEncoding::utf8Bom && windows.lineEnding() == LineEnding::crlf);
    EXPECT(windows.text() == "a\nb\n" && windows.encode() == bytes("\xEF\xBB\xBF" "a\r\nb\r\n"));
    // Old Mac line ends.
    TextDocument mac = decoded("a\rb");
    EXPECT(mac.lineEnding() == LineEnding::cr && mac.text() == "a\nb" && mac.encode() == bytes("a\rb"));
    // UTF-16 by its mark, both ways round, with a surrogate pair.
    const std::string le("\xFF\xFE" "a\0\n\0" "\x3D\xD8\x00\xDE", 10);
    TextDocument utf16 = decoded(le);
    EXPECT(utf16.encoding() == TextEncoding::utf16le && utf16.text() == "a\n\xF0\x9F\x98\x80");
    EXPECT(utf16.encode() == bytes(le));
    const std::string be("\xFE\xFF\0a\0\n", 6);
    EXPECT(decoded(be).text() == "a\n" && decoded(be).encode() == bytes(be));
    // Not UTF-8: Windows-1252, byte for byte, both ways.
    const std::string latin = "caf\xE9 \x80 \x81\n";
    TextDocument old = decoded(latin);
    EXPECT(old.encoding() == TextEncoding::windows1252);
    EXPECT(old.text() == "caf\xC3\xA9 \xE2\x82\xAC \xC2\x81\n" && old.encode() == bytes(latin) && old.canEncode());
    // What it can't hold comes out as '?', and canEncode says so.
    old.replace(0, 0, "\xE2\x9C\x93");
    EXPECT(!old.canEncode() && old.encode().front() == '?');
    old.setEncoding(TextEncoding::utf8);
    EXPECT(old.canEncode() && old.encode().front() == 0xE2);
    // NULs near the start: binary.
    EXPECT(!TextDocument::decode(bytes(std::string_view("PK\x03\x04\0\0", 6))).has_value());
    // Empty.
    EXPECT(decoded("").text().empty() && decoded("").lineCount() == 1);
}

void testLines() {
    TextDocument doc(std::string("one\ntwo\n\nfour"));
    EXPECT(doc.lineCount() == 4 && doc.line(0) == "one" && doc.line(2).empty() && doc.line(3) == "four");
    EXPECT(doc.lineStart(1) == 4 && doc.lineEnd(1) == 7 && doc.lineEnd(3) == doc.size());
    EXPECT(doc.lineOf(0) == 0 && doc.lineOf(3) == 0 && doc.lineOf(4) == 1 && doc.lineOf(doc.size()) == 3);
    EXPECT(TextDocument(std::string("a\n")).lineCount() == 2);
}

// Every edit leaves the same lines as indexing the result from scratch.
void testEditsMatchReindexing() {
    std::mt19937 random(7);
    TextDocument doc(std::string("first line\nsecond\n\nthird line here\n"));
    const std::string_view pieces[] = {"", "x", "\n", "ab\ncd", "\n\n", "tail\r\nmore\r", "\xC3\xA9"};
    for (int i = 0; i < 2000; ++i) {
        const std::uint32_t a = random() % (doc.size() + 1);
        const std::uint32_t b = a + random() % (doc.size() - a + 1);
        doc.replace(a, b, pieces[random() % std::size(pieces)]);
        const TextDocument fresh(doc.text());
        EXPECT(doc.lineCount() == fresh.lineCount());
        for (std::size_t line = 0; line < fresh.lineCount(); ++line) {
            EXPECT(doc.lineStart(line) == fresh.lineStart(line));
        }
        EXPECT(doc.text().find('\r') == std::string::npos);
    }
}

}  // namespace

int main() {
    testDecodeAndEncode();
    testLines();
    testEditsMatchReindexing();
    return 0;
}
