// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>

#include <archive.h>
#include <archive_entry.h>
#include <sys/stat.h>

#include "filepan/create_archive.hpp"
#include "test_util.hpp"

using filepan::CreateArchiveCallbacks;
using filepan::CreateArchiveOptions;
using filepan::test::TempDir;
namespace fs = std::filesystem;

namespace {

class Recorder : public CreateArchiveCallbacks {
public:
    std::uint64_t size = 0;
    std::uint64_t entries = 0;
    std::vector<std::string> errors;
    bool abort = false;

    bool onProgress(std::uint64_t readSize, std::uint64_t entryCount, std::string_view) override {
        size += readSize;
        entries += entryCount;
        return !abort;
    }

    bool onSourceError(std::string_view path, int) override {
        errors.emplace_back(path);
        return true;
    }
};

struct ReadEntry {
    mode_t type;
    std::string content;
    std::string linkTarget;
};

std::map<std::string, ReadEntry> readBack(const fs::path &path, const char *password = nullptr) {
    archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (password) {
        archive_read_add_passphrase(a, password);
    }
    EXPECT(archive_read_open_filename(a, path.c_str(), 65536) == ARCHIVE_OK);
    std::map<std::string, ReadEntry> entries;
    archive_entry *entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        std::string name = archive_entry_pathname(entry);
        if (!name.empty() && name.back() == '/') {
            name.pop_back();
        }
        ReadEntry read{archive_entry_filetype(entry), "", ""};
        if (const char *target = archive_entry_symlink(entry)) {
            read.linkTarget = target;
        }
        char buffer[65536];
        la_ssize_t length;
        while ((length = archive_read_data(a, buffer, sizeof(buffer))) > 0) {
            read.content.append(buffer, static_cast<std::size_t>(length));
        }
        EXPECT(length == 0);
        entries.emplace(name, read);
    }
    archive_read_free(a);
    return entries;
}

std::string randomBytes(std::size_t size) {
    std::mt19937 random(7);
    std::string bytes(size, '\0');
    for (auto &byte : bytes) {
        byte = static_cast<char>(random() % 11);
    }
    return bytes;
}

const std::string kBig = randomBytes(5 * 1024 * 1024 + 3);

fs::path makeTree(const fs::path &root) {
    const auto source = root / "photos";
    fs::create_directories(source / "2026/june");
    std::ofstream(source / "2026/june/a.jpg", std::ios::binary) << kBig;
    std::ofstream(source / "2026/notes.txt") << "notes";
    std::ofstream(source / "empty");
    fs::create_symlink("2026/notes.txt", source / "link");
    return source;
}

void testFormat(int format, int filter, const char *password = nullptr) {
    TempDir dir;
    const auto source = makeTree(dir.path());
    const auto target = dir.path() / "out.archive";
    CreateArchiveOptions options{format, filter, password ? password : "", 4};
    Recorder recorder;
    const auto result = filepan::createArchive(target.string(), options,
                                               {{source.string(), "photos"}}, recorder);
    if (result.error) {
        std::fprintf(stderr, "format %d filter %d: %d %s\n", format, filter, result.error,
                     result.message.c_str());
    }
    EXPECT(result.error == 0);
    EXPECT(recorder.errors.empty());
    EXPECT(recorder.size == kBig.size() + 5);
    // photos, 2026, june, a.jpg, notes.txt, empty, link
    EXPECT(recorder.entries == 7);

    const auto entries = readBack(target, password);
    EXPECT(entries.size() == 7);
    EXPECT(entries.at("photos").type == AE_IFDIR);
    EXPECT(entries.at("photos/2026/june/a.jpg").content == kBig);
    EXPECT(entries.at("photos/2026/notes.txt").content == "notes");
    EXPECT(entries.at("photos/empty").content.empty());
    if (format != ARCHIVE_FORMAT_7ZIP) {
        EXPECT(entries.at("photos/link").type == AE_IFLNK);
        EXPECT(entries.at("photos/link").linkTarget == "2026/notes.txt");
    }
}

