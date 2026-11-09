// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <algorithm>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <archive.h>
#include <archive_entry.h>
#include <sys/stat.h>

#include "filepan/archive_stream.hpp"
#include "test_util.hpp"

using filepan::ArchiveError;
using filepan::ArchiveReader;
using filepan::ArchiveSink;
using filepan::ArchiveSource;
using filepan::ArchiveWriter;
using filepan::ByteWriter;
using filepan::CreateArchiveOptions;

namespace {

constexpr int kFormatZip = ARCHIVE_FORMAT_ZIP;
constexpr int kFormatTar = ARCHIVE_FORMAT_TAR_PAX_RESTRICTED;
constexpr int kFormat7zip = ARCHIVE_FORMAT_7ZIP;

class MemorySink : public ArchiveSink {
public:
    bool write(const void *buffer, std::size_t size) override {
        if (failAfter && bytes.size() + size > *failAfter) {
            return false;
        }
        const auto *begin = static_cast<const char *>(buffer);
        bytes.append(begin, size);
        return true;
    }

    std::string bytes;
    std::optional<std::size_t> failAfter;
};

class MemorySource : public ArchiveSource {
public:
    explicit MemorySource(std::string bytes) : bytes_(std::move(bytes)) {}

    std::int64_t read(void *buffer, std::size_t size) override {
        if (failReads) {
            return -1;
        }
        // Small reads exercise libarchive's buffering across callbacks.
        const std::size_t length = std::min({size, bytes_.size() - position_, std::size_t{4096}});
        std::memcpy(buffer, bytes_.data() + position_, length);
        position_ += length;
        return static_cast<std::int64_t>(length);
    }

    std::int64_t seek(std::int64_t offset, int whence) override {
        ++seekCount;
        std::int64_t base = 0;
        if (whence == SEEK_CUR) {
            base = static_cast<std::int64_t>(position_);
        } else if (whence == SEEK_END) {
            base = static_cast<std::int64_t>(bytes_.size());
        }
        const std::int64_t position = base + offset;
        if (position < 0) {
            return -1;
        }
        position_ = std::min(static_cast<std::size_t>(position), bytes_.size());
        return static_cast<std::int64_t>(position_);
    }

