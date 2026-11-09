// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <optional>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include <archive.h>
#include <archive_entry.h>
#include <unistd.h>
#include <zlib.h>

#include "filepan/verify_archive.hpp"
#include "test_util.hpp"

using filepan::ArchiveSource;
using filepan::VerifyArchiveCallbacks;
using filepan::VerifyArchiveResult;

namespace {

struct Source {
    std::string name;
    std::string content;
    bool isDirectory = false;
};

std::string randomBytes(std::size_t size, unsigned seed) {
    std::mt19937 random(seed);
    std::string bytes(size, '\0');
    for (auto &byte : bytes) {
        byte = static_cast<char>(random() % 7);  // Compressible, but not trivially.
    }
    return bytes;
}

const std::vector<Source> &sources() {
    static const std::vector<Source> sources = {
        {"dir", "", true},
        {"dir/a.txt", "alpha alpha alpha"},
        // Larger than a read block.
        {"dir/big.bin", randomBytes(3 << 19, 1)},
        {"b.txt", "bravo"},
    };
    return sources;
}

// An archive in memory; format, filter and options are libarchive names.
std::string makeArchive(const std::string &format, const std::string &filter,
                        const char *options = nullptr, const char *password = nullptr,
                        const std::vector<Source> &entries = sources()) {
    std::size_t capacity = 1 << 20;
    for (const auto &source : entries) {
        capacity += source.content.size() + 1024;
    }
    std::string out(capacity, '\0');
    size_t used = 0;
    archive *a = archive_write_new();
    EXPECT(archive_write_set_format_by_name(a, format.c_str()) == ARCHIVE_OK);
    if (!filter.empty()) {
        EXPECT(archive_write_add_filter_by_name(a, filter.c_str()) == ARCHIVE_OK);
    }
    if (options) {
        EXPECT(archive_write_set_options(a, options) == ARCHIVE_OK);
    }
    if (password) {
        EXPECT(archive_write_set_passphrase(a, password) == ARCHIVE_OK);
    }
    // Tar's blocking would pad the compressed stream; keep it byte-exact for the tests that cut it.
    archive_write_set_bytes_in_last_block(a, 1);
    EXPECT(archive_write_open_memory(a, out.data(), out.size(), &used) == ARCHIVE_OK);
    for (const auto &source : entries) {
        archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname(entry, source.name.c_str());
        archive_entry_set_mtime(entry, 1'700'000'000, 0);
        if (source.isDirectory) {
            archive_entry_set_filetype(entry, AE_IFDIR);
            archive_entry_set_perm(entry, 0755);
            archive_entry_set_size(entry, 0);
        } else {
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0644);
            archive_entry_set_size(entry, static_cast<la_int64_t>(source.content.size()));
        }
        const int written = archive_write_header(a, entry);
        if (written != ARCHIVE_OK) {
            std::fprintf(stderr, "%s: %s\n", format.c_str(), archive_error_string(a));
        }
        EXPECT(written == ARCHIVE_OK);
        if (!source.content.empty()) {
            EXPECT(archive_write_data(a, source.content.data(), source.content.size())
                   == static_cast<la_ssize_t>(source.content.size()));
        }
        archive_entry_free(entry);
    }
    EXPECT(archive_write_close(a) == ARCHIVE_OK);
    archive_write_free(a);
    out.resize(used);
    return out;
}

// One gzip member, as gzip writes it.
std::string gzip(const std::string &data) {
    z_stream stream{};
    EXPECT(deflateInit2(&stream, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    std::string out(deflateBound(&stream, data.size()) + 64, '\0');
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.data()));
    stream.avail_in = static_cast<uInt>(data.size());
    stream.next_out = reinterpret_cast<Bytef *>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());
    EXPECT(deflate(&stream, Z_FINISH) == Z_STREAM_END);
    out.resize(stream.total_out);
    deflateEnd(&stream);
    return out;
}

class MemorySource : public ArchiveSource {
public:
    explicit MemorySource(std::string data, std::optional<std::size_t> failAt = std::nullopt)
        : data_(std::move(data)), failAt_(failAt) {}

    std::int64_t read(void *buffer, std::size_t size) override {
        if (failAt_ && position_ >= *failAt_) {
            return -1;
        }
        const std::size_t count = std::min(size, data_.size() - std::min(position_, data_.size()));
        std::memcpy(buffer, data_.data() + position_, count);
        position_ += count;
        return static_cast<std::int64_t>(count);
    }

