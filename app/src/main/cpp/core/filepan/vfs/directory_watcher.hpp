// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "filepan/error.hpp"
#include "filepan/vfs/watch.hpp"

namespace filepan::vfs {

// Watches directories with inotify on one thread for all watches. Events for a directory are
// coalesced from its first event until the debounce interval has passed, then delivered as one
// DirectoryChange, so a burst (an extraction, a camera writing a file) costs one callback.
class DirectoryWatcher {
public:
    using WatchId = std::uint64_t;

    // Names per change beyond which a change becomes a rescan.
    static constexpr std::size_t kMaxNames = 1024;

    static Result<std::unique_ptr<DirectoryWatcher>> create(
        std::chrono::milliseconds debounce = std::chrono::milliseconds(100));

    ~DirectoryWatcher();
    DirectoryWatcher(const DirectoryWatcher &) = delete;
    DirectoryWatcher &operator=(const DirectoryWatcher &) = delete;

    // Watches the directory's direct children. Watching the same directory twice is fine; each
    // watch gets its own callbacks.
    Result<WatchId> watch(const std::string &path, WatchCallback callback);

    // Once this returns, the watch's callback is not running and won't be called again, unless
    // this is called from a callback.
    void unwatch(WatchId id);

    // Owns the watch through a shared watcher; for Backend::watch().
    static std::unique_ptr<WatchHandle> makeHandle(std::shared_ptr<DirectoryWatcher> watcher,
                                                   WatchId id);

    // Handles an inotify_event as if read from the kernel, which sends IN_Q_OVERFLOW with wd -1.
    // For tests of what the kernel rarely does on demand.
    void injectEventForTesting(std::uint32_t mask, int wd = -1, const std::string &name = {});

private:
    struct Watch {
        int wd;
        std::string path;
        std::shared_ptr<const WatchCallback> callback;
    };

    // One inotify watch descriptor, which inotify shares between watches of the same inode.
    struct Directory {
        std::vector<WatchId> watchIds;
        std::vector<std::string> names;
        bool rescan = false;
        bool gone = false;
        bool pending = false;
        std::chrono::steady_clock::time_point deadline;
    };

    DirectoryWatcher(int inotifyFd, int eventFd, int epollFd, std::chrono::milliseconds debounce);

    void run();
    void readEvents();
    // Handles raw inotify_event records; called with mutex_ held.
    void processEvents(const char *buffer, std::size_t length);
    void markPending(Directory &directory, std::chrono::steady_clock::time_point now);
    // Delivers due changes. Returns the time until the next deadline, or -1 for none.
    int dispatchDue();

    int inotifyFd_;
    int eventFd_;
    int epollFd_;
    std::chrono::milliseconds debounce_;

    std::mutex mutex_;
    bool stopping_ = false;
    WatchId nextId_ = 1;
    std::unordered_map<WatchId, Watch> watches_;
    std::unordered_map<int, Directory> directories_;

    // Held by the watcher thread while it runs callbacks, so unwatch() can wait them out.
    std::mutex dispatchMutex_;
    std::thread thread_;
};

}  // namespace filepan::vfs
