// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cerrno>
#include <cstdint>
#include <optional>
#include <span>

#include "filepan/error.hpp"
#include "filepan/vfs/backend.hpp"
#include "filepan/vfs/entry.hpp"
#include "filepan/vfs/location.hpp"

namespace filepan::vfs {

// What a file looked like when it was read, to tell whether something else has written it since.
struct FileStamp {
    std::uint64_t size = 0;
    Timestamp modifiedTime;

    static FileStamp of(const Entry &entry) { return {entry.size, entry.modifiedTime}; }
    bool operator==(const FileStamp &) const = default;
};

// The error saveFile() returns when the file changed (or went away) since it was read.
inline constexpr int kSaveConflict = ESTALE;

// Replaces the file at location with bytes, or creates it. The bytes go to a hidden sibling that is
// then renamed over the file, so a failure part way leaves the old contents whole; the new file
// keeps the old one's permission bits where the backend can set them. A symlink or a file with
// other hard links is written in place instead, so that the link and the other names keep pointing
// at it. With expected, the file must still match it, or the save fails with kSaveConflict before
// writing anything. Returns the saved file's entry, for the next save's expected. Synchronous: call
// it on an I/O thread.
Result<Entry> saveFile(Backend &backend, const Location &location, std::span<const std::uint8_t> bytes,
                       std::optional<FileStamp> expected);

}  // namespace filepan::vfs