    bool failReads = false;
    int seekCount = 0;

private:
    std::string bytes_;
    std::size_t position_ = 0;
};

struct Entry {
    std::uint32_t flags;
    std::uint32_t mode;
    std::int64_t size;
    std::int64_t uid;
    std::int64_t gid;
    std::int64_t mtime;
    std::optional<std::string> pathnameUtf8;
    std::optional<std::string> pathname;
    std::optional<std::string> unameUtf8;
    std::optional<std::string> symlinkUtf8;
};

template <typename T>
T get(const std::uint8_t *&p) {
    T value;
    std::memcpy(&value, p, sizeof(T));
    p += sizeof(T);
    return value;
}

std::optional<std::string> getString(const std::uint8_t *&p) {
    const auto length = get<std::int32_t>(p);
    if (length < 0) {
        return std::nullopt;
    }
    std::string string(reinterpret_cast<const char *>(p), static_cast<std::size_t>(length));
    p += length;
    return string;
}

std::vector<Entry> decode(const ByteWriter &out) {
    std::vector<Entry> entries;
    const std::uint8_t *p = out.data();
    const std::uint8_t *end = p + out.size();
    while (p < end) {
        Entry entry{};
        entry.flags = get<std::uint32_t>(p);
        entry.mode = get<std::uint32_t>(p);
        entry.size = get<std::int64_t>(p);
        entry.uid = get<std::int64_t>(p);
        entry.gid = get<std::int64_t>(p);
        entry.mtime = get<std::int64_t>(p);
        p += 8 * 5;  // mtime nanoseconds, atime, birthtime.
        entry.pathnameUtf8 = getString(p);
        entry.pathname = getString(p);
        entry.unameUtf8 = getString(p);
        getString(p);
        getString(p);
        getString(p);
        entry.symlinkUtf8 = getString(p);
        getString(p);
        entries.push_back(std::move(entry));
    }
    EXPECT(p == end);
    return entries;
}

std::string makeContent(std::size_t size) {
    std::string content(size, '\0');
    std::uint32_t state = 12345;
    for (auto &c : content) {
        state = state * 1103515245 + 12345;
        c = static_cast<char>(state >> 16);
    }
    return content;
}

const std::string kName = "d\xc3\xadr/\xe6\x96\x87\xe4\xbb\xb6.bin";  // "dír/文件.bin"

std::string writeArchive(int format, int filter, const std::string &password,
                         const std::string &content) {
    MemorySink sink;
    ArchiveError error;
    auto writer = ArchiveWriter::open(sink, CreateArchiveOptions{format, filter, password, 2},
                                      error);
    EXPECT(writer);
    ArchiveWriter::Entry directory;
    directory.pathname = "d\xc3\xadr/";
    directory.mode = S_IFDIR | 0755;
    directory.hasMtime = true;
    directory.mtime = 1700000000;
    EXPECT(writer->writeHeader(directory));
    ArchiveWriter::Entry file;
    file.pathname = kName;
    file.mode = S_IFREG | 0640;
    file.size = static_cast<std::int64_t>(content.size());
    file.hasMtime = true;
    file.mtime = 1700000001;
    file.hasOwner = true;
    file.uid = 1000;
    file.uname = "us\xc3\xa9r";
    file.hasGroup = true;
    file.gid = 1001;
    file.gname = "group";
    EXPECT(writer->writeHeader(file));
    // Uneven writes, like an InputStream copy.
    for (std::size_t offset = 0; offset < content.size(); offset += 7777) {
        EXPECT(writer->writeData(content.data() + offset,
                                 std::min<std::size_t>(7777, content.size() - offset)));
    }
    if (format != kFormat7zip) {
        // 7z stores symbolic links as files.
        ArchiveWriter::Entry link;
        link.pathname = "link";
        link.mode = S_IFLNK | 0777;
        link.hasSymlink = true;
        link.symlink = kName;
        EXPECT(writer->writeHeader(link));
    }
    EXPECT(writer->close());
    // Writing after closing fails rather than crashing.
    EXPECT(!writer->writeHeader(directory));
    writer.reset();
    return sink.bytes;
}

std::string readAll(ArchiveReader &reader) {
    std::string data;
    char buffer[5000];
    while (true) {
        const std::int64_t size = reader.readData(buffer, sizeof(buffer));
        EXPECT(size >= 0);
        if (size == 0) {
            return data;
        }
        data.append(buffer, static_cast<std::size_t>(size));
    }
}

const Entry *findEntry(const std::vector<Entry> &entries, const std::string &name,
                       std::uint64_t *index) {
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].pathnameUtf8 == name) {
            *index = i;
            return &entries[i];
        }
    }
    return nullptr;
}

std::vector<Entry> readEntries(ArchiveReader &reader) {
    ByteWriter out;
    // A small batch size exercises resuming between batches.
    ArchiveReader::Status status;
    while ((status = reader.readEntries(out, 2)) == ArchiveReader::Status::kOk) {
    }
    EXPECT(status == ArchiveReader::Status::kEnd);
    return decode(out);
}

