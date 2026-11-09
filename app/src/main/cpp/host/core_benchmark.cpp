// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Synthetic benchmarks for the native core. They measure the core alone, not JNI or the Kotlin
// side, so compare runs on the same machine rather than against device numbers.
//
//   build/host/core_benchmark [directory on the file system to test]

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <dirent.h>
#include <sys/stat.h>

#include <archive.h>
#include <archive_entry.h>

#include "filepan/checksum.hpp"
#include "filepan/copy_file.hpp"
#include "filepan/create_archive.hpp"
#include "filepan/delete_tree.hpp"
#include "filepan/directory_reader.hpp"
#include "filepan/extract_archive.hpp"
#include "test_util.hpp"

namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

double millisSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

class NoProgress : public filepan::DeleteTreeCallbacks,
                   public filepan::CopyFileCallbacks,
                   public filepan::CreateArchiveCallbacks,
                   public filepan::ChecksumCallbacks {
public:
    bool onProgress(std::uint64_t, std::string_view) override { return true; }
    bool onError(std::string_view, ErrorKind, int) override { return true; }
    bool onProgress(std::uint64_t) override { return true; }
    bool onProgress(std::uint64_t, std::uint64_t, std::string_view) override { return true; }
    bool onSourceError(std::string_view, int) override { return true; }
};

class NoExtractProgress : public filepan::ExtractCallbacks {
public:
    bool onProgress(std::uint64_t) override { return true; }
    bool onError(const filepan::ExtractItem *, Stage, int, const std::string &) override {
        return true;
    }
};

void createFlatDirectory(const fs::path &path, int count) {
    fs::create_directories(path);
    for (int i = 0; i < count; ++i) {
        std::ofstream(path / ("IMG_" + std::to_string(20260000 + i) + ".jpg")) << i;
    }
}

int listWithBatches(const fs::path &path) {
    filepan::DirectoryReader reader(filepan::DirectoryReader::open(path.c_str()));
    filepan::ByteWriter batch;
    int bytes = 0;
    while (reader.readBatch(true, batch) == 0 && !batch.empty()) {
        bytes += static_cast<int>(batch.size());
    }
    return bytes;
}

// What the old path did per entry, minus the JNI and allocation overhead it added on top.
int listWithReaddirAndLstat(const fs::path &path) {
    DIR *dir = opendir(path.c_str());
    int count = 0;
    while (const dirent *entry = readdir(dir)) {
        struct stat st {};
        lstat((path / entry->d_name).c_str(), &st);
        ++count;
    }
    closedir(dir);
    return count;
}

void benchmarkListing(const fs::path &root) {
    for (const int count : {1'000, 10'000, 50'000}) {
        const auto path = root / ("list-" + std::to_string(count));
        createFlatDirectory(path, count);
        // Warm the dentry cache so that both variants see the same state.
        listWithReaddirAndLstat(path);
        auto start = Clock::now();
        listWithReaddirAndLstat(path);
        const double baseline = millisSince(start);
        start = Clock::now();
        listWithBatches(path);
        const double batched = millisSince(start);
        std::printf("list %6d entries: readdir+lstat %8.2f ms, batched statx %8.2f ms\n", count,
                    baseline, batched);
    }
}

void benchmarkDelete(const fs::path &root) {
    const auto path = root / "delete";
    for (int i = 0; i < 100; ++i) {
        createFlatDirectory(path / std::to_string(i), 200);
    }
    NoProgress callbacks;
    const auto start = Clock::now();
    const auto result = filepan::deleteTree(path.string(), callbacks);
    std::printf("delete %llu entries: %.2f ms\n",
                static_cast<unsigned long long>(result.deletedCount), millisSince(start));
}

void benchmarkCopy(const fs::path &root) {
    const auto source = root / "copy-source";
    constexpr std::size_t kSize = 256 * 1024 * 1024;
    {
        std::ofstream out(source, std::ios::binary);
        std::string chunk(1024 * 1024, 'x');
        for (std::size_t i = 0; i < kSize / chunk.size(); ++i) {
            out << chunk;
        }
    }
    NoProgress callbacks;
    const auto start = Clock::now();
    filepan::copyFile(source.c_str(), (root / "copy-target").c_str(), {}, callbacks);
    const double millis = millisSince(start);
    std::printf("copy 256 MiB: %.2f ms (%.0f MiB/s)\n", millis, 256 / (millis / 1000));
}

// Extracting every entry of a solid tar.gz: the old path reopened the archive and scanned from the
// start for each entry; FilePan reads it once.
void benchmarkExtract(const fs::path &root) {
    const auto source = root / "extract-source";
    constexpr int kCount = 500;
    createFlatDirectory(source, kCount);
    const auto archivePath = root / "extract.tar.gz";
    NoProgress callbacks;
    filepan::createArchive(archivePath.string(), {ARCHIVE_FORMAT_TAR, ARCHIVE_FILTER_GZIP, "", 0},
                           {{source.string(), "s"}}, callbacks);

    auto start = Clock::now();
    for (int target = 1; target <= kCount; ++target) {
        archive *a = archive_read_new();
        archive_read_support_filter_all(a);
        archive_read_support_format_all(a);
        archive_read_open_filename(a, archivePath.c_str(), 65536);
        archive_entry *entry;
        for (int index = 0; archive_read_next_header(a, &entry) == ARCHIVE_OK; ++index) {
            if (index == target) {
                char buffer[4096];
                while (archive_read_data(a, buffer, sizeof(buffer)) > 0) {
                }
                break;
            }
        }
        archive_read_free(a);
    }
    const double perEntry = millisSince(start);

    const auto target = root / "extract-target";
    fs::create_directory(target);
    std::vector<filepan::ExtractItem> items;
    for (int index = 1; index <= kCount; ++index) {
        items.push_back({static_cast<std::uint64_t>(index), filepan::ExtractItem::Type::kFile, -1,
                         (target / std::to_string(index)).string()});
    }
    NoExtractProgress extractCallbacks;
    start = Clock::now();
    filepan::extractArchive(archivePath.string(), {}, items, extractCallbacks);
    std::printf("extract %d entries from tar.gz: reopen per entry %8.2f ms, single pass %6.2f ms\n",
                kCount, perEntry, millisSince(start));
}

void benchmarkChecksums(const fs::path &root) {
    const auto path = root / "copy-source";
    NoProgress callbacks;
    using filepan::ChecksumAlgorithm;
    const std::vector<ChecksumAlgorithm> all = {ChecksumAlgorithm::kCrc32, ChecksumAlgorithm::kMd5,
                                                ChecksumAlgorithm::kSha1, ChecksumAlgorithm::kSha256,
                                                ChecksumAlgorithm::kSha512};
    double sequential = 0;
    for (const auto algorithm : all) {
        const auto start = Clock::now();
        filepan::computeChecksums(path.string(), {algorithm}, callbacks);
        sequential += millisSince(start);
    }
    const auto start = Clock::now();
    filepan::computeChecksums(path.string(), all, callbacks);
    std::printf("checksums 256 MiB, 5 algorithms: one at a time %.0f ms, in parallel %.0f ms\n",
                sequential, millisSince(start));
}

}  // namespace

int main(int argc, char **argv) {
    filepan::test::TempDir temp;
    const fs::path root = argc > 1 ? fs::path(argv[1]) / "filepan-benchmark" : temp.path();
    fs::create_directories(root);
    benchmarkListing(root);
    benchmarkDelete(root);
    benchmarkCopy(root);
    benchmarkChecksums(root);
    benchmarkExtract(root);
    if (argc > 1) {
        fs::remove_all(root);
    }
    return 0;
}
