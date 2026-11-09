// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/jobs/decision.hpp"

#include <algorithm>

namespace filepan::jobs {

void QueuedDecisionProvider::setListener(std::function<void()> listener) {
    std::lock_guard lock(mutex_);
    listener_ = std::move(listener);
}

void QueuedDecisionProvider::prune() {
    std::erase_if(entries_, [](const Entry &entry) {
        return std::visit([](const auto &promise) { return !promise.isPending(); },
                          entry.promise);
    });
}

std::vector<QueuedDecisionProvider::Pending> QueuedDecisionProvider::pending() {
    std::lock_guard lock(mutex_);
    prune();
    std::vector<Pending> result;
    result.reserve(entries_.size());
    for (const auto &entry : entries_) {
        result.push_back({entry.id, entry.request});
    }
    return result;
}

namespace {

template <typename Decision, typename Entries, typename Id>
bool resolveIn(std::mutex &mutex, Entries &entries, Id id, Decision decision) {
    std::unique_lock lock(mutex);
    const auto found =
        std::ranges::find_if(entries, [id](const auto &entry) { return entry.id == id; });
    if (found == entries.end()) {
        return false;
    }
    const auto *promise = std::get_if<DecisionPromise<Decision>>(&found->promise);
    if (!promise) {
        return false;
    }
    const auto keep = *promise;
    entries.erase(found);
    lock.unlock();
    return keep.fulfil(decision);
}

}  // namespace

bool QueuedDecisionProvider::resolve(RequestId id, ConflictDecision decision) {
    return resolveIn(mutex_, entries_, id, decision);
}

bool QueuedDecisionProvider::resolve(RequestId id, ErrorDecision decision) {
    return resolveIn(mutex_, entries_, id, decision);
}

void QueuedDecisionProvider::add(Entry entry) {
    std::function<void()> listener;
    {
        std::lock_guard lock(mutex_);
        entry.id = nextId_++;
        prune();
        entries_.push_back(std::move(entry));
        listener = listener_;
    }
    if (listener) {
        listener();
    }
}

void QueuedDecisionProvider::requestConflict(ConflictRequest request,
                                             DecisionPromise<ConflictDecision> promise) {
    add({0, std::move(request), std::move(promise)});
}

void QueuedDecisionProvider::requestError(ErrorRequest request,
                                          DecisionPromise<ErrorDecision> promise) {
    add({0, std::move(request), std::move(promise)});
}

}  // namespace filepan::jobs
