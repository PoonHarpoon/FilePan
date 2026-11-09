// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "filepan/adirstat/tree.hpp"
#include "filepan/cancellation.hpp"
#include "filepan/error.hpp"

struct statx;

namespace filepan::adirstat {

struct ScanOptions {
    // Descend into directories on other file systems instead of stopping at them.
    bool crossMounts = false;
    // Directories are read in parallel; the calling thread is one of them.
    int threads = 4;
};

// Live counters for a running scan, safe to read from any thread.
class ScanProgress {
public:
    std::atomic<std::uint64_t> dirs{0};
    std::atomic<std::uint64_t> files{0};
    std::atomic<std::uint64_t> bytes{0};

    [[nodiscard]] std::string current() const {
        std::lock_guard lock(mutex_);
        return current_;
    }
    void setCurrent(std::string_view path) {
        std::lock_guard lock(mutex_);
        current_.assign(path);
    }

private:
    mutable std::mutex mutex_;
    std::string current_;
};

// Whether a directory entry is on another file system than the scan's root, so the scan stops
// there. rootDevice is makedev() of the root's st_dev.
[[nodiscard]] bool isMountBoundary(std::uint64_t rootDevice, const struct statx &st);

// Walks a local directory tree and returns its sizes, finalized (aggregated and sorted). Symlinks
// are not followed, hard-linked files count once, and directories that can't be read keep their
// errno and are counted in Node::errors; only failing to read the root fails the scan. Fails with
// ECANCELED once cancel is set.
Result<std::unique_ptr<Tree>> scan(std::string_view rootPath, const ScanOptions &options,
                                   const CancellationToken &cancel, ScanProgress *progress = nullptr);

}  // namespace filepan::adirstat