    std::int64_t seek(std::int64_t offset, int whence) override {
        const std::int64_t base = whence == SEEK_SET   ? 0
                                  : whence == SEEK_CUR ? static_cast<std::int64_t>(position_)
                                                       : static_cast<std::int64_t>(data_.size());
        if (base + offset < 0) {
            return -1;
        }
        position_ = static_cast<std::size_t>(base + offset);
        return static_cast<std::int64_t>(position_);
    }

private:
    std::string data_;
    std::optional<std::size_t> failAt_;
    std::size_t position_ = 0;
};

class Recorder : public VerifyArchiveCallbacks {
public:
    bool onProgress(std::uint64_t readSize, std::string_view) override {
        read += readSize;
        ++calls;
        return calls <= abortAfter;
    }
    std::uint64_t read = 0;
    int calls = 0;
    int abortAfter = 1 << 30;
};

VerifyArchiveResult verify(const std::string &data, bool seekable,
                           const std::vector<std::string> &passwords = {},
                           Recorder *recorder = nullptr) {
    MemorySource source(data);
    Recorder own;
    return filepan::verifyArchive(source, seekable, passwords, recorder ? *recorder : own);
}

void expectIntact(const std::string &data, std::uint64_t entries,
                  const std::vector<std::string> &passwords = {}) {
    for (const bool seekable : {true, false}) {
        Recorder recorder;
        const auto result = verify(data, seekable, passwords, &recorder);
        if (result.error || !result.problems.empty()) {
            std::fprintf(stderr, "seekable %d: error %d %s; %zu problems: %s\n", seekable,
                         result.error, result.message.c_str(), result.problems.size(),
                         result.problems.empty() ? "" : result.problems[0].message.c_str());
        }
        EXPECT(result.error == 0 && result.problems.empty());
        EXPECT(result.entryCount == entries);
        // Progress counts each byte of the archive once, though zip and 7z read some twice.
        EXPECT(recorder.read == data.size());
    }
}

void testIntact() {
    for (const auto &[format, filter, options] : {
             std::tuple{"zip", "", static_cast<const char *>(nullptr)},
             std::tuple{"zip", "", "zip:compression=store"},
             std::tuple{"pax", "", static_cast<const char *>(nullptr)},
             std::tuple{"pax", "gzip", static_cast<const char *>(nullptr)},
             std::tuple{"pax", "bzip2", static_cast<const char *>(nullptr)},
             std::tuple{"pax", "xz", static_cast<const char *>(nullptr)},
             std::tuple{"pax", "zstd", "zstd:compression-level=3"},
             std::tuple{"pax", "lz4", static_cast<const char *>(nullptr)},
             std::tuple{"cpio", "gzip", static_cast<const char *>(nullptr)},
         }) {
        expectIntact(makeArchive(format, filter, options), sources().size());
    }
    // 7z reads its header from the end, so it needs a source that seeks.
    const std::string sevenZip = makeArchive("7zip", "");
    const auto seeking = verify(sevenZip, true);
    EXPECT(seeking.error == 0 && seeking.problems.empty());
    EXPECT(seeking.entryCount == sources().size());
    const auto streamed = verify(sevenZip, false);
    EXPECT(streamed.error == ENOTSUP && streamed.problems.empty());
    // Empty archives are fine too.
    expectIntact(makeArchive("zip", "", nullptr, nullptr, {}), 0);
    expectIntact(makeArchive("pax", "gzip", nullptr, nullptr, {}), 0);
}

std::string flip(std::string data, std::size_t at) {
    data[at] = static_cast<char>(data[at] ^ 0x5a);
    return data;
}

void testDamagedZipEntry() {
    // Stored, so the flipped byte is inside big.bin's data and only its CRC catches it.
    const std::string zip = makeArchive("zip", "", "zip:compression=store");
    const auto at = zip.find(sources()[2].content.substr(1000, 64));
    EXPECT(at != std::string::npos);
    for (const bool seekable : {true, false}) {
        const auto result = verify(flip(zip, at + 10), seekable);
        EXPECT(result.error == 0);
        EXPECT(result.problems.size() == 1);
        EXPECT(result.problems[0].path == "dir/big.bin");
        if (seekable) {
            // The other entries still read, found through the central directory.
            EXPECT(result.entryCount == sources().size());
            EXPECT(result.problems[0].message.find("CRC") != std::string::npos);
        }
        // Streamed, a stored entry ends where its data descriptor matches, which it no longer
        // does: libarchive reads on to the end and can't go further.
    }
}

