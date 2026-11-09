// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/vfs/directory_watcher.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <utility>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "filepan/eintr.hpp"

namespace filepan::vfs {

namespace {

constexpr std::uint32_t kWatchMask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO
                                     | IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF
                                     | IN_MOVE_SELF | IN_ONLYDIR | IN_EXCL_UNLINK;
constexpr std::uint32_t kGoneMask = IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT;

constexpr std::uint32_t kInotifyTag = 0;
constexpr std::uint32_t kWakeTag = 1;

class Handle : public WatchHandle {
public:
    Handle(std::shared_ptr<DirectoryWatcher> watcher, DirectoryWatcher::WatchId id)
        : watcher_(std::move(watcher)), id_(id) {}
    ~Handle() override { watcher_->unwatch(id_); }

private:
    std::shared_ptr<DirectoryWatcher> watcher_;
    DirectoryWatcher::WatchId id_;
};

}  // namespace

Result<std::unique_ptr<DirectoryWatcher>> DirectoryWatcher::create(
    std::chrono::milliseconds debounce) {
    const int inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotifyFd == -1) {
        return failErrno("inotify_init1");
    }
    const int eventFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    const int epollFd = eventFd == -1 ? -1 : epoll_create1(EPOLL_CLOEXEC);
    if (epollFd == -1) {
        const int error = errno;
        if (eventFd != -1) {
            ::close(eventFd);
        }
        ::close(inotifyFd);
        return fail(error, "eventfd/epoll_create1");
    }
    epoll_event inotifyEvent{.events = EPOLLIN, .data = {.u32 = kInotifyTag}};
    epoll_event wakeEvent{.events = EPOLLIN, .data = {.u32 = kWakeTag}};
    if (epoll_ctl(epollFd, EPOLL_CTL_ADD, inotifyFd, &inotifyEvent) == -1
        || epoll_ctl(epollFd, EPOLL_CTL_ADD, eventFd, &wakeEvent) == -1) {
        const int error = errno;
        ::close(epollFd);
        ::close(eventFd);
        ::close(inotifyFd);
        return fail(error, "epoll_ctl");
    }
    std::unique_ptr<DirectoryWatcher> watcher(
        new DirectoryWatcher(inotifyFd, eventFd, epollFd, debounce));
    watcher->thread_ = std::thread([raw = watcher.get()] { raw->run(); });
    return watcher;
}

DirectoryWatcher::DirectoryWatcher(int inotifyFd, int eventFd, int epollFd,
                                   std::chrono::milliseconds debounce)
    : inotifyFd_(inotifyFd), eventFd_(eventFd), epollFd_(epollFd), debounce_(debounce) {}

DirectoryWatcher::~DirectoryWatcher() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    const std::uint64_t one = 1;
    FILEPAN_RETRY(::write(eventFd_, &one, sizeof(one)));
    thread_.join();
    ::close(epollFd_);
    ::close(eventFd_);
    ::close(inotifyFd_);
}

Result<DirectoryWatcher::WatchId> DirectoryWatcher::watch(const std::string &path,
                                                          WatchCallback callback) {
    const int wd = inotify_add_watch(inotifyFd_, path.c_str(), kWatchMask);
    if (wd == -1) {
        return failErrno(path);
    }
    std::lock_guard lock(mutex_);
    const WatchId id = nextId_++;
    watches_.emplace(id, Watch{wd, path,
                               std::make_shared<const WatchCallback>(std::move(callback))});
    directories_[wd].watchIds.push_back(id);
    return id;
}

void DirectoryWatcher::unwatch(WatchId id) {
    {
        std::lock_guard lock(mutex_);
        const auto watch = watches_.find(id);
        if (watch == watches_.end()) {
            return;
        }
        const int wd = watch->second.wd;
        watches_.erase(watch);
        if (const auto directory = directories_.find(wd); directory != directories_.end()) {
            std::erase(directory->second.watchIds, id);
            if (directory->second.watchIds.empty()) {
                inotify_rm_watch(inotifyFd_, wd);
                directories_.erase(directory);
            }
        }
    }
    if (std::this_thread::get_id() != thread_.get_id()) {
        // Wait out a dispatch that may have picked up this watch's callback.
        std::lock_guard dispatchLock(dispatchMutex_);
    }
}

std::unique_ptr<WatchHandle> DirectoryWatcher::makeHandle(
    std::shared_ptr<DirectoryWatcher> watcher, WatchId id) {
    return std::make_unique<Handle>(std::move(watcher), id);
}

