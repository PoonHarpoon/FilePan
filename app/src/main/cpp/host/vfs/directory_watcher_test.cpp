// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <sys/inotify.h>

#include "filepan/vfs/directory_watcher.hpp"
#include "filepan/vfs/local_backend.hpp"
#include "test_util.hpp"

namespace fs = std::filesystem;
using namespace filepan::vfs;
using namespace std::chrono_literals;
using filepan::test::TempDir;

namespace {

void touch(const fs::path &path) { std::ofstream(path) << "x"; }

// Collects changes from the watcher thread.
class Collector {
public:
    WatchCallback callback() {
        return [this](const DirectoryChange &change) {
            if (block) {
                block(change);
            }
            std::lock_guard lock(mutex_);
            changes_.push_back(change);
            condition_.notify_all();
        };
    }

    // Waits until predicate holds for the changes so far.
    bool waitFor(const std::function<bool(const std::vector<DirectoryChange> &)> &predicate,
                 std::chrono::milliseconds timeout = 3s) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [&] { return predicate(changes_); });
    }

    std::vector<DirectoryChange> changes() {
        std::lock_guard lock(mutex_);
        return changes_;
    }

    std::set<std::string> names() {
        std::set<std::string> result;
        for (const auto &change : changes()) {
            result.insert(change.names.begin(), change.names.end());
        }
        return result;
    }

    std::function<void(const DirectoryChange &)> block;

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<DirectoryChange> changes_;
};

bool hasNames(const std::vector<DirectoryChange> &changes, const std::set<std::string> &wanted) {
    std::set<std::string> seen;
    for (const auto &change : changes) {
        seen.insert(change.names.begin(), change.names.end());
    }
    return std::ranges::includes(seen, wanted);
}

void testCreateDeleteRename() {
    TempDir temp;
    touch(temp.path() / "to-delete");
    touch(temp.path() / "to-rename");
    auto watcher = DirectoryWatcher::create(50ms);
    EXPECT(watcher);
    Collector collector;
    const auto id = (*watcher)->watch(temp.path().string(), collector.callback());
    EXPECT(id);

    touch(temp.path() / "created");
    fs::remove(temp.path() / "to-delete");
    fs::rename(temp.path() / "to-rename", temp.path() / "renamed");
    EXPECT(collector.waitFor([](const auto &changes) {
        return hasNames(changes, {"created", "to-delete", "to-rename", "renamed"});
    }));
    for (const auto &change : collector.changes()) {
        EXPECT(change.path == temp.path().string());
        EXPECT(!change.rescan && !change.directoryGone);
        EXPECT(std::ranges::is_sorted(change.names));
        EXPECT(std::ranges::adjacent_find(change.names) == change.names.end());
    }
    (*watcher)->unwatch(*id);
}

// A burst within the debounce window arrives as few changes, not one per event.
void testCoalescing() {
    TempDir temp;
    auto watcher = DirectoryWatcher::create(200ms);
    Collector collector;
    const auto id = (*watcher)->watch(temp.path().string(), collector.callback());
    std::set<std::string> created;
    for (int i = 0; i < 200; ++i) {
        const auto name = "f" + std::to_string(i);
        touch(temp.path() / name);
        created.insert(name);
    }
    EXPECT(collector.waitFor([&](const auto &changes) { return hasNames(changes, created); }));
    std::this_thread::sleep_for(300ms);
    // Each name appears once per change though it got create, modify and close events.
    EXPECT(collector.changes().size() <= 3);
    (*watcher)->unwatch(*id);
}

void testSeparateDirectoriesAndUnwatch() {
    TempDir temp;
    fs::create_directory(temp.path() / "a");
    fs::create_directory(temp.path() / "b");
    auto watcher = DirectoryWatcher::create(20ms);
    Collector a;
    Collector b;
    Collector aAgain;
    const auto idA = (*watcher)->watch((temp.path() / "a").string(), a.callback());
    const auto idB = (*watcher)->watch((temp.path() / "b").string(), b.callback());
    const auto idAgain = (*watcher)->watch((temp.path() / "a").string(), aAgain.callback());
    touch(temp.path() / "a" / "x");
    EXPECT(a.waitFor([](const auto &changes) { return hasNames(changes, {"x"}); }));
    EXPECT(aAgain.waitFor([](const auto &changes) { return hasNames(changes, {"x"}); }));
    std::this_thread::sleep_for(100ms);
    EXPECT(b.changes().empty());

    // The shared inotify watch survives until both watches of a are gone.
    (*watcher)->unwatch(*idA);
    touch(temp.path() / "a" / "y");
    EXPECT(aAgain.waitFor([](const auto &changes) { return hasNames(changes, {"y"}); }));
    EXPECT(!a.names().contains("y"));
    (*watcher)->unwatch(*idAgain);
    (*watcher)->unwatch(*idB);
    touch(temp.path() / "b" / "z");
    std::this_thread::sleep_for(100ms);
    EXPECT(b.changes().empty());
    // Unknown and repeated ids are ignored.
    (*watcher)->unwatch(*idB);
    (*watcher)->unwatch(12345);
}

