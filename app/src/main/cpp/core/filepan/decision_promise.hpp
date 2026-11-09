// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "filepan/cancellation.hpp"

// An answer that an I/O or job thread waits for while the UI asks the user: the jobs' conflict
// and error decisions, an archive password, an SFTP host key or login prompt.

namespace filepan {

namespace detail {

template <typename T>
struct DecisionState {
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<T> value;
    bool abandoned = false;  // Every promise was dropped unfulfilled.
    bool waiterGone = false;  // The job stopped waiting (cancelled).
};

// Shared by the copies of one promise; the last one to go abandons an unfulfilled decision.
template <typename T>
struct PromiseLink {
    std::shared_ptr<DecisionState<T>> state;

    ~PromiseLink() {
        std::lock_guard lock(state->mutex);
        if (!state->value) {
            state->abandoned = true;
            state->condition.notify_all();
        }
    }
};

}  // namespace detail

// The UI's side of a pending decision. Copyable, so it can travel in posted closures and wait in
// a list while the UI is away. Dropping every copy unfulfilled aborts the job.
template <typename T>
class DecisionPromise {
public:
    explicit DecisionPromise(std::shared_ptr<detail::DecisionState<T>> state)
        : link_(std::make_shared<detail::PromiseLink<T>>(std::move(state))) {}

    // Returns false if it was already decided or the job no longer waits.
    bool fulfil(T value) const {
        auto &state = *link_->state;
        std::lock_guard lock(state.mutex);
        if (state.value || state.waiterGone) {
            return false;
        }
        state.value = std::move(value);
        state.condition.notify_all();
        return true;
    }

    // Whether the job still waits for this decision.
    [[nodiscard]] bool isPending() const {
        auto &state = *link_->state;
        std::lock_guard lock(state.mutex);
        return !state.value && !state.waiterGone;
    }

private:
    std::shared_ptr<detail::PromiseLink<T>> link_;
};

// The job's side: blocks the job thread until the decision arrives.
template <typename T>
class DecisionFuture {
public:
    explicit DecisionFuture(std::shared_ptr<detail::DecisionState<T>> state)
        : state_(std::move(state)) {}
    DecisionFuture(DecisionFuture &&) = default;
    DecisionFuture &operator=(DecisionFuture &&) = default;

    ~DecisionFuture() {
        if (state_) {
            std::lock_guard lock(state_->mutex);
            state_->waiterGone = true;
        }
    }

    // The decision, or nullopt when cancelled or abandoned. Checks cancel periodically, since a
    // token can't wake a condition variable.
    std::optional<T> wait(const CancellationToken &cancel) {
        std::unique_lock lock(state_->mutex);
        while (!state_->value && !state_->abandoned) {
            if (cancel.isCancelled()) {
                return std::nullopt;
            }
            state_->condition.wait_for(lock, std::chrono::milliseconds(50));
        }
        return state_->value;
    }

private:
    std::shared_ptr<detail::DecisionState<T>> state_;
};

template <typename T>
std::pair<DecisionPromise<T>, DecisionFuture<T>> makeDecision() {
    auto state = std::make_shared<detail::DecisionState<T>>();
    return {DecisionPromise<T>(state), DecisionFuture<T>(state)};
}

}  // namespace filepan
