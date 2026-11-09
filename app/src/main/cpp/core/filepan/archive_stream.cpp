// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/archive_stream.hpp"

#include <cerrno>
#include <cstdio>
#include <string_view>

#include <archive.h>
#include <archive_entry.h>

namespace filepan {

namespace {

constexpr std::size_t kReadBlockSize = 1024 * 1024;

ArchiveError archiveError(archive *a) {
    ArchiveError error;
    error.code = archive_errno(a);
    if (const char *message = archive_error_string(a)) {
        error.message = message;
    }
    return error;
}

void putString(ByteWriter &out, const char *string) {
    if (!string) {
        out.put<std::int32_t>(-1);
        return;
    }
    const std::string_view view(string);
    out.put(static_cast<std::int32_t>(view.size()));
    out.putBytes(view);
}

void putEntry(ByteWriter &out, archive_entry *entry) {
    std::uint32_t flags = 0;
    if (archive_entry_is_encrypted(entry)) {
        flags |= ArchiveReader::kEntryEncrypted;
    }
    if (archive_entry_mtime_is_set(entry)) {
        flags |= ArchiveReader::kEntryMtimeSet;
    }
    if (archive_entry_atime_is_set(entry)) {
        flags |= ArchiveReader::kEntryAtimeSet;
    }
    if (archive_entry_birthtime_is_set(entry)) {
        flags |= ArchiveReader::kEntryBirthtimeSet;
    }
    out.put(flags);
    out.put(static_cast<std::uint32_t>(archive_entry_mode(entry)));
    out.put(static_cast<std::int64_t>(archive_entry_size(entry)));
    out.put(static_cast<std::int64_t>(archive_entry_uid(entry)));
    out.put(static_cast<std::int64_t>(archive_entry_gid(entry)));
    out.put(static_cast<std::int64_t>(archive_entry_mtime(entry)));
    out.put(static_cast<std::int64_t>(archive_entry_mtime_nsec(entry)));
    out.put(static_cast<std::int64_t>(archive_entry_atime(entry)));
    out.put(static_cast<std::int64_t>(archive_entry_atime_nsec(entry)));
    out.put(static_cast<std::int64_t>(archive_entry_birthtime(entry)));
    out.put(static_cast<std::int64_t>(archive_entry_birthtime_nsec(entry)));
    putString(out, archive_entry_pathname_utf8(entry));
    putString(out, archive_entry_pathname(entry));
    putString(out, archive_entry_uname_utf8(entry));
    putString(out, archive_entry_uname(entry));
    putString(out, archive_entry_gname_utf8(entry));
    putString(out, archive_entry_gname(entry));
    putString(out, archive_entry_symlink_utf8(entry));
    putString(out, archive_entry_symlink(entry));
}

archive *newReadArchive(const std::vector<std::string> &passwords) {
    archive *a = archive_read_new();
    if (!a) {
        return nullptr;
    }
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    for (const auto &password : passwords) {
        archive_read_add_passphrase(a, password.c_str());
    }
    return a;
}

}  // namespace

ArchiveReader::ArchiveReader(archive *a) : archive_(a) {}

ArchiveReader::~ArchiveReader() { archive_read_free(archive_); }

std::unique_ptr<ArchiveReader> ArchiveReader::openFile(const std::string &path,
                                                       const std::vector<std::string> &passwords,
                                                       ArchiveError &error) {
    archive *a = newReadArchive(passwords);
    if (!a) {
        error = {ENOMEM, "Can't allocate archive"};
        return nullptr;
    }
    std::unique_ptr<ArchiveReader> reader(new ArchiveReader(a));
    if (archive_read_open_filename(a, path.c_str(), kReadBlockSize) != ARCHIVE_OK) {
        error = archiveError(a);
        return nullptr;
    }
    return reader;
}

std::unique_ptr<ArchiveReader> ArchiveReader::openSource(ArchiveSource &source, bool isSeekable,
                                                         const std::vector<std::string> &passwords,
                                                         ArchiveError &error) {
    archive *a = newReadArchive(passwords);
    if (!a) {
        error = {ENOMEM, "Can't allocate archive"};
        return nullptr;
    }
    std::unique_ptr<ArchiveReader> reader(new ArchiveReader(a));
    reader->source_ = &source;
    reader->buffer_ = std::make_unique<std::uint8_t[]>(kReadBlockSize);
    archive_read_set_callback_data(a, reader.get());
    archive_read_set_read_callback(
        a, [](archive *a, void *data, const void **buffer) -> la_ssize_t {
            auto *reader = static_cast<ArchiveReader *>(data);
            const std::int64_t size = reader->source_->read(reader->buffer_.get(), kReadBlockSize);
            if (size < 0) {
                archive_set_error(a, EIO, "Can't read archive");
                return ARCHIVE_FATAL;
            }
            *buffer = reader->buffer_.get();
            return static_cast<la_ssize_t>(size);
        });
    if (isSeekable) {
        archive_read_set_seek_callback(
            a, [](archive *a, void *data, la_int64_t offset, int whence) -> la_int64_t {
                auto *reader = static_cast<ArchiveReader *>(data);
                const std::int64_t position = reader->source_->seek(offset, whence);
                if (position < 0) {
                    archive_set_error(a, EIO, "Can't seek archive");
                    return ARCHIVE_FATAL;
                }
                return position;
            });
        archive_read_set_skip_callback(
            a, [](archive *a, void *data, la_int64_t request) -> la_int64_t {
                auto *reader = static_cast<ArchiveReader *>(data);
                if (reader->source_->seek(request, SEEK_CUR) < 0) {
                    archive_set_error(a, EIO, "Can't seek archive");
                    return ARCHIVE_FATAL;
                }
                return request;
            });
    }
    if (archive_read_open1(a) != ARCHIVE_OK) {
        error = archiveError(a);
        return nullptr;
    }
    return reader;
}

ArchiveReader::Status ArchiveReader::nextHeader(archive_entry **entry) {
    const int status = archive_read_next_header(archive_, entry);
    if (status == ARCHIVE_EOF) {
        return Status::kEnd;
    }
    // A warning is e.g. a name that can't be converted, which still has its stored form.
    if (status < ARCHIVE_WARN) {
        return Status::kError;
    }
    ++nextIndex_;
    return Status::kOk;
}

ArchiveReader::Status ArchiveReader::readEntries(ByteWriter &out, std::size_t maxCount) {
    for (std::size_t count = 0; count < maxCount; ++count) {
        archive_entry *entry;
        const Status status = nextHeader(&entry);
        if (status != Status::kOk) {
            return status;
        }
        putEntry(out, entry);
    }
    return Status::kOk;
}

ArchiveReader::Status ArchiveReader::readHeader(ArchiveEntryHeader &header) {
    archive_entry *entry;
    const Status status = nextHeader(&entry);
    if (status != Status::kOk) {
        return status;
    }
    header.flags = 0;
    if (archive_entry_is_encrypted(entry)) {
        header.flags |= kEntryEncrypted;
    }
    if (archive_entry_mtime_is_set(entry)) {
        header.flags |= kEntryMtimeSet;
    }
    if (archive_entry_atime_is_set(entry)) {
        header.flags |= kEntryAtimeSet;
    }
    if (archive_entry_birthtime_is_set(entry)) {
        header.flags |= kEntryBirthtimeSet;
    }
    header.mode = static_cast<std::uint32_t>(archive_entry_mode(entry));
    header.size = archive_entry_size(entry);
    header.uid = archive_entry_uid(entry);
    header.gid = archive_entry_gid(entry);
    header.mtime = archive_entry_mtime(entry);
    header.mtimeNsec = archive_entry_mtime_nsec(entry);
    header.atime = archive_entry_atime(entry);
    header.atimeNsec = archive_entry_atime_nsec(entry);
    header.birthtime = archive_entry_birthtime(entry);
    header.birthtimeNsec = archive_entry_birthtime_nsec(entry);
    const auto assign = [](std::string &out, const char *utf8, const char *stored) {
        const char *value = utf8 ? utf8 : stored;
        out.assign(value ? value : "");
    };
    assign(header.pathname, archive_entry_pathname_utf8(entry), archive_entry_pathname(entry));
    assign(header.symlink, archive_entry_symlink_utf8(entry), archive_entry_symlink(entry));
    assign(header.hardlink, archive_entry_hardlink_utf8(entry), archive_entry_hardlink(entry));
    return Status::kOk;
}

ArchiveReader::Status ArchiveReader::skipTo(std::uint64_t index) {
    while (nextIndex_ <= index) {
        archive_entry *entry;
        const Status status = nextHeader(&entry);
        if (status != Status::kOk) {
            return status;
        }
    }
    return Status::kOk;
}

std::int64_t ArchiveReader::readData(void *buffer, std::size_t size) {
    const la_ssize_t result = archive_read_data(archive_, buffer, size);
    return result < 0 ? -1 : result;
}

ArchiveError ArchiveReader::error() const { return archiveError(archive_); }

ArchiveWriter::ArchiveWriter(archive *a) : archive_(a) {}

ArchiveWriter::~ArchiveWriter() { archive_write_free(archive_); }

std::unique_ptr<ArchiveWriter> ArchiveWriter::open(ArchiveSink &sink,
                                                   const CreateArchiveOptions &options,
                                                   ArchiveError &error) {
    archive *a = archive_write_new();
    if (!a) {
        error = {ENOMEM, "Can't allocate archive"};
        return nullptr;
    }
    std::unique_ptr<ArchiveWriter> writer(new ArchiveWriter(a));
    if (!configureArchiveWrite(a, options)) {
        error = archiveError(a);
        return nullptr;
    }
    // A failed sink is reported to libarchive as written, so that it can still finish and free
    // the entry in progress: it leaks a zip entry's compressor when freed in the fatal state.
    writer->sink_ = &sink;
    const auto write = [](archive *, void *data, const void *buffer, size_t size) -> la_ssize_t {
        auto *writer = static_cast<ArchiveWriter *>(data);
        if (!writer->sinkFailed_ && !writer->sink_->write(buffer, size)) {
            writer->sinkFailed_ = true;
        }
        return static_cast<la_ssize_t>(size);
    };
    if (archive_write_open2(a, writer.get(), nullptr, write, nullptr, nullptr) != ARCHIVE_OK
        || writer->sinkFailed_) {
        error = writer->error();
        return nullptr;
    }
    return writer;
}

bool ArchiveWriter::writeHeader(const Entry &entry) {
    std::unique_ptr<archive_entry, decltype(&archive_entry_free)> archiveEntry(
        archive_entry_new2(archive_), archive_entry_free);
    if (!archiveEntry) {
        archive_set_error(archive_, ENOMEM, "Can't allocate entry");
        return false;
    }
    archive_entry *e = archiveEntry.get();
    archive_entry_set_pathname_utf8(e, entry.pathname.c_str());
    archive_entry_set_mode(e, entry.mode);
    archive_entry_set_size(e, entry.size);
    if (entry.hasMtime) {
        archive_entry_set_mtime(e, entry.mtime, entry.mtimeNsec);
    }
    if (entry.hasAtime) {
        archive_entry_set_atime(e, entry.atime, entry.atimeNsec);
    }
    if (entry.hasBirthtime) {
        archive_entry_set_birthtime(e, entry.birthtime, entry.birthtimeNsec);
    }
    if (entry.hasOwner) {
        archive_entry_set_uid(e, entry.uid);
        if (!entry.uname.empty()) {
            archive_entry_set_uname_utf8(e, entry.uname.c_str());
        }
    }
    if (entry.hasGroup) {
        archive_entry_set_gid(e, entry.gid);
        if (!entry.gname.empty()) {
            archive_entry_set_gname_utf8(e, entry.gname.c_str());
        }
    }
    if (entry.hasSymlink) {
        archive_entry_set_symlink_utf8(e, entry.symlink.c_str());
    }
    return archive_write_header(archive_, e) >= ARCHIVE_WARN && !sinkFailed_;
}

bool ArchiveWriter::writeData(const void *buffer, std::size_t size) {
    const auto *bytes = static_cast<const std::uint8_t *>(buffer);
    while (size > 0) {
        const la_ssize_t written = archive_write_data(archive_, bytes, size);
        if (written < 0 || sinkFailed_) {
            return false;
        }
        // Data beyond the size in the header is dropped.
        if (written == 0) {
            break;
        }
        bytes += written;
        size -= static_cast<std::size_t>(written);
    }
    return true;
}

bool ArchiveWriter::close() {
    return archive_write_close(archive_) == ARCHIVE_OK && !sinkFailed_;
}

ArchiveError ArchiveWriter::error() const {
    if (sinkFailed_) {
        return {EIO, "Can't write archive"};
    }
    return archiveError(archive_);
}

}  // namespace filepan
