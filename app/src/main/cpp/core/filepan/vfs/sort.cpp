// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/vfs/sort.hpp"

#include <algorithm>
#include <cstddef>

namespace filepan::vfs {

namespace {

bool isDigit(unsigned char c) { return c >= '0' && c <= '9'; }

unsigned char foldCase(unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }

int sign(int value) { return (value > 0) - (value < 0); }

template <typename T>
int compareValues(const T &a, const T &b) {
    return a < b ? -1 : b < a ? 1 : 0;
}

}  // namespace

int naturalCompare(std::string_view a, std::string_view b) {
    std::size_t i = 0;
    std::size_t j = 0;
    // The first difference in leading zeros, for "1" < "01" when everything else is equal.
    int zerosTie = 0;
    while (i < a.size() && j < b.size()) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[j]);
        if (isDigit(ca) && isDigit(cb)) {
            std::size_t startA = i;
            std::size_t startB = j;
            while (startA < a.size() && a[startA] == '0') ++startA;
            while (startB < b.size() && b[startB] == '0') ++startB;
            std::size_t endA = startA;
            std::size_t endB = startB;
            while (endA < a.size() && isDigit(a[endA])) ++endA;
            while (endB < b.size() && isDigit(b[endB])) ++endB;
            // Without leading zeros, a longer run is a larger number.
            if (endA - startA != endB - startB) {
                return endA - startA < endB - startB ? -1 : 1;
            }
            if (const int order = a.substr(startA, endA - startA)
                                      .compare(b.substr(startB, endB - startB));
                order != 0) {
                return sign(order);
            }
            if (zerosTie == 0 && startA - i != startB - j) {
                zerosTie = startA - i < startB - j ? -1 : 1;
            }
            i = endA;
            j = endB;
            continue;
        }
        const auto fa = foldCase(ca);
        const auto fb = foldCase(cb);
        if (fa != fb) {
            return fa < fb ? -1 : 1;
        }
        ++i;
        ++j;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    if (zerosTie != 0) return zerosTie;
    return sign(a.compare(b));
}

const NameCollator &defaultNameCollator() {
    static const NaturalNameCollator collator;
    return collator;
}

EntryComparator::EntryComparator(const SortOptions &options)
    : options_(options),
      collator_(options.collator ? options.collator : &defaultNameCollator()) {}

bool EntryComparator::operator()(const Entry &a, const Entry &b) const {
    if (options_.directoriesFirst) {
        const bool aIsDirectory = a.isDirectoryLike();
        if (aIsDirectory != b.isDirectoryLike()) {
            return aIsDirectory;
        }
    }
    const int order = compareByKey(a, b);
    return options_.descending ? order > 0 : order < 0;
}

int EntryComparator::compareByKey(const Entry &a, const Entry &b) const {
    int order = 0;
    switch (options_.key) {
        case SortKey::kName:
            break;
        case SortKey::kSize:
            order = compareValues(a.size, b.size);
            break;
        case SortKey::kModified:
            order = compareValues(a.modifiedTime, b.modifiedTime);
            break;
        case SortKey::kType: {
            const auto &typeA = a.mimeType();
            const auto &typeB = b.mimeType();
            order = compareValues(typeA.kind, typeB.kind);
            if (order == 0) {
                order = sign(typeA.type.compare(typeB.type));
            }
            if (order == 0) {
                order = collator_->compare(extensionOf(a.name), extensionOf(b.name));
            }
            break;
        }
    }
    return order != 0 ? order : collator_->compare(a.name, b.name);
}

void sortEntries(std::span<Entry> entries, const SortOptions &options) {
    std::ranges::sort(entries, EntryComparator(options));
}

}  // namespace filepan::vfs
