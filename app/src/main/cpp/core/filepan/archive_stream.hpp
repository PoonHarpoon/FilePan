// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "filepan/byte_writer.hpp"
#include "filepan/create_archive.hpp"

struct archive;
struct archive_entry;

namespace filepan {

// libarchive's errno and message for a failed operation. An error from the source or sink itself
// (e.g. a Java exception) is reported there instead.
struct ArchiveError {
    int code = 0;
    std::string message;
};

// Where an ArchiveReader reads from when the archive isn't a local file.
class ArchiveSource {
public:
    virtual ~ArchiveSource() = default;

    // Reads up to size bytes. Returns 0 at the end, or -1 on failure.
    virtual std::int64_t read(void *buffer, std::size_t size) = 0;

    // Only called if the source was opened as seekable. Returns the new position, or -1 on
    // failure.
    virtual std::int64_t seek(std::int64_t offset, int whence) = 0;
};

// One archive header, as ArchiveReader::readHeader() returns it.
struct ArchiveEntryHeader {
    std::uint32_t flags = 0;  // ArchiveReader::kEntry*
    std::uint32_t mode = 0;   // Type and permissions; the type may be 0 for a regular file.
    std::int64_t size = 0;
    std::int64_t uid = 0;
    std::int64_t gid = 0;
    std::int64_t mtime = 0;
    std::int64_t mtimeNsec = 0;
    std::int64_t atime = 0;
    std::int64_t atimeNsec = 0;
    std::int64_t birthtime = 0;
    std::int64_t birthtimeNsec = 0;
    // UTF-8 when the archive's names convert, otherwise as stored.
    std::string pathname;
    std::string symlink;
    // For a hard link (tar, cpio): the pathname of the entry holding the data.
    std::string hardlink;
};

// Reads headers and entry data from an archive, in order.
class ArchiveReader {
public:
    enum class Status { kOk, kEnd, kError };

    ~ArchiveReader();
    ArchiveReader(const ArchiveReader &) = delete;
    ArchiveReader &operator=(const ArchiveReader &) = delete;

    // Return null on failure, with the error in error.
    static std::unique_ptr<ArchiveReader> openFile(const std::string &path,
                                                   const std::vector<std::string> &passwords,
                                                   ArchiveError &error);
    // The source must outlive the reader. Random access lets zip and 7z read their central
    // directory instead of scanning the whole archive.
    static std::unique_ptr<ArchiveReader> openSource(ArchiveSource &source, bool isSeekable,
                                                     const std::vector<std::string> &passwords,
                                                     ArchiveError &error);

    // Appends up to maxCount headers to out, each little-endian:
    //   u32 flags (kEntry*), u32 mode (type and permissions), i64 size, i64 uid, i64 gid,
    //   i64 mtime seconds, i64 mtime nanoseconds, then the same for atime and birthtime,
    //   then 8 strings, each an i32 length (-1 if unset) and bytes: pathname as UTF-8 and as
    //   stored, uname as UTF-8 and as stored, gname likewise, symlink target likewise.
    // A string "as stored" is in the archive's own encoding when it doesn't say it's UTF-8.
    // Returns kEnd when there were no more headers; out may still have gained some.
    Status readEntries(ByteWriter &out, std::size_t maxCount);

    // Reads the next header into header, for callers in C++. Returns kEnd after the last one.
    Status readHeader(ArchiveEntryHeader &header);

    // Advances to the header at index without reading data, so that readData() reads its data.
    // Headers are counted from 0; index must not be before the next one.
    Status skipTo(std::uint64_t index);

    // Reads data of the current entry. Returns the size read, 0 at its end, or -1 on failure.
    std::int64_t readData(void *buffer, std::size_t size);

    [[nodiscard]] ArchiveError error() const;

    static constexpr std::uint32_t kEntryEncrypted = 1u << 0;
    static constexpr std::uint32_t kEntryMtimeSet = 1u << 1;
    static constexpr std::uint32_t kEntryAtimeSet = 1u << 2;
    static constexpr std::uint32_t kEntryBirthtimeSet = 1u << 3;

private:
    explicit ArchiveReader(archive *a);

    Status nextHeader(archive_entry **entry);

    archive *archive_;
    std::uint64_t nextIndex_ = 0;
    // For ArchiveSource reads, which libarchive needs to stay valid until the next one.
    std::unique_ptr<std::uint8_t[]> buffer_;
    ArchiveSource *source_ = nullptr;
};

// Where an ArchiveWriter writes to.
class ArchiveSink {
public:
    virtual ~ArchiveSink() = default;

    // Writes all of the buffer. Returns false on failure.
    virtual bool write(const void *buffer, std::size_t size) = 0;
};

// Writes a new archive entry by entry.
class ArchiveWriter {
public:
    struct Entry {
        std::string pathname;  // UTF-8
        std::uint32_t mode = 0;  // Type and permissions.
        std::int64_t size = 0;
        bool hasMtime = false;
        std::int64_t mtime = 0;
        std::int64_t mtimeNsec = 0;
        bool hasAtime = false;
        std::int64_t atime = 0;
        std::int64_t atimeNsec = 0;
        bool hasBirthtime = false;
        std::int64_t birthtime = 0;
        std::int64_t birthtimeNsec = 0;
        bool hasOwner = false;
        std::int64_t uid = 0;
        std::string uname;  // UTF-8; empty for none.
        bool hasGroup = false;
        std::int64_t gid = 0;
        std::string gname;  // UTF-8; empty for none.
        bool hasSymlink = false;
        std::string symlink;  // UTF-8
    };

    ~ArchiveWriter();
    ArchiveWriter(const ArchiveWriter &) = delete;
    ArchiveWriter &operator=(const ArchiveWriter &) = delete;

    // The sink must outlive the writer. Returns null on failure, with the error in error.
    static std::unique_ptr<ArchiveWriter> open(ArchiveSink &sink,
                                               const CreateArchiveOptions &options,
                                               ArchiveError &error);

    bool writeHeader(const Entry &entry);
    bool writeData(const void *buffer, std::size_t size);
    // Finishes the archive. Writing after closing fails.
    bool close();

    [[nodiscard]] ArchiveError error() const;

private:
    explicit ArchiveWriter(archive *a);

    archive *archive_;
    ArchiveSink *sink_ = nullptr;
    bool sinkFailed_ = false;
};

}  // namespace filepan
