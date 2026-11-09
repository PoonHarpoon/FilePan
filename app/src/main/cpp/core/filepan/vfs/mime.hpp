// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <string_view>

namespace filepan::vfs {

// A coarse category for picking an icon.
enum class FileKind : std::uint8_t {
    kDirectory,
    kImage,
    kVideo,
    kAudio,
    kArchive,
    kApk,
    kPdf,
    kDocument,
    kSpreadsheet,
    kPresentation,
    kText,
    kCode,
    kFont,
    kOther,
};

// type points to static storage, so a MimeType is cheap to copy and store per entry.
struct MimeType {
    std::string_view type;
    FileKind kind = FileKind::kOther;

    friend bool operator==(const MimeType &, const MimeType &) = default;
};

inline constexpr std::string_view kDirectoryMimeType = "inode/directory";
inline constexpr std::string_view kGenericMimeType = "application/octet-stream";

// MIME types come from freedesktop.org shared-mime-info (third_party/shared-mime-info), by file
// name only, with FilePan's overrides, additions and kinds in mime_policy.txt. Types are lower
// case, as Android spells them (MimeTypeMap, Intent.normalizeMimeType).

// The MIME type for an extension without its leading dot, ignoring ASCII case except where
// shared-mime-info is case-sensitive (.C is C++, .c is C). A compound extension such as "tar.gz"
// works too. Empty if unknown.
std::string_view mimeTypeForExtension(std::string_view extension);

// The extension of a file name without the dot: a compound archive extension such as "tar.gz" or
// "src.rpm" when the name ends in one, otherwise what follows the last dot ("pdf.gz" is "gz"). Empty when there is
// none, including for names that only start with a dot (".profile").
std::string_view extensionOf(std::string_view name);

// The MIME type and kind of a regular file, by name. Unknown extensions get kGenericMimeType.
MimeType mimeTypeForFileName(std::string_view name);

inline MimeType directoryMimeType() { return {kDirectoryMimeType, FileKind::kDirectory}; }

// The kind of a MIME type or one of its aliases, ignoring ASCII case. Types shared-mime-info
// doesn't know go by their top-level type.
FileKind fileKindForMimeType(std::string_view mimeType);

// Whether a file name looks like an archive FilePan's libarchive build can open, by its (possibly
// compound) extension. APKs are kApk and e-books and Office files kDocument and the like, not
// archives, though they are zips too.
bool isArchiveName(std::string_view name);

}  // namespace filepan::vfs
