// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/reactive.hpp"

#include <algorithm>

namespace filepan::ui {

namespace detail {

namespace {

void eraseOne(std::vector<Source *> &list, Source *source) {
    if (auto it = std::ranges::find(list, source); it != list.end()) {
        list.erase(it);
    }
}

}  // namespace

Source::~Source() {
    for (Computation *observer : observers_) {
        eraseOne(observer->sources_, this);
        eraseOne(observer->newSources_, this);
    }
}

void Source::track() {
    if (Computation *listener = runtime().listener) {
        listener->addSource(this);
    }
}

void Source::notifyWrite() {
    for (std::size_t i = 0; i < observers_.size(); ++i) {
        observers_[i]->markStale(ComputationState::dirty);
    }
    runtime().afterWrite();
}

void Source::removeObserver(Computation *observer) {
    if (auto it = std::ranges::find(observers_, observer); it != observers_.end()) {
        *it = observers_.back();
        observers_.pop_back();
    }
}

Owner::~Owner() {
    if (!disposed_) {
        disposed_ = true;
        reset();
    }
}

void Owner::dispose() {
    if (disposed_) {
        return;
    }
    disposed_ = true;
    reset();
}

void Owner::addCleanup(std::function<void()> cleanup) { cleanups_.push_back(std::move(cleanup)); }

void Owner::adopt(std::shared_ptr<Owner> child) { owned_.push_back(std::move(child)); }

void Owner::reset() {
    auto owned = std::move(owned_);
    owned_.clear();
    for (auto &child : owned) {
        child->dispose();
    }
    auto cleanups = std::move(cleanups_);
    cleanups_.clear();
    for (auto &cleanup : cleanups) {
        cleanup();
    }
}

Computation::~Computation() {
    for (Source *source : sources_) {
        source->removeObserver(this);
    }
}

void Computation::dispose() {
    if (disposed()) {
        return;
    }
    for (Source *source : sources_) {
        source->removeObserver(this);
    }
    sources_.clear();
    Owner::dispose();
}

void Computation::markStale(ComputationState state) {
    if (state_ >= state) {
        return;
    }
    const bool wasClean = state_ == ComputationState::clean;
    state_ = state;
    if (!wasClean) {
        return;
    }
    if (isEffect()) {
        runtime().enqueue(this);
    } else if (Source *source = asSource()) {
        for (std::size_t i = 0; i < source->observers_.size(); ++i) {
            source->observers_[i]->markStale(ComputationState::check);
        }
    }
}

void Computation::updateIfNecessary() {
    if (disposed() && isEffect()) {
        return;
    }
    if (state_ == ComputationState::check) {
        for (std::size_t i = 0; i < sources_.size(); ++i) {
            sources_[i]->refresh();
            if (state_ == ComputationState::dirty) {
                break;
            }
        }
    }
    if (state_ == ComputationState::dirty) {
        execute();
    }
    state_ = ComputationState::clean;
}

void Computation::addSource(Source *source) {
    if (newSources_.empty()) {
        if (trackIndex_ < sources_.size() && sources_[trackIndex_] == source) {
            ++trackIndex_;
            return;
        }
        if (trackIndex_ > 0 && sources_[trackIndex_ - 1] == source) {
            return;
        }
    } else if (newSources_.back() == source) {
        return;
    }
    newSources_.push_back(source);
}

void Computation::beginTracking() {
    auto &rt = runtime();
    previousListener_ = rt.listener;
    previousOwner_ = rt.owner;
    rt.listener = this;
    rt.owner = this;
    trackIndex_ = 0;
    newSources_.clear();
}

void Computation::endTracking() {
    auto &rt = runtime();
    rt.listener = previousListener_;
    rt.owner = previousOwner_;
    if (!newSources_.empty() || trackIndex_ != sources_.size()) {
        for (std::size_t i = trackIndex_; i < sources_.size(); ++i) {
            sources_[i]->removeObserver(this);
        }
        sources_.resize(trackIndex_);
        for (Source *source : newSources_) {
            if (std::ranges::find(sources_, source) != sources_.end()) {
                continue;
            }
            sources_.push_back(source);
            source->observers_.push_back(this);
        }
        newSources_.clear();
    }
    int height = 0;
    for (Source *source : sources_) {
        height = std::max(height, source->height());
    }
    height_ = height + 1;
}

void Runtime::enqueue(Computation *effect) { queue_.push_back(effect->shared_from_this()); }

void Runtime::afterWrite() {
    if (batchDepth > 0 || flushing || queue_.empty()) {
        return;
    }
    if (scheduler) {
        if (!scheduled) {
            scheduled = true;
            scheduler();
        }
        return;
    }
    flush();
}

void Runtime::flush() {
    if (flushing) {
        return;
    }
    flushing = true;
    scheduled = false;
    // An effect that keeps writing what it reads would loop forever; give up after this many
    // rounds rather than hang the UI thread.
    constexpr int kMaxRounds = 10000;
    int rounds = 0;
    while (!queue_.empty()) {
        if (++rounds > kMaxRounds) {
            queue_.clear();
            break;
        }
        auto pending = std::move(queue_);
        queue_.clear();
        std::ranges::stable_sort(pending, {}, [](const std::shared_ptr<Owner> &owner) {
            return static_cast<const Computation *>(owner.get())->computationHeight();
        });
        for (auto &owner : pending) {
            static_cast<Computation *>(owner.get())->updateIfNecessary();
        }
    }
    flushing = false;
}

Runtime &runtime() {
    thread_local Runtime instance;
    return instance;
}

}  // namespace detail

Effect effect(std::function<void()> fn) {
    auto state = std::make_shared<detail::EffectState>(std::move(fn));
    if (auto *owner = detail::runtime().owner) {
        owner->adopt(state);
    }
    state->run();
    return Effect(std::move(state));
}

class Scope::RootOwner final : public detail::Owner {
public:
    void clear() { reset(); }
};

Scope::Scope() : owner_(std::make_shared<RootOwner>()) {}

Scope::~Scope() { owner_->dispose(); }

void Scope::dispose() { static_cast<RootOwner *>(owner_.get())->clear(); }

void onCleanup(std::function<void()> cleanup) {
    if (auto *owner = detail::runtime().owner) {
        owner->addCleanup(std::move(cleanup));
    }
}

void flushEffects() { detail::runtime().flush(); }

}  // namespace filepan::ui