void DirectoryWatcher::run() {
    while (true) {
        const int timeout = dispatchDue();
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                return;
            }
        }
        epoll_event events[2];
        const int count = epoll_wait(epollFd_, events, 2, timeout);
        if (count == -1) {
            continue;  // EINTR
        }
        for (int i = 0; i < count; ++i) {
            if (events[i].data.u32 == kInotifyTag) {
                readEvents();
            } else {
                std::uint64_t value;
                FILEPAN_RETRY(::read(eventFd_, &value, sizeof(value)));
            }
        }
    }
}

void DirectoryWatcher::markPending(Directory &directory, std::chrono::steady_clock::time_point now) {
    if (!directory.pending) {
        directory.pending = true;
        directory.deadline = now + debounce_;
    }
}

void DirectoryWatcher::readEvents() {
    alignas(inotify_event) char buffer[64 * 1024];
    while (true) {
        const ssize_t length = FILEPAN_RETRY(::read(inotifyFd_, buffer, sizeof(buffer)));
        if (length <= 0) {
            return;  // EAGAIN: drained.
        }
        std::lock_guard lock(mutex_);
        processEvents(buffer, static_cast<std::size_t>(length));
    }
}

void DirectoryWatcher::processEvents(const char *buffer, std::size_t length) {
    const auto now = std::chrono::steady_clock::now();
    for (std::size_t offset = 0; offset < length;) {
        const auto &event = *reinterpret_cast<const inotify_event *>(buffer + offset);
        offset += sizeof(inotify_event) + event.len;
        if (event.mask & IN_Q_OVERFLOW) {
            for (auto &[wd, directory] : directories_) {
                directory.rescan = true;
                directory.names.clear();
                markPending(directory, now);
            }
            continue;
        }
        const auto found = directories_.find(event.wd);
        if (found == directories_.end()) {
            continue;  // Already unwatched.
        }
        Directory &directory = found->second;
        if (event.mask & kGoneMask) {
            directory.gone = true;
        } else if (event.len > 0 && !directory.rescan) {
            if (directory.names.size() >= kMaxNames) {
                directory.rescan = true;
                directory.names.clear();
            } else {
                directory.names.emplace_back(event.name);
            }
        }
        markPending(directory, now);
    }
}

void DirectoryWatcher::injectEventForTesting(std::uint32_t mask, int wd,
                                              const std::string &name) {
    // Like the kernel, pad the name with NULs to keep records aligned.
    const std::size_t nameLength =
        name.empty() ? 0
                     : (name.size() + sizeof(inotify_event)) / sizeof(inotify_event)
                           * sizeof(inotify_event);
    std::vector<inotify_event> buffer(1 + nameLength / sizeof(inotify_event));
    buffer[0] = {.wd = wd,
                 .mask = mask,
                 .cookie = 0,
                 .len = static_cast<std::uint32_t>(nameLength)};
    std::memcpy(reinterpret_cast<char *>(buffer.data() + 1), name.data(), name.size());
    {
        std::lock_guard lock(mutex_);
        processEvents(reinterpret_cast<const char *>(buffer.data()),
                      sizeof(inotify_event) + nameLength);
    }
    const std::uint64_t one = 1;
    FILEPAN_RETRY(::write(eventFd_, &one, sizeof(one)));
}

int DirectoryWatcher::dispatchDue() {
    std::lock_guard dispatchLock(dispatchMutex_);
    std::vector<std::pair<std::shared_ptr<const WatchCallback>, DirectoryChange>> due;
    int timeout = -1;
    {
        std::lock_guard lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        for (auto it = directories_.begin(); it != directories_.end();) {
            Directory &directory = it->second;
            if (!directory.pending) {
                ++it;
                continue;
            }
            if (directory.deadline > now) {
                const auto wait = std::chrono::ceil<std::chrono::milliseconds>(directory.deadline
                                                                              - now);
                const int millis = static_cast<int>(std::max<long long>(wait.count(), 1));
                timeout = timeout == -1 ? millis : std::min(timeout, millis);
                ++it;
                continue;
            }
            std::ranges::sort(directory.names);
            const auto [first, last] = std::ranges::unique(directory.names);
            directory.names.erase(first, last);
            for (const WatchId id : directory.watchIds) {
                auto &watch = watches_.at(id);
                due.emplace_back(watch.callback, DirectoryChange{watch.path, directory.names,
                                                                 directory.rescan,
                                                                 directory.gone});
                if (directory.gone) {
                    watch.wd = -1;
                }
            }
            if (directory.gone) {
                // Harmless if the kernel already dropped it (IN_IGNORED); needed after a move.
                inotify_rm_watch(inotifyFd_, it->first);
                it = directories_.erase(it);
                continue;
            }
            directory.names.clear();
            directory.rescan = false;
            directory.pending = false;
            ++it;
        }
    }
    for (const auto &[callback, change] : due) {
        (*callback)(change);
    }
    return timeout;
}

}  // namespace filepan::vfs
