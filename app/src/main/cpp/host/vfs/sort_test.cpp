// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <algorithm>
#include <random>
#include <string>
#include <vector>

#include "filepan/vfs/sort.hpp"
#include "test_util.hpp"

using namespace filepan::vfs;

namespace {

int sign(int value) { return (value > 0) - (value < 0); }

Entry makeEntry(std::string name, EntryType type, std::uint64_t size = 0, std::int64_t mtime = 0) {
    Entry entry;
    entry.name = std::move(name);
    entry.type = type;
    entry.targetType = type;
    entry.size = size;
    entry.modifiedTime.seconds = mtime;
    return entry;
}

std::vector<std::string> names(const std::vector<Entry> &entries) {
    std::vector<std::string> result;
    for (const auto &entry : entries) {
        result.push_back(entry.name);
    }
    return result;
}

void testNaturalCompare() {
    EXPECT(naturalCompare("file2", "file10") < 0);
    EXPECT(naturalCompare("file10", "file2") > 0);
    EXPECT(naturalCompare("File2", "file10") < 0);
    EXPECT(naturalCompare("abc", "ABD") < 0);
    EXPECT(naturalCompare("a", "a") == 0);
    EXPECT(naturalCompare("a", "ab") < 0);
    EXPECT(naturalCompare("", "a") < 0);
    // Equal ignoring case: deterministic by raw bytes, never zero.
    EXPECT(naturalCompare("A", "a") < 0 && naturalCompare("a", "A") > 0);
    // Equal numbers: fewer leading zeros first.
    EXPECT(naturalCompare("x1", "x01") < 0 && naturalCompare("x01", "x1") > 0);
    EXPECT(naturalCompare("x01y2", "x1y3") < 0);
    // Huge numbers don't overflow.
    EXPECT(naturalCompare("99999999999999999999999", "100000000000000000000000") < 0);
    EXPECT(naturalCompare("v1.2.10", "v1.2.9") > 0);
    // Non-ASCII bytes sort after ASCII, by value.
    EXPECT(naturalCompare("z", "\xc3\xa4") < 0);
}

void testStrictWeakOrdering() {
    std::vector<std::string> pool = {"a", "A", "a1", "a01", "a001", "a2", "a10", "B", "b",
                                     "b2c", "b02c", "b2C", "1", "01", "10", "", "_", "\xc3\xa4",
                                     "x9y", "x09y", "x9Y", "file 2", "file 10"};
    for (const auto &a : pool) {
        EXPECT(naturalCompare(a, a) == 0);
        for (const auto &b : pool) {
            EXPECT(sign(naturalCompare(a, b)) == -sign(naturalCompare(b, a)));
            if (a != b) {
                EXPECT(naturalCompare(a, b) != 0);
            }
            for (const auto &c : pool) {
                if (naturalCompare(a, b) < 0 && naturalCompare(b, c) < 0) {
                    EXPECT(naturalCompare(a, c) < 0);
                }
            }
        }
    }
}

void testSortKeys() {
    std::vector<Entry> entries = {
        makeEntry("file10.txt", EntryType::kFile, 100, 3),
        makeEntry("File2.txt", EntryType::kFile, 300, 1),
        makeEntry("photos", EntryType::kDirectory, 4096, 5),
        makeEntry("Archive.zip", EntryType::kFile, 200, 2),
        makeEntry("docs", EntryType::kDirectory, 4096, 4),
        makeEntry("song.mp3", EntryType::kFile, 50, 6),
    };
    auto link = makeEntry("link-to-dir", EntryType::kSymlink);
    link.targetType = EntryType::kDirectory;
    entries.push_back(link);

    std::mt19937 random(42);
    std::ranges::shuffle(entries, random);
    sortEntries(entries, {.key = SortKey::kName});
    EXPECT((names(entries) == std::vector<std::string>{"docs", "link-to-dir", "photos",
                                                       "Archive.zip", "File2.txt", "file10.txt",
                                                       "song.mp3"}));

    sortEntries(entries, {.key = SortKey::kName, .descending = true});
    EXPECT((names(entries) == std::vector<std::string>{"photos", "link-to-dir", "docs",
                                                       "song.mp3", "file10.txt", "File2.txt",
                                                       "Archive.zip"}));

    sortEntries(entries, {.key = SortKey::kName, .directoriesFirst = false});
    EXPECT((names(entries) == std::vector<std::string>{"Archive.zip", "docs", "File2.txt",
                                                       "file10.txt", "link-to-dir", "photos",
                                                       "song.mp3"}));

    sortEntries(entries, {.key = SortKey::kSize});
    EXPECT((names(entries) == std::vector<std::string>{"link-to-dir", "docs", "photos",
                                                       "song.mp3", "file10.txt", "Archive.zip",
                                                       "File2.txt"}));

    sortEntries(entries, {.key = SortKey::kModified, .descending = true});
    EXPECT((names(entries) == std::vector<std::string>{"photos", "docs", "link-to-dir",
                                                       "song.mp3", "file10.txt", "Archive.zip",
                                                       "File2.txt"}));

    // By FileKind (audio before archive before text), then MIME type, extension and name.
    sortEntries(entries, {.key = SortKey::kType});
    EXPECT((names(entries) == std::vector<std::string>{"docs", "link-to-dir", "photos",
                                                       "song.mp3", "Archive.zip", "File2.txt",
                                                       "file10.txt"}));
}

// A collator that reverses the default, standing in for ICU.
class ReverseCollator : public NameCollator {
public:
    [[nodiscard]] int compare(std::string_view a, std::string_view b) const override {
        return -naturalCompare(a, b);
    }
};

void testPluggableCollator() {
    std::vector<Entry> entries = {makeEntry("a", EntryType::kFile),
                                  makeEntry("c", EntryType::kFile),
                                  makeEntry("b", EntryType::kFile)};
    ReverseCollator collator;
    sortEntries(entries, {.collator = &collator});
    EXPECT((names(entries) == std::vector<std::string>{"c", "b", "a"}));
}

}  // namespace

int main() {
    testNaturalCompare();
    testStrictWeakOrdering();
    testSortKeys();
    testPluggableCollator();
    return 0;
}
