// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/vfs/mime.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>

namespace filepan::vfs {

namespace {

// kTypes, kExtensions, kCompoundExtensions, kCaseSensitiveExtensions and kAliases, generated at
// build time by generate_mime_table.py from shared-mime-info and mime_policy.txt.
#include "mime_table.inc"

consteval bool isStrictlySorted(const auto &table, auto key) {
    for (std::size_t i = 1; i < std::size(table); ++i) {
        if (!(table[i - 1].*key < table[i].*key)) {
            return false;
        }
    }
    return true;
}
static_assert(isStrictlySorted(kTypes, &TypeEntry::type));
static_assert(isStrictlySorted(kExtensions, &ExtEntry::extension));
static_assert(isStrictlySorted(kCompoundExtensions, &ExtEntry::extension));
static_assert(isStrictlySorted(kCaseSensitiveExtensions, &ExtEntry::extension));
static_assert(isStrictlySorted(kAliases, &AliasEntry::alias));

consteval bool idsInRange(const auto &table) {
    return std::ranges::all_of(table, [](const auto &e) { return e.type < std::size(kTypes); });
}
static_assert(idsInRange(kExtensions) && idsInRange(kCompoundExtensions)
              && idsInRange(kCaseSensitiveExtensions) && idsInRange(kAliases));

// Longer than any extension in the tables.
constexpr std::size_t kMaxExtensionLength = 48;
// Longer than any type in the tables.
constexpr std::size_t kMaxTypeLength = 128;

consteval bool keysFit() {
    for (const auto table : {std::span<const ExtEntry>(kExtensions),
                             std::span<const ExtEntry>(kCompoundExtensions),
                             std::span<const ExtEntry>(kCaseSensitiveExtensions)}) {
        for (const auto &e : table) {
            if (e.extension.size() > kMaxExtensionLength) {
                return false;
            }
        }
    }
    for (const auto &e : kTypes) {
        if (e.type.size() > kMaxTypeLength) {
            return false;
        }
    }
    return true;
}
static_assert(keysFit());

constexpr char toLower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

// Lowercases into buffer; empty if too long to be in a table.
template <std::size_t N>
std::string_view lowercase(std::string_view text, char (&buffer)[N]) {
    if (text.size() > N) {
        return {};
    }
    std::ranges::transform(text, buffer, toLower);
    return {buffer, text.size()};
}

const TypeEntry *find(std::span<const ExtEntry> table, std::string_view key) {
    if (key.empty()) {
        return nullptr;
    }
    const auto it = std::ranges::lower_bound(table, key, {}, &ExtEntry::extension);
    return it != table.end() && it->extension == key ? &kTypes[it->type] : nullptr;
}

// A single extension: an exact-case match first (.C is C++, .c is C), then any case.
const TypeEntry *findSingle(std::string_view extension) {
    if (const auto *type = find(kCaseSensitiveExtensions, extension)) {
        return type;
    }
    char buffer[kMaxExtensionLength];
    return find(kExtensions, lowercase(extension, buffer));
}

struct Compound {
    std::string_view extension;  // As spelled in the name, e.g. "TAR.GZ".
    const TypeEntry *type = nullptr;
};

// The last two dot-separated parts of a name when they are a compound archive extension such as
// "tar.gz" or "src.rpm". Something must come before them, so ".tar.gz" alone is a hidden gz file.
Compound compoundExtension(std::string_view name) {
    const auto last = name.rfind('.');
    if (last == std::string_view::npos || last == 0) {
        return {};
    }
    const auto first = name.rfind('.', last - 1);
    if (first == std::string_view::npos || first == 0) {
        return {};
    }
    const auto extension = name.substr(first + 1);
    char buffer[kMaxExtensionLength];
    const auto *type = find(kCompoundExtensions, lowercase(extension, buffer));
    return type ? Compound{extension, type} : Compound{};
}

}  // namespace

std::string_view mimeTypeForExtension(std::string_view extension) {
    const TypeEntry *type = nullptr;
    if (extension.contains('.')) {
        char buffer[kMaxExtensionLength];
        type = find(kCompoundExtensions, lowercase(extension, buffer));
    } else {
        type = findSingle(extension);
    }
    return type ? type->type : std::string_view();
}

std::string_view extensionOf(std::string_view name) {
    if (const auto compound = compoundExtension(name); compound.type) {
        return compound.extension;
    }
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0) {
        return {};
    }
    return name.substr(dot + 1);
}

MimeType mimeTypeForFileName(std::string_view name) {
    const TypeEntry *type = compoundExtension(name).type;
    if (!type) {
        if (const auto dot = name.rfind('.'); dot != std::string_view::npos && dot != 0) {
            type = findSingle(name.substr(dot + 1));
        }
    }
    if (!type) {
        return {kGenericMimeType, FileKind::kOther};
    }
    return {type->type, type->kind};
}

FileKind fileKindForMimeType(std::string_view mimeType) {
    if (mimeType == kDirectoryMimeType) {
        return FileKind::kDirectory;
    }
    char buffer[kMaxTypeLength];
    const auto type = lowercase(mimeType, buffer);
    if (!type.empty()) {
        if (const auto it = std::ranges::lower_bound(kTypes, type, {}, &TypeEntry::type);
            it != std::end(kTypes) && it->type == type) {
            return it->kind;
        }
        if (const auto it = std::ranges::lower_bound(kAliases, type, {}, &AliasEntry::alias);
            it != std::end(kAliases) && it->alias == type) {
            return kTypes[it->type].kind;
        }
    }
    // Unknown to shared-mime-info: go by the top-level type.
    if (type.starts_with("image/")) return FileKind::kImage;
    if (type.starts_with("video/")) return FileKind::kVideo;
    if (type.starts_with("audio/")) return FileKind::kAudio;
    if (type.starts_with("font/")) return FileKind::kFont;
    if (type.starts_with("text/")) return FileKind::kText;
    return FileKind::kOther;
}

bool isArchiveName(std::string_view name) {
    return mimeTypeForFileName(name).kind == FileKind::kArchive;
}

}  // namespace filepan::vfs