void testSkipsUnsupportedSources() {
    TempDir dir;
    const auto source = makeTree(dir.path());
    EXPECT(mkfifo((source / "pipe").c_str(), 0600) == 0);
    Recorder recorder;
    const auto result = filepan::createArchive(
        (dir.path() / "out.tar").string(), {ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_NONE, "", 0},
        {{source.string(), "photos"}}, recorder);
    EXPECT(result.error == 0);
    EXPECT(recorder.errors == std::vector<std::string>{(source / "pipe").string()});
    EXPECT(!readBack(dir.path() / "out.tar").contains("photos/pipe"));
}

void testAbort() {
    TempDir dir;
    const auto source = makeTree(dir.path());
    Recorder recorder;
    recorder.abort = true;
    const auto result = filepan::createArchive(
        (dir.path() / "out.zip").string(), {ARCHIVE_FORMAT_ZIP, ARCHIVE_FILTER_NONE, "", 0},
        {{source.string(), "photos"}}, recorder);
    EXPECT(result.error == ECANCELED);
}

void testRefusesExistingTarget() {
    TempDir dir;
    const auto source = makeTree(dir.path());
    std::ofstream(dir.path() / "exists.zip") << "x";
    Recorder recorder;
    const auto result = filepan::createArchive(
        (dir.path() / "exists.zip").string(), {ARCHIVE_FORMAT_ZIP, ARCHIVE_FILTER_NONE, "", 0},
        {{source.string(), "photos"}}, recorder);
    EXPECT(result.error == EEXIST);
}

// Each level reads back, and a higher one compresses better: the data is a random block written
// twice, further apart than a low level's window reaches.
void testLevels(int format, int filter, int low, int high) {
    TempDir dir;
    const auto source = dir.path() / "text";
    fs::create_directories(source);
    const std::string block = randomBytes(768 * 1024);
    const std::string text = block + block;
    std::ofstream(source / "a.txt") << text;
    std::uintmax_t sizes[2] = {};
    for (int i = 0; i < 2; ++i) {
        const auto target = dir.path() / ("out" + std::to_string(i));
        CreateArchiveOptions options{format, filter, "", 1};
        options.level = i == 0 ? low : high;
        Recorder recorder;
        EXPECT(filepan::createArchive(target.string(), options, {{source.string(), "text"}}, recorder).error == 0);
        EXPECT(readBack(target).at("text/a.txt").content == text);
        sizes[i] = fs::file_size(target);
    }
    EXPECT(sizes[1] < sizes[0]);
}

}  // namespace

int main() {
    testLevels(ARCHIVE_FORMAT_ZIP, ARCHIVE_FILTER_NONE, 0, 9);
    testLevels(ARCHIVE_FORMAT_TAR, ARCHIVE_FILTER_ZSTD, 1, 12);
    testLevels(ARCHIVE_FORMAT_TAR, ARCHIVE_FILTER_XZ, 0, 6);
    testLevels(ARCHIVE_FORMAT_7ZIP, ARCHIVE_FILTER_NONE, 1, 9);
    // The formats FilePan offers, plus tar.zst.
    testFormat(ARCHIVE_FORMAT_ZIP, ARCHIVE_FILTER_NONE);
    testFormat(ARCHIVE_FORMAT_ZIP, ARCHIVE_FILTER_NONE, "hunter2");
    testFormat(ARCHIVE_FORMAT_TAR, ARCHIVE_FILTER_XZ);
    testFormat(ARCHIVE_FORMAT_TAR, ARCHIVE_FILTER_ZSTD);
    testFormat(ARCHIVE_FORMAT_7ZIP, ARCHIVE_FILTER_NONE);
    testSkipsUnsupportedSources();
    testAbort();
    testRefusesExistingTarget();
    std::puts("create_archive_test: OK");
    return 0;
}