void testDirectoryGone() {
    TempDir temp;
    fs::create_directory(temp.path() / "dir");
    auto watcher = DirectoryWatcher::create(20ms);
    Collector collector;
    const auto id = (*watcher)->watch((temp.path() / "dir").string(), collector.callback());
    fs::remove(temp.path() / "dir");
    EXPECT(collector.waitFor([](const auto &changes) {
        return !changes.empty() && changes.back().directoryGone;
    }));
    (*watcher)->unwatch(*id);
    EXPECT(!(*watcher)->watch((temp.path() / "missing").string(), collector.callback()));
}

// Blocks the watcher thread in a callback while far more events than the kernel queues pile up,
// so the next change must ask for a rescan.
void testOverflowRequestsRescan() {
    int maxQueued = 0;
    if (std::FILE *file = std::fopen("/proc/sys/fs/inotify/max_queued_events", "r")) {
        if (std::fscanf(file, "%d", &maxQueued) != 1) maxQueued = 0;
        std::fclose(file);
    }
    if (maxQueued <= 0 || maxQueued > 100'000) {
        std::printf("skipping the overflow test: max_queued_events is %d\n", maxQueued);
        return;
    }
    TempDir temp;
    auto watcher = DirectoryWatcher::create(10ms);
    Collector collector;
    std::mutex gate;
    std::unique_lock closed(gate);
    bool first = true;
    collector.block = [&](const DirectoryChange &) {
        if (first) {
            first = false;
            std::lock_guard wait(gate);
        }
    };
    const auto id = (*watcher)->watch(temp.path().string(), collector.callback());
    touch(temp.path() / "first");
    std::this_thread::sleep_for(100ms);  // The callback is now blocked.
    // Each create + unlink queues at least three events.
    for (int i = 0; i < maxQueued / 2; ++i) {
        const auto path = temp.path() / ("n" + std::to_string(i));
        touch(path);
        fs::remove(path);
    }
    closed.unlock();
    EXPECT(collector.waitFor([](const auto &changes) {
        return std::ranges::any_of(changes, &DirectoryChange::rescan);
    }));
    (*watcher)->unwatch(*id);
}

// The same, through the parsing path but without needing a small kernel queue.
void testInjectedOverflow() {
    TempDir temp;
    fs::create_directory(temp.path() / "a");
    fs::create_directory(temp.path() / "b");
    auto watcher = DirectoryWatcher::create(10ms);
    Collector a;
    Collector b;
    const auto idA = (*watcher)->watch((temp.path() / "a").string(), a.callback());
    const auto idB = (*watcher)->watch((temp.path() / "b").string(), b.callback());
    (*watcher)->injectEventForTesting(IN_Q_OVERFLOW);
    const auto rescanned = [](const auto &changes) {
        return changes.size() == 1 && changes[0].rescan && changes[0].names.empty();
    };
    EXPECT(a.waitFor(rescanned));
    EXPECT(b.waitFor(rescanned));
    (*watcher)->unwatch(*idA);
    (*watcher)->unwatch(*idB);
}

void testTooManyNamesRequestsRescan() {
    TempDir temp;
    auto watcher = DirectoryWatcher::create(1s);
    Collector collector;
    const auto id = (*watcher)->watch(temp.path().string(), collector.callback());
    for (std::size_t i = 0; i <= DirectoryWatcher::kMaxNames; ++i) {
        touch(temp.path() / std::to_string(i));
    }
    EXPECT(collector.waitFor([](const auto &changes) {
        return !changes.empty() && changes[0].rescan && changes[0].names.empty();
    }));
    (*watcher)->unwatch(*id);
}

void testBackendWatch() {
    TempDir temp;
    LocalBackend backend;
    Collector collector;
    auto handle = backend.watch(Location::local(temp.path().string()), collector.callback());
    EXPECT(handle);
    touch(temp.path() / "through-backend");
    EXPECT(collector.waitFor(
        [](const auto &changes) { return hasNames(changes, {"through-backend"}); }));
    handle->reset();
    touch(temp.path() / "after");
    std::this_thread::sleep_for(250ms);
    EXPECT(!collector.names().contains("after"));
}

}  // namespace

int main() {
    testCreateDeleteRename();
    testCoalescing();
    testSeparateDirectoriesAndUnwatch();
    testDirectoryGone();
    testOverflowRequestsRescan();
    testInjectedOverflow();
    testTooManyNamesRequestsRescan();
    testBackendWatch();
    return 0;
}
