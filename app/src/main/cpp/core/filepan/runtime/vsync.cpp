// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/runtime/vsync.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <utility>

#ifdef __ANDROID__
#include <android/choreographer.h>
#include <android/looper.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "filepan/runtime/log.hpp"
#endif

namespace filepan::runtime {

namespace {

#ifdef __ANDROID__

// The UI thread's looper: an eventfd for wakeups, the thread's AChoreographer for vsync.
class AndroidVsyncWaiter final : public VsyncWaiter {
public:
    AndroidVsyncWaiter(VsyncCallback callback, std::function<std::int64_t()> clock)
        : callback_(std::move(callback)), clock_(std::move(clock)) {
        looper_ = ALooper_prepare(0);
        ALooper_acquire(looper_);
        eventFd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (eventFd_ < 0) {
            FILEPAN_LOGE("eventfd failed");
        } else {
            ALooper_addFd(looper_, eventFd_, ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, &drain,
                          nullptr);
        }
        // Needs the looper prepared above; the instance belongs to this thread.
        choreographer_ = AChoreographer_getInstance();
    }

    ~AndroidVsyncWaiter() override {
        // A vsync callback still posted never fires: nobody polls this looper again.
        if (eventFd_ >= 0) {
            ALooper_removeFd(looper_, eventFd_);
            close(eventFd_);
        }
        ALooper_release(looper_);
    }

    void requestVsync() override {
        if (vsyncPending_ || choreographer_ == nullptr) {
            return;
        }
        vsyncPending_ = true;
        AChoreographer_postVsyncCallback(choreographer_, &onVsync, this);
    }

    void wait(std::optional<std::int64_t> deadlineNs) override {
        int timeoutMs = -1;
        if (deadlineNs) {
            const std::int64_t remaining = std::max<std::int64_t>(0, *deadlineNs - clock_());
            timeoutMs = static_cast<int>(
                    std::min<std::int64_t>((remaining + 999'999) / 1'000'000, 60'000));
        }
        // Runs the eventfd and vsync callbacks.
        ALooper_pollOnce(timeoutMs, nullptr, nullptr, nullptr);
    }

    void wake() override {
        if (eventFd_ >= 0) {
            const std::uint64_t one = 1;
            [[maybe_unused]] auto written = write(eventFd_, &one, sizeof(one));
        }
    }

private:
    static int drain(int fd, int, void *) {
        std::uint64_t value;
        [[maybe_unused]] auto got = read(fd, &value, sizeof(value));
        return 1;
    }

    static void onVsync(const AChoreographerFrameCallbackData *data, void *self) {
        auto *waiter = static_cast<AndroidVsyncWaiter *>(self);
        waiter->vsyncPending_ = false;
        waiter->callback_(AChoreographerFrameCallbackData_getFrameTimeNanos(data));
    }

    VsyncCallback callback_;
    std::function<std::int64_t()> clock_;
    ALooper *looper_ = nullptr;
    AChoreographer *choreographer_ = nullptr;
    int eventFd_ = -1;
    bool vsyncPending_ = false;
};

#else

// A condition variable, and vsync on multiples of a fixed interval.
class HostVsyncWaiter final : public VsyncWaiter {
public:
    HostVsyncWaiter(VsyncCallback callback, std::int64_t intervalNs,
                    std::function<std::int64_t()> clock)
        : callback_(std::move(callback)), intervalNs_(std::max<std::int64_t>(1, intervalNs)),
          clock_(std::move(clock)) {}

    void requestVsync() override {
        if (vsyncPending_) {
            return;
        }
        vsyncPending_ = true;
        nextVsync_ = (clock_() / intervalNs_ + 1) * intervalNs_;
    }

    void wait(std::optional<std::int64_t> deadlineNs) override {
        constexpr auto kNever = std::numeric_limits<std::int64_t>::max();
        const std::int64_t until =
                std::min(deadlineNs.value_or(kNever), vsyncPending_ ? nextVsync_ : kNever);
        {
            std::unique_lock lock(mutex_);
            while (!woken_) {
                const std::int64_t now = clock_();
                if (now >= until) {
                    break;
                }
                if (until == kNever) {
                    condition_.wait(lock);
                } else {
                    condition_.wait_for(lock, std::chrono::nanoseconds(until - now));
                }
            }
            woken_ = false;
        }
        if (vsyncPending_ && clock_() >= nextVsync_) {
            vsyncPending_ = false;
            callback_(nextVsync_);
        }
    }

    void wake() override {
        {
            std::lock_guard lock(mutex_);
            woken_ = true;
        }
        condition_.notify_one();
    }

private:
    VsyncCallback callback_;
    std::int64_t intervalNs_;
    std::function<std::int64_t()> clock_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool woken_ = false;
    bool vsyncPending_ = false;
    std::int64_t nextVsync_ = 0;
};

#endif

}  // namespace

std::unique_ptr<VsyncWaiter> VsyncWaiter::create(VsyncCallback callback,
                                                 [[maybe_unused]] std::int64_t intervalNs,
                                                 std::function<std::int64_t()> clock) {
#ifdef __ANDROID__
    return std::make_unique<AndroidVsyncWaiter>(std::move(callback), std::move(clock));
#else
    return std::make_unique<HostVsyncWaiter>(std::move(callback), intervalNs, std::move(clock));
#endif
}

}  // namespace filepan::runtime
