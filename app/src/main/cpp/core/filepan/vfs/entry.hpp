// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <string>

#include "filepan/vfs/mime.hpp"

namespace filepan::vfs {

enum class EntryType : std::uint8_t {
    kFile,
    kDirectory,
    kSymlink,
    // Devices, FIFOs, sockets, and anything a backend can't classify.
    kOther,
};

struct Timestamp {
    std::int64_t seconds = 0;
    std::uint32_t nanoseconds = 0;

    friend auto operator<=>(const Timestamp &, const Timestamp &) = default;
};

// One file as listed or stat'ed. Plain data: backends fill it on an I/O thread, then it is moved
// to whoever displays it.
struct Entry {
    std::string name;  // Raw bytes, not necessarily UTF-8.
    EntryType type = EntryType::kOther;
    // For symlinks: the type of what the link resolves to, or kOther when it is broken
    // (brokenLink) or the backend didn't resolve it. Equal to type for everything else.
    EntryType targetType = EntryType::kOther;
    bool brokenLink = false;
    // Starts with a dot. Backends with another convention may set it otherwise.
    bool hidden = false;
    // Only the type and name are valid without it, e.g. when stat failed or wasn't requested.
    bool hasStat = false;
    bool hasBirthTime = false;
    // For symlinks, the target's size when it resolves; directory sizes are as the backend
    // reports them.
    std::uint64_t size = 0;
    Timestamp modifiedTime;
    Timestamp accessTime;
    Timestamp birthTime;
    std::uint32_t mode = 0;  // st_mode: type and permission bits.
    std::uint32_t uid = 0;
    std::uint32_t gid = 0;
    std::uint32_t linkCount = 0;
    // Device and inode, when the backend has them, to detect hard links and loops.
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::string linkTarget;  // For symlinks; raw bytes.

    // A directory, or a symlink that resolves to one.
    [[nodiscard]] bool isDirectoryLike() const { return targetType == EntryType::kDirectory; }

    // Computed from the name and type on first use and cached. Not thread-safe: call it from
    // the one thread that owns the entry.
    [[nodiscard]] const MimeType &mimeType() const {
        if (mimeType_.type.empty()) {
            mimeType_ = isDirectoryLike() ? directoryMimeType()
                        : targetType == EntryType::kFile ? mimeTypeForFileName(name)
                                                         : MimeType{kGenericMimeType};
        }
        return mimeType_;
    }
    // For backends that know better than the name, e.g. from content sniffing.
    void setMimeType(MimeType mimeType) { mimeType_ = mimeType; }

    static bool isHiddenName(std::string_view name) { return !name.empty() && name[0] == '.'; }

private:
    mutable MimeType mimeType_;
};

}  // namespace filepan::vfs