void checkRoundTrip(int format, int filter, const std::string &password) {
    const std::string content = makeContent(300 * 1024 + 17);
    const std::string bytes = writeArchive(format, filter, password, content);
    const std::vector<std::string> passwords =
        password.empty() ? std::vector<std::string>{} : std::vector<std::string>{"wrong", password};
    for (const bool isSeekable : {true, false}) {
        if (format == kFormat7zip && !isSeekable) {
            // 7z has its index at the end, so it can't be streamed.
            MemorySource source(bytes);
            ArchiveError error;
            auto reader = ArchiveReader::openSource(source, isSeekable, passwords, error);
            ByteWriter out;
            EXPECT(!reader || reader->readEntries(out, 10) == ArchiveReader::Status::kError);
            continue;
        }
        MemorySource source(bytes);
        ArchiveError error;
        auto reader = ArchiveReader::openSource(source, isSeekable, passwords, error);
        EXPECT(reader);
        const auto entries = readEntries(*reader);
        EXPECT(entries.size() == (format == kFormat7zip ? 2u : 3u));
        std::uint64_t fileIndex;
        const Entry *file = findEntry(entries, kName, &fileIndex);
        EXPECT(file);
        EXPECT(S_ISREG(file->mode));
        // Streamed zip has only the local headers, which don't keep permissions, nor sizes that
        // follow the data.
        if (format != kFormatZip || isSeekable) {
            EXPECT((file->mode & 07777) == 0640);
            EXPECT(file->size == static_cast<std::int64_t>(content.size()));
        }
        EXPECT(file->mtime == 1700000001);
        EXPECT((file->flags & ArchiveReader::kEntryMtimeSet) != 0);
        EXPECT(((file->flags & ArchiveReader::kEntryEncrypted) != 0) == !password.empty());
        if (format == kFormatTar) {
            EXPECT(file->uid == 1000 && file->gid == 1001);
            EXPECT(file->unameUtf8 == "us\xc3\xa9r");
        }
        // Likewise for the type of a symbolic link.
        if (format != kFormat7zip && (format != kFormatZip || isSeekable)) {
            std::uint64_t linkIndex;
            const Entry *link = findEntry(entries, "link", &linkIndex);
            EXPECT(link && S_ISLNK(link->mode));
            EXPECT(link->symlinkUtf8 == kName);
        }
        if (format == kFormatZip && isSeekable) {
            // Zip reads its central directory from the end.
            EXPECT(source.seekCount > 0);
        }

        MemorySource dataSource(bytes);
        auto dataReader = ArchiveReader::openSource(dataSource, isSeekable, passwords, error);
        EXPECT(dataReader);
        EXPECT(dataReader->skipTo(fileIndex) == ArchiveReader::Status::kOk);
        EXPECT(readAll(*dataReader) == content);
        EXPECT(dataReader->skipTo(100) == ArchiveReader::Status::kEnd);

        // readHeader() agrees with readEntries().
        MemorySource headerSource(bytes);
        auto headerReader = ArchiveReader::openSource(headerSource, isSeekable, passwords, error);
        EXPECT(headerReader);
        std::vector<filepan::ArchiveEntryHeader> headers;
        filepan::ArchiveEntryHeader header;
        while (headerReader->readHeader(header) == ArchiveReader::Status::kOk) {
            headers.push_back(header);
        }
        EXPECT(headers.size() == entries.size());
        for (std::size_t i = 0; i < headers.size(); ++i) {
            EXPECT(headers[i].pathname == entries[i].pathnameUtf8.value_or(""));
            EXPECT(headers[i].mode == entries[i].mode && headers[i].size == entries[i].size);
            EXPECT(headers[i].flags == entries[i].flags && headers[i].mtime == entries[i].mtime);
            EXPECT(headers[i].symlink == entries[i].symlinkUtf8.value_or(""));
        }
    }
}

void testRoundTrips() {
    checkRoundTrip(kFormatZip, ARCHIVE_FILTER_NONE, "");
    checkRoundTrip(kFormatZip, ARCHIVE_FILTER_NONE, "s3cret");
    checkRoundTrip(kFormatTar, ARCHIVE_FILTER_XZ, "");
    checkRoundTrip(kFormatTar, ARCHIVE_FILTER_ZSTD, "");
    checkRoundTrip(kFormat7zip, ARCHIVE_FILTER_NONE, "");
}

void testMissingPassword() {
    const std::string content = makeContent(1000);
    const std::string bytes = writeArchive(kFormatZip, ARCHIVE_FILTER_NONE, "s3cret", content);
    for (const auto &passwords :
         {std::vector<std::string>{}, std::vector<std::string>{"wrong"}}) {
        MemorySource source(bytes);
        ArchiveError error;
        auto reader = ArchiveReader::openSource(source, true, passwords, error);
        EXPECT(reader);
        const auto entries = readEntries(*reader);
        std::uint64_t index;
        EXPECT(findEntry(entries, kName, &index));
        MemorySource dataSource(bytes);
        auto dataReader = ArchiveReader::openSource(dataSource, true, passwords, error);
        EXPECT(dataReader->skipTo(index) == ArchiveReader::Status::kOk);
        char buffer[100];
        EXPECT(dataReader->readData(buffer, sizeof(buffer)) == -1);
        // The app matches these messages to ask for a password.
        const ArchiveError dataError = dataReader->error();
        EXPECT(dataError.code == -1);  // ARCHIVE_ERRNO_MISC, which is private.
        EXPECT(dataError.message == "Passphrase required for this entry"
               || dataError.message == "Incorrect passphrase");
    }
}

