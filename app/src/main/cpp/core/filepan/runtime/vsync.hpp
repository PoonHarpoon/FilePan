// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

// The UI thread's wait-and-vsync primitive, so AppHost's loop is the same on Android and a host.
//
// The UI thread alternates between running UiLoop's tasks and wait(). wait() returns when woken
// (from any thread), when the next timer is due, or after delivering a requested vsync to the
// callback. On Android it's ALooper_pollOnce on the thread's looper, with an eventfd for wakeups
// and AChoreographer_postVsyncCallback for vsync. On a host it's a condition variable, and vsync
// ticks on a fixed interval.
namespace filepan::runtime {

class VsyncWaiter {
public:
    using VsyncCallback = std::function<void(std::int64_t frameTimeNs)>;

    // Call on the thread that will wait (it becomes the looper thread on Android).
    // intervalNs is the host's frame interval; Android ignores it.
    static std::unique_ptr<VsyncWaiter> create(VsyncCallback callback, std::int64_t intervalNs,
                                               std::function<std::int64_t()> clock);
    virtual ~VsyncWaiter() = default;

    // UI thread: one vsync callback, at the next vsync. Repeated calls before it fires coalesce.
    virtual void requestVsync() = 0;
    // UI thread: blocks until woken, deadlineNs (on the clock) passes, or a vsync was delivered.
    virtual void wait(std::optional<std::int64_t> deadlineNs) = 0;
    // Any thread.
    virtual void wake() = 0;
};

}  // namespace filepan::runtime
