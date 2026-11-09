// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// The UI thread's loop and the I/O pool.
//
// UiLoop is a task queue that other threads post closures to, plus timers and a frame request.
// It's portable: the platform drives it. On Android, the wakeup hook signals an eventfd that the
// UI thread's ALooper watches, which then calls runPending(), and frame requests become
// AChoreographer callbacks that call runFrame(). On a host, run() is a condition-variable loop,
// and tests call runPending()/runFrame() by hand.
//
// IoPool runs blocking work (file system, thumbnails, SFTP) on worker threads and delivers each
// result back on the UI thread, where it may touch signals and nodes.
namespace filepan::ui {

class UiLoop {
public:
    using Task = std::function<void()>;
    using TimerId = std::uint64_t;

    UiLoop();
    ~UiLoop();
    UiLoop(const UiLoop &) = delete;
    UiLoop &operator=(const UiLoop &) = delete;

    // Monotonic nanoseconds; replaceable for tests.
    [[nodiscard]] std::int64_t now() const;
    void setClock(std::function<std::int64_t()> clock) { clock_ = std::move(clock); }

    // --- Any thread ---
    void post(Task task);
    TimerId postDelayed(Task task, std::int64_t delayNs);
    void cancel(TimerId timer);
    // Coalesced: one frame runs however many times this is called before it.
    void requestFrame();
    // Called from whichever thread posted, to wake the UI thread. The host loop doesn't need it.
    void setWakeup(std::function<void()> wakeup);
    void quit();

    // --- UI thread ---
    // What a frame does (UiRoot::frame plus handing the result to the renderer).
    void setFrameCallback(std::function<void(std::int64_t frameTimeNs)> callback);
    // Runs posted tasks and due timers. Returns the next timer's deadline, if any.
    std::optional<std::int64_t> runPending();
    [[nodiscard]] bool frameRequested() const { return frameRequested_.load(std::memory_order_acquire); }
    // Runs the requested frame, if any.
    void runFrame(std::int64_t frameTimeNs);
    // Host loop: runs tasks, timers and frames until quit().
    void run();

private:
    void wake();

    std::function<std::int64_t()> clock_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Task> tasks_;
    std::multimap<std::int64_t, std::pair<TimerId, Task>> timers_;
    TimerId nextTimer_ = 1;
    std::function<void()> wakeup_;
    std::function<void(std::int64_t)> frameCallback_;
    std::atomic<bool> frameRequested_{false};
    std::atomic<bool> quit_{false};
};

// A cancellation flag shared between a submitted job and whoever may cancel it.
class Cancellation {
public:
    void cancel() { cancelled_.store(true, std::memory_order_release); }
    [[nodiscard]] bool cancelled() const { return cancelled_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> cancelled_{false};
};

class IoPool {
public:
    explicit IoPool(UiLoop &loop, unsigned threads = 4);
    ~IoPool();
    IoPool(const IoPool &) = delete;
    IoPool &operator=(const IoPool &) = delete;

    // Runs work on a worker; then then(result) on the UI thread, unless cancelled first.
    // work must not touch signals or nodes.
    template <typename Work, typename Then>
    std::shared_ptr<Cancellation> submit(Work work, Then then) {
        auto token = std::make_shared<Cancellation>();
        enqueue([this, token, work = std::move(work), then = std::move(then)]() mutable {
            if (token->cancelled()) {
                return;
            }
            using Result = std::invoke_result_t<Work &>;
            if constexpr (std::is_void_v<Result>) {
                work();
                loop_.post([token, then]() mutable {
                    if (!token->cancelled()) {
                        then();
                    }
                });
            } else {
                // Shared so the posted closure stays copyable whatever Result is.
                auto result = std::make_shared<Result>(work());
                loop_.post([token, then, result]() mutable {
                    if (!token->cancelled()) {
                        then(std::move(*result));
                    }
                });
            }
        });
        return token;
    }

    // Fire and forget, no UI-thread callback.
    void execute(std::function<void()> work) { enqueue(std::move(work)); }

private:
    void enqueue(std::function<void()> job);
    void workerLoop();

    UiLoop &loop_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::function<void()>> jobs_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};

}  // namespace filepan::ui