void testUndeclaredEncoding() {
    // A tar written by a non-UTF-8 system: names are stored as raw bytes.
    const std::string rawName = "\xce\xc4\xbc\xfe.txt";  // "文件.txt" in GBK
    std::string bytes;
    {
        archive *a = archive_write_new();
        archive_write_set_format_ustar(a);
        std::vector<char> buffer(64 * 1024);
        std::size_t used = 0;
        EXPECT(archive_write_open_memory(a, buffer.data(), buffer.size(), &used) == ARCHIVE_OK);
        archive_entry *entry = archive_entry_new();
        archive_entry_copy_pathname(entry, rawName.c_str());
        archive_entry_set_mode(entry, S_IFREG | 0644);
        archive_entry_set_size(entry, 0);
        EXPECT(archive_write_header(a, entry) >= ARCHIVE_WARN);
        archive_entry_free(entry);
        EXPECT(archive_write_close(a) == ARCHIVE_OK);
        archive_write_free(a);
        bytes.assign(buffer.data(), used);
    }
    MemorySource source(bytes);
    ArchiveError error;
    auto reader = ArchiveReader::openSource(source, true, {}, error);
    EXPECT(reader);
    const auto entries = readEntries(*reader);
    EXPECT(entries.size() == 1);
    // Kotlin then decodes the stored bytes with the charset the user picked.
    EXPECT(!entries[0].pathnameUtf8);
    EXPECT(entries[0].pathname == rawName);
}

void testFailures() {
    const std::string bytes = writeArchive(kFormatZip, ARCHIVE_FILTER_NONE, "", makeContent(1000));
    {
        MemorySource source(bytes);
        source.failReads = true;
        ArchiveError error;
        EXPECT(!ArchiveReader::openSource(source, false, {}, error));
    }
    {
        MemorySource source("this is not an archive at all, just some text");
        ArchiveError error;
        EXPECT(!ArchiveReader::openSource(source, true, {}, error));
        EXPECT(!error.message.empty());
    }
    {
        // A truncated archive fails when reading headers or data, never crashes.
        MemorySource source(bytes.substr(0, bytes.size() / 2));
        ArchiveError error;
        auto reader = ArchiveReader::openSource(source, false, {}, error);
        if (reader) {
            ByteWriter out;
            while (reader->readEntries(out, 1) == ArchiveReader::Status::kOk) {
                char buffer[100];
                while (reader->readData(buffer, sizeof(buffer)) > 0) {
                }
            }
        }
    }
    {
        MemorySink sink;
        sink.failAfter = 100;
        ArchiveError error;
        auto writer = ArchiveWriter::open(
            sink, CreateArchiveOptions{kFormatZip, ARCHIVE_FILTER_NONE, "", 1}, error);
        EXPECT(writer);
        ArchiveWriter::Entry file;
        file.pathname = "file";
        file.mode = S_IFREG | 0644;
        const std::string content = makeContent(200 * 1024);
        file.size = static_cast<std::int64_t>(content.size());
        bool failed = !writer->writeHeader(file)
            || !writer->writeData(content.data(), content.size()) || !writer->close();
        EXPECT(failed);
        EXPECT(writer->error().code == EIO);
    }
}

void testOpenFile() {
    filepan::test::TempDir directory;
    const std::string content = makeContent(5000);
    const std::string bytes = writeArchive(kFormatZip, ARCHIVE_FILTER_NONE, "", content);
    const std::string path = (directory.path() / "a.zip").string();
    std::ofstream(path, std::ios::binary) << bytes;
    ArchiveError error;
    auto reader = ArchiveReader::openFile(path, {}, error);
    EXPECT(reader);
    std::uint64_t index;
    EXPECT(findEntry(readEntries(*reader), kName, &index));
    auto dataReader = ArchiveReader::openFile(path, {}, error);
    EXPECT(dataReader->skipTo(index) == ArchiveReader::Status::kOk);
    EXPECT(readAll(*dataReader) == content);
    EXPECT(!ArchiveReader::openFile((directory.path() / "missing.zip").string(), {}, error));
    EXPECT(error.code == ENOENT);
}

}  // namespace

int main() {
    // Bionic's default locale is UTF-8, which libarchive uses to convert names.
    EXPECT(std::setlocale(LC_CTYPE, "C.UTF-8"));
    testRoundTrips();
    testMissingPassword();
    testUndeclaredEncoding();
    testFailures();
    testOpenFile();
    std::puts("archive_stream_test: OK");
    return 0;
}
