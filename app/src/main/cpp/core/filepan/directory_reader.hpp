// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "filepan/byte_writer.hpp"

struct statx;

namespace filepan {

// One directory entry as read, valid only during DirectoryEntrySink::onEntry(). The stat
// pointers are null when that statx wasn't requested or failed.
struct DirectoryEntryView {
    std::string_view name;
    unsigned char type;  // DT_* from the dirent.
    const struct statx *stat;  // lstat-like (does not follow symlinks).
    int statError;  // errno if stat was requested and failed, else 0.
    bool hasLinkTarget;
    std::string_view linkTarget;
    const struct statx *targetStat;  // Follows symlinks.
    int targetStatError;
};

class DirectoryEntrySink {
public:
    virtual ~DirectoryEntrySink() = default;
    virtual void onEntry(const DirectoryEntryView &entry) = 0;
};

// Reads a directory in getdents64-sized batches and serializes each batch into one buffer, so
// that the JVM crosses JNI once per batch instead of once (or several times) per entry.
//
// Batch format, all little-endian, repeated until the end of the buffer:
//
//   u8  d_type                      DT_* from the dirent
//   u8  flags                       kStatOk | kHasLinkTarget | kTargetStatOk
//   u16 name length, then the name bytes (not NUL-terminated)
//   if kStatOk:        Stat         lstat-like (does not follow symlinks)
//   else if stat was requested:     i32 errno
//   if kHasLinkTarget: u16 target length, then the target bytes
//                      if kTargetStatOk: Stat (follows symlinks)
//                      else:             i32 errno
//
//   Stat: u32 mode, u32 uid, u32 gid, u32 nlink, i64 size,
//         i64 atime_sec, u32 atime_nsec, i64 mtime_sec, u32 mtime_nsec,
//         i64 btime_sec, u32 btime_nsec (btime_sec == kNoBirthTime if unknown),
//         u64 dev, u64 ino
//
// "." and ".." are never emitted.
class DirectoryReader {
public:
    static constexpr std::uint8_t kStatOk = 1 << 0;
    static constexpr std::uint8_t kHasLinkTarget = 1 << 1;
    static constexpr std::uint8_t kTargetStatOk = 1 << 2;
    static constexpr std::int64_t kNoBirthTime = INT64_MIN;
    static constexpr std::size_t kStatSize = 4 * 4 + 8 + (8 + 4) * 3 + 8 * 2;

    // Returns the directory fd, or -errno.
    static int open(const char *path);

    explicit DirectoryReader(int fd) : fd_(fd) {}
    ~DirectoryReader();
    DirectoryReader(const DirectoryReader &) = delete;
    DirectoryReader &operator=(const DirectoryReader &) = delete;

    // Appends the next batch to out. Returns 0 on success (out is left empty at end of
    // directory), or an errno.
    int readBatch(bool withStat, ByteWriter &out);

    // Hands the entries of the next getdents64 buffer to sink without serializing them. Returns
    // 0 or an errno. A call may emit no entries, so loop until atEnd().
    int readEntries(bool withStat, DirectoryEntrySink &sink);
    [[nodiscard]] bool atEnd() const { return eof_; }
    // The directory, for *at() calls on its entries; owned by the reader.
    [[nodiscard]] int fd() const { return fd_; }

private:
    int fd_;
    bool eof_ = false;
    alignas(8) std::uint8_t buffer_[32 * 1024];
};

}  // namespace filepan
