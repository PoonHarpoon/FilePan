// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/ui_loop.hpp"

#include <chrono>

namespace filepan::ui {

namespace {

std::int64_t steadyNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

}  // namespace

UiLoop::UiLoop() : clock_(steadyNow) {}

UiLoop::~UiLoop() = default;

std::int64_t UiLoop::now() const { return clock_(); }

void UiLoop::wake() {
    std::function<void()> wakeup;
    {
        // Taking the lock orders this after a waiter's predicate check, so the notify isn't lost.
        std::lock_guard lock(mutex_);
        wakeup = wakeup_;
    }
    condition_.notify_all();
    if (wakeup) {
        wakeup();
    }
}

void UiLoop::post(Task task) {
    {
        std::lock_guard lock(mutex_);
        tasks_.push_back(std::move(task));
    }
    wake();
}

UiLoop::TimerId UiLoop::postDelayed(Task task, std::int64_t delayNs) {
    TimerId id;
    {
        std::lock_guard lock(mutex_);
        id = nextTimer_++;
        timers_.emplace(now() + delayNs, std::make_pair(id, std::move(task)));
    }
    wake();
    return id;
}

void UiLoop::cancel(TimerId timer) {
    std::lock_guard lock(mutex_);
    for (auto it = timers_.begin(); it != timers_.end(); ++it) {
        if (it->second.first == timer) {
            timers_.erase(it);
            return;
        }
    }
}

void UiLoop::requestFrame() {
    if (!frameRequested_.exchange(true, std::memory_order_acq_rel)) {
        wake();
    }
}

void UiLoop::setWakeup(std::function<void()> wakeup) {
    std::lock_guard lock(mutex_);
    wakeup_ = std::move(wakeup);
}

void UiLoop::quit() {
    quit_.store(true, std::memory_order_release);
    wake();
}

void UiLoop::setFrameCallback(std::function<void(std::int64_t)> callback) {
    frameCallback_ = std::move(callback);
}

std::optional<std::int64_t> UiLoop::runPending() {
    std::deque<Task> tasks;
    {
        std::lock_guard lock(mutex_);
        tasks.swap(tasks_);
    }
    for (auto &task : tasks) {
        task();
    }
    // Timers due now; ones they add for later wait for the next call.
    const std::int64_t current = now();
    while (true) {
        Task task;
        {
            std::lock_guard lock(mutex_);
            auto it = timers_.begin();
            if (it == timers_.end() || it->first > current) {
                break;
            }
            task = std::move(it->second.second);
            timers_.erase(it);
        }
        task();
    }
    std::lock_guard lock(mutex_);
    if (timers_.empty()) {
        return std::nullopt;
    }
    return timers_.begin()->first;
}

void UiLoop::runFrame(std::int64_t frameTimeNs) {
    if (!frameRequested_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (frameCallback_) {
        frameCallback_(frameTimeNs);
    }
}

void UiLoop::run() {
    while (!quit_.load(std::memory_order_acquire)) {
        const auto deadline = runPending();
        if (quit_.load(std::memory_order_acquire)) {
            break;
        }
        if (frameRequested()) {
            runFrame(now());
            continue;
        }
        std::unique_lock lock(mutex_);
        const auto ready = [this] {
            return !tasks_.empty() || frameRequested() || quit_.load(std::memory_order_acquire);
        };
        if (deadline) {
            const auto wait = std::chrono::nanoseconds(*deadline - now());
            condition_.wait_for(lock, wait, ready);
        } else {
            condition_.wait(lock, ready);
        }
    }
}

IoPool::IoPool(UiLoop &loop, unsigned threads) : loop_(loop) {
    workers_.reserve(threads);
    for (unsigned i = 0; i < threads; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

IoPool::~IoPool() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_all();
    for (auto &worker : workers_) {
        worker.join();
    }
}

void IoPool::enqueue(std::function<void()> job) {
    {
        std::lock_guard lock(mutex_);
        jobs_.push_back(std::move(job));
    }
    condition_.notify_one();
}

void IoPool::workerLoop() {
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_ && jobs_.empty()) {
                return;
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        job();
    }
}

}  // namespace filepan::ui
