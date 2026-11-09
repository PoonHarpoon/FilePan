// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Listing benchmark for the local backend; not run by ctest. It measures the core alone, so
// compare runs on the same machine rather than against device numbers.
//
//   build/host/vfs/vfs_benchmark [directory on the file system to test] [entry count]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "filepan/directory_reader.hpp"
#include "filepan/vfs/local_backend.hpp"
#include "filepan/vfs/sort.hpp"
#include "test_util.hpp"

namespace fs = std::filesystem;
using namespace filepan::vfs;

namespace {

using Clock = std::chrono::steady_clock;

double millisSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// Best of a few runs, after a warm-up, so all variants see a warm dentry and inode cache.
template <typename F>
double bestOf(F &&run) {
    run();
    double best = 1e300;
    for (int i = 0; i < 5; ++i) {
        const auto start = Clock::now();
        run();
        best = std::min(best, millisSince(start));
    }
    return best;
}

void createDirectory(const fs::path &path, int count) {
    fs::create_directories(path);
    for (int i = 0; i < count; ++i) {
        // Camera-style names; every 50th is a directory and every 100th a symlink.
        const auto name = path / ("IMG_" + std::to_string(20260000 + i) + ".jpg");
        if (i % 100 == 99) {
            fs::create_symlink("IMG_20260000.jpg", name);
        } else if (i % 50 == 49) {
            fs::create_directory(name);
        } else {
            const int fd = open(name.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
            if (fd != -1) {
                (void)!write(fd, "x", 1);
                close(fd);
            }
        }
    }
}

std::size_t readdirAndLstat(const fs::path &path) {
    DIR *dir = opendir(path.c_str());
    std::size_t count = 0;
    std::string child = path.string() + "/";
    const auto length = child.size();
    while (const dirent *entry = readdir(dir)) {
        child.resize(length);
        child.append(entry->d_name);
        struct stat st {};
        lstat(child.c_str(), &st);
        ++count;
    }
    closedir(dir);
    return count;
}

std::size_t byteBatches(const fs::path &path) {
    filepan::DirectoryReader reader(filepan::DirectoryReader::open(path.c_str()));
    filepan::ByteWriter batch;
    std::size_t bytes = 0;
    while (reader.readBatch(true, batch) == 0 && !batch.empty()) {
        bytes += batch.size();
    }
    return bytes;
}

std::size_t stdFilesystem(const fs::path &path) {
    std::size_t count = 0;
    for (const auto &entry : fs::directory_iterator(path)) {
        count += entry.symlink_status().type() == fs::file_type::directory;
        std::error_code error;
        count += entry.file_size(error) > 0 ? 1 : 0;
    }
    return count;
}

std::vector<Entry> backendList(LocalBackend &backend, const fs::path &path, bool withStat,
                               unsigned threads = 4) {
    std::vector<Entry> entries;
    const auto result = backend.list(
        Location::local(path.string()), {.withStat = withStat, .statThreads = threads},
        [&](std::vector<Entry> batch) {
            if (entries.empty()) {
                entries = std::move(batch);
            } else {
                std::ranges::move(batch, std::back_inserter(entries));
            }
        },
        filepan::CancellationToken());
    EXPECT(result);
    return entries;
}

// Keeps the batches as delivered, which is what a consumer does, and counts the entries.
std::size_t backendCount(LocalBackend &backend, const fs::path &path, bool withStat,
                         unsigned threads) {
    std::vector<std::vector<Entry>> batches;
    std::size_t count = 0;
    const auto result = backend.list(
        Location::local(path.string()), {.withStat = withStat, .statThreads = threads},
        [&](std::vector<Entry> batch) {
            count += batch.size();
            batches.push_back(std::move(batch));
        },
        filepan::CancellationToken());
    EXPECT(result);
    return count;
}

}  // namespace

int main(int argc, char **argv) {
    filepan::test::TempDir temp;
    const fs::path root = argc > 1 ? fs::path(argv[1]) / "filepan-vfs-benchmark" : temp.path();
    const int count = argc > 2 ? std::atoi(argv[2]) : 100'000;
    const auto path = root / "list";
    std::printf("creating %d entries in %s\n", count, path.c_str());
    createDirectory(path, count);

    LocalBackend backend;
    std::size_t sink = 0;
    const double readdir = bestOf([&] { sink += readdirAndLstat(path); });
    const double bytes = bestOf([&] { sink += byteBatches(path); });
    const double filesystem = bestOf([&] { sink += stdFilesystem(path); });
    const double names = bestOf([&] { sink += backendCount(backend, path, false, 1); });
    const double serial = bestOf([&] { sink += backendCount(backend, path, true, 1); });
    const double entries = bestOf([&] { sink += backendCount(backend, path, true, 4); });
    const double entries8 = bestOf([&] { sink += backendCount(backend, path, true, 8); });

    auto listed = backendList(backend, path, true);
    EXPECT(listed.size() == static_cast<std::size_t>(count));
    const double mime = bestOf([&] {
        for (auto &entry : listed) {
            entry.setMimeType({});
            sink += entry.mimeType().type.size();
        }
    });
    // In directory order, which is hash order on most file systems.
    const std::vector<Entry> unsorted = listed;
    const double sortName = bestOf([&] {
        auto copy = unsorted;
        sortEntries(copy, {.key = SortKey::kName});
        sink += copy.front().name.size();
    });
    const double sortType = bestOf([&] {
        auto copy = unsorted;
        sortEntries(copy, {.key = SortKey::kType});
        sink += copy.front().name.size();
    });

    std::printf("list %d entries (best of 5, warm cache):\n", count);
    std::printf("  readdir + lstat (per-entry lstat)  %8.2f ms\n", readdir);
    std::printf("  std::filesystem iterator + stat    %8.2f ms\n", filesystem);
    std::printf("  DirectoryReader byte batches       %8.2f ms\n", bytes);
    std::printf("  LocalBackend::list, names only     %8.2f ms\n", names);
    std::printf("  LocalBackend::list, 1 stat thread  %8.2f ms  (%.0f ns/entry)\n", serial,
                serial * 1e6 / count);
    std::printf("  LocalBackend::list, 4 stat threads %8.2f ms  (%.0f ns/entry)\n", entries,
                entries * 1e6 / count);
    std::printf("  LocalBackend::list, 8 stat threads %8.2f ms\n", entries8);
    std::printf("  MIME lookup for all entries        %8.2f ms\n", mime);
    std::printf("  natural sort by name               %8.2f ms\n", sortName);
    std::printf("  sort by type                       %8.2f ms\n", sortType);
    if (argc > 1) {
        fs::remove_all(root);
    }
    return sink == 0;
}
