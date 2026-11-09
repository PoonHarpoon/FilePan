// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Checks the batch format against the spec in directory_reader.hpp, which the Kotlin decoder
// (NativeDirectory.decodeBatch) is tested against as well.

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <string>

#include <dirent.h>
#include <sys/stat.h>

#include "filepan/directory_reader.hpp"
#include "test_util.hpp"

using filepan::ByteWriter;
using filepan::DirectoryReader;
using filepan::test::TempDir;

namespace {

struct Stat {
    std::uint32_t mode;
    std::int64_t size;
    std::int64_t mtimeSec;
    std::uint64_t ino;
};

struct Entry {
    std::uint8_t type;
    std::uint8_t flags;
    bool hasStat = false;
    Stat stat{};
    std::int32_t statErrno = 0;
    std::string target;
    Stat targetStat{};
    std::int32_t targetErrno = 0;
};

class Reader {
public:
    explicit Reader(const ByteWriter &writer)
        : data_(writer.data()), end_(writer.data() + writer.size()) {}

    [[nodiscard]] bool done() const { return data_ == end_; }

    template <typename T>
    T get() {
        EXPECT(data_ + sizeof(T) <= end_);
        T value;
        std::memcpy(&value, data_, sizeof(T));
        data_ += sizeof(T);
        return value;
    }

    std::string getString() {
        const auto length = get<std::uint16_t>();
        EXPECT(data_ + length <= end_);
        std::string string(reinterpret_cast<const char *>(data_), length);
        data_ += length;
        return string;
    }

    Stat getStat() {
        const auto *start = data_;
        Stat stat{};
        stat.mode = get<std::uint32_t>();
        get<std::uint32_t>();  // uid
        get<std::uint32_t>();  // gid
        get<std::uint32_t>();  // nlink
        stat.size = get<std::int64_t>();
        get<std::int64_t>();  // atime_sec
        get<std::uint32_t>();  // atime_nsec
        stat.mtimeSec = get<std::int64_t>();
        get<std::uint32_t>();  // mtime_nsec
        get<std::int64_t>();  // btime_sec
        get<std::uint32_t>();  // btime_nsec
        get<std::uint64_t>();  // dev
        stat.ino = get<std::uint64_t>();
        EXPECT(static_cast<std::size_t>(data_ - start) == DirectoryReader::kStatSize);
        return stat;
    }

private:
    const std::uint8_t *data_;
    const std::uint8_t *end_;
};

std::map<std::string, Entry> readAll(const std::string &path, bool withStat) {
    const int fd = DirectoryReader::open(path.c_str());
    EXPECT(fd >= 0);
    DirectoryReader reader(fd);
    ByteWriter batch;
    std::map<std::string, Entry> entries;
    while (true) {
        EXPECT(reader.readBatch(withStat, batch) == 0);
        if (batch.empty()) {
            break;
        }
        Reader in(batch);
        while (!in.done()) {
            Entry entry;
            entry.type = in.get<std::uint8_t>();
            entry.flags = in.get<std::uint8_t>();
            const std::string name = in.getString();
            if (entry.flags & DirectoryReader::kStatOk) {
                entry.hasStat = true;
                entry.stat = in.getStat();
            } else if (withStat) {
                entry.statErrno = in.get<std::int32_t>();
            }
            if (entry.flags & DirectoryReader::kHasLinkTarget) {
                entry.target = in.getString();
                if (entry.flags & DirectoryReader::kTargetStatOk) {
                    entry.targetStat = in.getStat();
                } else {
                    entry.targetErrno = in.get<std::int32_t>();
                }
            }
            EXPECT(entries.emplace(name, entry).second);
        }
    }
    return entries;
}

void testListing() {
    TempDir dir;
    const auto &root = dir.path();
    std::ofstream(root / "file") << "hello";
    std::ofstream(root / ".hidden");
    std::filesystem::create_directory(root / "directory");
    std::filesystem::create_symlink("file", root / "link");
    std::filesystem::create_symlink("missing", root / "dangling");
    // Names are bytes, not necessarily UTF-8.
    std::ofstream(root / "\xff\xfe");

    const auto entries = readAll(root.string(), true);
    EXPECT(entries.size() == 6);
    EXPECT(!entries.contains("."));
    EXPECT(!entries.contains(".."));

    const auto &file = entries.at("file");
    EXPECT(file.hasStat);
    EXPECT(S_ISREG(file.stat.mode));
    EXPECT(file.stat.size == 5);
    struct stat st {};
    EXPECT(lstat((root / "file").c_str(), &st) == 0);
    EXPECT(file.stat.ino == st.st_ino);
    EXPECT(file.stat.mtimeSec == st.st_mtim.tv_sec);
    EXPECT(!(file.flags & DirectoryReader::kHasLinkTarget));

    EXPECT(S_ISDIR(entries.at("directory").stat.mode));
    EXPECT(entries.contains(".hidden"));
    EXPECT(entries.contains("\xff\xfe"));

    const auto &link = entries.at("link");
    EXPECT(S_ISLNK(link.stat.mode));
    EXPECT(link.target == "file");
    EXPECT(link.flags & DirectoryReader::kTargetStatOk);
    EXPECT(S_ISREG(link.targetStat.mode));
    EXPECT(link.targetStat.size == 5);

    const auto &dangling = entries.at("dangling");
    EXPECT(dangling.target == "missing");
    EXPECT(!(dangling.flags & DirectoryReader::kTargetStatOk));
    EXPECT(dangling.targetErrno == ENOENT);
}

void testNamesOnly() {
    TempDir dir;
    std::ofstream(dir.path() / "a");
    std::filesystem::create_symlink("a", dir.path() / "b");
    const auto entries = readAll(dir.path().string(), false);
    EXPECT(entries.size() == 2);
    for (const auto &[name, entry] : entries) {
        EXPECT(entry.flags == 0);
    }
    EXPECT(entries.at("b").type == DT_LNK);
}

void testManyEntriesSpanBatches() {
    TempDir dir;
    constexpr int kCount = 5000;
    for (int i = 0; i < kCount; ++i) {
        std::ofstream(dir.path() / ("entry-with-a-reasonably-long-name-" + std::to_string(i)));
    }
    const auto entries = readAll(dir.path().string(), true);
    EXPECT(entries.size() == kCount);
}

void testOpenErrors() {
    EXPECT(DirectoryReader::open("/nonexistent/filepan") == -ENOENT);
    TempDir dir;
    std::ofstream(dir.path() / "file");
    EXPECT(DirectoryReader::open((dir.path() / "file").c_str()) == -ENOTDIR);
}

}  // namespace

int main() {
    testListing();
    testNamesOnly();
    testManyEntriesSpanBatches();
    testOpenErrors();
    std::puts("directory_reader_test: OK");
    return 0;
}