void testDamagedTarGz() {
    const std::string tgz = makeArchive("pax", "gzip");
    // A wrong CRC-32 in the trailer: the data inflates fine, so only the gzip check finds it.
    const auto badCrc = verify(flip(tgz, tgz.size() - 8), false);
    EXPECT(badCrc.error == 0 && badCrc.problems.size() == 1);
    EXPECT(badCrc.problems[0].path.empty());
    EXPECT(badCrc.problems[0].message.find("gzip") != std::string::npos);
    // A wrong length.
    const auto badLength = verify(flip(tgz, tgz.size() - 2), true);
    EXPECT(badLength.error == 0 && badLength.problems.size() == 1);
    // Cut short, past the tar's end marker: libarchive only notices when drained.
    const auto truncated = verify(tgz.substr(0, tgz.size() - 4), false);
    EXPECT(truncated.error == 0 && !truncated.problems.empty());
    // Damage in the middle of the compressed data.
    const auto middle = verify(flip(tgz, tgz.size() / 2), false);
    EXPECT(middle.error == 0 && !middle.problems.empty());
}

void testTruncatedTarXz() {
    const std::string txz = makeArchive("pax", "xz");
    const auto result = verify(txz.substr(0, txz.size() - 20), true);
    EXPECT(result.error == 0 && !result.problems.empty());
    const auto halved = verify(txz.substr(0, txz.size() / 2), true);
    EXPECT(halved.error == 0 && !halved.problems.empty());
    EXPECT(halved.problems.back().path == "dir/big.bin");
}

void testGzipMembers() {
    // Several members (as bgzip writes), then zero padding, which gzip ignores.
    const std::string tar = makeArchive("pax", "");
    std::string members = gzip(tar.substr(0, 1000)) + gzip(tar.substr(1000, 1 << 20))
                          + gzip(tar.substr(1000 + (1 << 20))) + std::string(512, '\0');
    expectIntact(members, sources().size());
    // A plain compressed file, not an archive: verified as its one stream.
    const std::string text = gzip(randomBytes(200'000, 2));
    expectIntact(text, 0);
    const auto bad = verify(flip(text, text.size() - 6), true);
    EXPECT(bad.error == 0 && bad.problems.size() == 1);
}

void testNotAnArchive() {
    const auto result = verify(randomBytes(10'000, 3) + "x", true);
    EXPECT(result.error == 0 && result.problems.size() == 1);
    EXPECT(result.problems[0].path.empty());
    const auto empty = verify("", false);
    EXPECT(empty.error == 0 && empty.problems.size() == 1);
}

void testPasswords() {
    const std::string zip = makeArchive("zip", "", "zip:encryption=aes256", "hunter2");
    for (const bool seekable : {true, false}) {
        EXPECT(verify(zip, seekable).error == EACCES);
        EXPECT(verify(zip, seekable, {"nope"}).error == EACCES);
    }
    expectIntact(zip, sources().size(), {"hunter2"});
    // A damaged encrypted entry is damage, not a password to ask for again.
    const std::string stored =
        makeArchive("zip", "", "zip:encryption=zipcrypt,zip:compression=store", "hunter2");
    const auto damaged = verify(flip(stored, stored.size() / 2), true, {"hunter2"});
    EXPECT(damaged.error == 0 && damaged.problems.size() == 1);
}

void testSourceFailureAndCancel() {
    const std::string tgz = makeArchive("pax", "gzip");
    MemorySource failing(tgz, tgz.size() / 2);
    Recorder recorder;
    const auto failed = filepan::verifyArchive(failing, false, {}, recorder);
    EXPECT(failed.error == EIO && failed.problems.empty());

    // Progress comes at most every 200 ms: a slow source reaches it before the end, and an abort
    // there stops the reading.
    struct Slow : MemorySource {
        using MemorySource::MemorySource;
        std::int64_t read(void *buffer, std::size_t size) override {
            usleep(60'000);
            ++reads;
            return MemorySource::read(buffer, size);
        }
        int reads = 0;
    } slow(makeArchive("pax", "", nullptr, nullptr, {{"x", randomBytes(8 << 20, 4)}}));
    Recorder cancelling;
    cancelling.abortAfter = 0;
    EXPECT(filepan::verifyArchive(slow, true, {}, cancelling).error == ECANCELED);
    EXPECT(cancelling.calls == 1 && slow.reads < 10);
}

}  // namespace

int main() {
    testIntact();
    testDamagedZipEntry();
    testDamagedTarGz();
    testTruncatedTarXz();
    testGzipMembers();
    testNotAnArchive();
    testPasswords();
    testSourceFailureAndCancel();
    std::puts("verify_archive_test: ok");
    return 0;
}
