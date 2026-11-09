// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <span>
#include <string_view>

#include "filepan/vfs/entry.hpp"

namespace filepan::vfs {

// Compares file names for display. Pluggable so that the text module can provide ICU collation
// for the user's locale; the default below needs no locale data.
class NameCollator {
public:
    virtual ~NameCollator() = default;
    // Negative, zero or positive, like strcmp. Must be a strict weak ordering, and should only
    // return zero for identical names so that sorting is deterministic.
    [[nodiscard]] virtual int compare(std::string_view a, std::string_view b) const = 0;
};

// Natural ordering with ASCII case folding: digit runs compare by numeric value ("file2" before
// "file10"), letters ignore ASCII case, and other bytes (including UTF-8) compare by value.
// Ties are broken by fewer leading zeros, then by raw bytes, so only equal names compare equal.
int naturalCompare(std::string_view a, std::string_view b);

class NaturalNameCollator final : public NameCollator {
public:
    [[nodiscard]] int compare(std::string_view a, std::string_view b) const override {
        return naturalCompare(a, b);
    }
};

const NameCollator &defaultNameCollator();

enum class SortKey {
    kName,
    kSize,
    kModified,
    // By kind of file and MIME type, then extension.
    kType,
};

struct SortOptions {
    SortKey key = SortKey::kName;
    bool descending = false;
    // Directories (and links to them) before everything else, regardless of descending.
    bool directoriesFirst = true;
    // Null for defaultNameCollator(). Must outlive the comparator.
    const NameCollator *collator = nullptr;
};

// A strict weak ordering over entries. Every key falls back to the name, so the order is total
// for distinct names. Calls Entry::mimeType() for kType, so use it on the entries' owning thread.
class EntryComparator {
public:
    explicit EntryComparator(const SortOptions &options);
    bool operator()(const Entry &a, const Entry &b) const;

private:
    [[nodiscard]] int compareByKey(const Entry &a, const Entry &b) const;

    SortOptions options_;
    const NameCollator *collator_;
};

void sortEntries(std::span<Entry> entries, const SortOptions &options);

}  // namespace filepan::vfs
