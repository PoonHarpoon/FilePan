// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace filepan::vfs {

// What changed in a watched directory during one debounce window.
struct DirectoryChange {
    // The directory as it was passed to watch().
    std::string path;
    // Children that were created, deleted, renamed (both names) or modified; sorted and unique.
    std::vector<std::string> names;
    // Events were lost (the kernel queue overflowed) or too many names changed to list them:
    // list the directory again. names may be empty.
    bool rescan = false;
    // The directory itself was deleted, moved or unmounted. The watch is dead and delivers
    // nothing more.
    bool directoryGone = false;
};

// Called on the watcher's own thread; post to the UI thread from here rather than doing work.
using WatchCallback = std::function<void(const DirectoryChange &change)>;

// Keeps a watch alive; destroying it stops the watch. Once the destructor returns, the callback
// is not running and won't be called again, unless it is destroyed from within the callback.
class WatchHandle {
public:
    virtual ~WatchHandle() = default;
};

}  // namespace filepan::vfs
