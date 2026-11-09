// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/gesture.hpp"

#include <algorithm>
#include <cmath>

#include "filepan/ui/semantics.hpp"

namespace filepan::ui {

// --- VelocityTracker ---

namespace {

constexpr std::int64_t kHorizonNs = 100'000'000;
constexpr std::int64_t kAssumeStoppedNs = 40'000'000;

}  // namespace

void VelocityTracker::add(std::int64_t timeNs, gfx::Vec2 position) {
    if (count_ > 0) {
        const auto &last = samples_[(head_ + kCapacity - 1) % kCapacity];
        if (timeNs - last.timeNs > kAssumeStoppedNs) {
            count_ = 0;  // The pointer rested: older motion says nothing about this one.
        }
    }
    samples_[head_] = {timeNs, position};
    head_ = (head_ + 1) % kCapacity;
    count_ = std::min(count_ + 1, kCapacity);
}

gfx::Vec2 VelocityTracker::velocity(std::optional<std::int64_t> atNs) const {
    if (count_ < 2) {
        return {};
    }
    const auto &newest = samples_[(head_ + kCapacity - 1) % kCapacity];
    if (atNs && *atNs - newest.timeNs > kAssumeStoppedNs) {
        return {};  // Nothing since: a resting finger reports no moves.
    }
    double t[kCapacity];
    double x[kCapacity];
    double y[kCapacity];
    std::size_t n = 0;
    for (std::size_t i = 0; i < count_; ++i) {
        const auto &sample = samples_[(head_ + kCapacity - 1 - i) % kCapacity];
        const std::int64_t age = newest.timeNs - sample.timeNs;
        if (age > kHorizonNs) {
            break;
        }
        t[n] = -static_cast<double>(age) * 1e-9;
        x[n] = sample.position.x;
        y[n] = sample.position.y;
        ++n;
    }
    if (n < 2) {
        return {};
    }
    // Least squares fit of p(t) = b0 + b1 t + b2 t^2; the velocity at the newest sample (t = 0)
    // is b1. Falls back to a line with two samples or a degenerate system.
    double s[5] = {};
    for (std::size_t i = 0; i < n; ++i) {
        double power = 1;
        for (double &sum : s) {
            sum += power;
            power *= t[i];
        }
    }
    const auto slope = [&](const double *values) {
        double xs[3] = {};
        for (std::size_t i = 0; i < n; ++i) {
            xs[0] += values[i];
            xs[1] += values[i] * t[i];
            xs[2] += values[i] * t[i] * t[i];
        }
        if (n >= 3) {
            const auto det3 = [](double a, double b, double c, double d, double e, double f, double g,
                                 double h, double k) {
                return a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
            };
            const double det = det3(s[0], s[1], s[2], s[1], s[2], s[3], s[2], s[3], s[4]);
            if (std::abs(det) > 1e-18) {
                return det3(s[0], xs[0], s[2], s[1], xs[1], s[3], s[2], xs[2], s[4]) / det;
            }
        }
        const double denominator = s[0] * s[2] - s[1] * s[1];
        if (std::abs(denominator) < 1e-18) {
            return 0.0;
        }
        return (s[0] * xs[1] - s[1] * xs[0]) / denominator;
    };
    return {static_cast<float>(slope(x)), static_cast<float>(slope(y))};
}

// --- GestureArena ---

void GestureArena::add(std::int32_t pointer, std::shared_ptr<GestureRecognizer> member) {
    auto &entry = entries_[pointer];
    if (entry.open) {
        entry.members.push_back(std::move(member));
    }
}

void GestureArena::close(std::int32_t pointer) {
    auto it = entries_.find(pointer);
    if (it == entries_.end()) {
        return;
    }
    Entry &entry = it->second;
    entry.open = false;
    if (entry.members.empty()) {
        entries_.erase(it);
    } else if (entry.members.size() == 1) {
        resolveInFavorOf(pointer, entry, entry.members.front().get());
    } else if (entry.eagerWinner != nullptr) {
        resolveInFavorOf(pointer, entry, entry.eagerWinner);
    }
}

void GestureArena::sweep(std::int32_t pointer) {
    auto it = entries_.find(pointer);
    if (it == entries_.end()) {
        return;
    }
    if (!it->second.winner && !it->second.members.empty()) {
        resolveInFavorOf(pointer, it->second, it->second.members.front().get());
    }
    entries_.erase(pointer);
}

void GestureArena::resolve(std::int32_t pointer, GestureRecognizer *member, bool accept) {
    auto it = entries_.find(pointer);
    if (it == entries_.end()) {
        return;
    }
    Entry &entry = it->second;
    auto found = std::ranges::find_if(entry.members, [member](const auto &m) { return m.get() == member; });
    if (found == entry.members.end()) {
        return;
    }
    if (!accept) {
        auto keep = *found;
        entry.members.erase(found);
        if (entry.eagerWinner == member) {
            entry.eagerWinner = nullptr;
        }
        if (entry.winner.get() == member) {
            entry.winner.reset();
        }
        keep->rejectGesture(pointer);
        it = entries_.find(pointer);
        if (it == entries_.end()) {
            return;
        }
        Entry &current = it->second;
        if (current.members.empty()) {
            entries_.erase(it);
        } else if (!current.open && !current.winner && current.members.size() == 1) {
            resolveInFavorOf(pointer, current, current.members.front().get());
        }
        return;
    }
    if (entry.winner) {
        return;
    }
    if (entry.open) {
        if (entry.eagerWinner == nullptr) {
            entry.eagerWinner = member;
        }
        return;
    }
    resolveInFavorOf(pointer, entry, member);
}

void GestureArena::resolveInFavorOf(std::int32_t pointer, Entry &entry, GestureRecognizer *member) {
    std::shared_ptr<GestureRecognizer> winner;
    std::vector<std::shared_ptr<GestureRecognizer>> losers;
    for (auto &m : entry.members) {
        if (m.get() == member && !winner) {
            winner = m;
        } else {
            losers.push_back(m);
        }
    }
    if (!winner) {
        return;
    }
    entry.members = {winner};
    entry.winner = winner;
    entry.eagerWinner = nullptr;
    for (auto &loser : losers) {
        clearDeadline(loser.get());
        loser->rejectGesture(pointer);
    }
    winner->acceptGesture(pointer);
}

void GestureArena::dispatch(const PointerEvent &event, const Localize &localize) {
    auto it = entries_.find(event.pointer);
    if (it == entries_.end()) {
        return;
    }
    const auto members = it->second.members;
    for (const auto &member : members) {
        auto current = entries_.find(event.pointer);
        if (current == entries_.end()) {
            return;
        }
        if (std::ranges::find(current->second.members, member) == current->second.members.end()) {
            continue;
        }
        if (localize) {
            PointerEvent local = event;
            localize(*member, local);
            member->handleEvent(local);
        } else {
            member->handleEvent(event);
        }
    }
}

void GestureArena::cancel(std::int32_t pointer) {
    auto it = entries_.find(pointer);
    if (it == entries_.end()) {
        return;
    }
    auto members = std::move(it->second.members);
    entries_.erase(it);
    for (auto &member : members) {
        clearDeadline(member.get());
        member->rejectGesture(pointer);
    }
}

GestureRecognizer *GestureArena::winner(std::int32_t pointer) const {
    auto it = entries_.find(pointer);
    return it == entries_.end() ? nullptr : it->second.winner.get();
}

void GestureArena::setDeadline(GestureRecognizer *member, std::int64_t timeNs) {
    clearDeadline(member);
    deadlines_.push_back({member->weak_from_this(), member, timeNs});
}

void GestureArena::clearDeadline(GestureRecognizer *member) {
    std::erase_if(deadlines_, [member](const Deadline &d) { return d.key == member; });
}

void GestureArena::checkDeadlines(std::int64_t nowNs) {
    std::vector<std::shared_ptr<GestureRecognizer>> due;
    std::erase_if(deadlines_, [&](const Deadline &d) {
        if (d.timeNs > nowNs) {
            return false;
        }
        if (auto member = d.member.lock()) {
            due.push_back(std::move(member));
        }
        return true;
    });
    for (auto &member : due) {
        member->didExceedDeadline(nowNs);
    }
}

std::optional<std::int64_t> GestureArena::nextDeadline() const {
    std::optional<std::int64_t> next;
    for (const auto &d : deadlines_) {
        if (!next || d.timeNs < *next) {
            next = d.timeNs;
        }
    }
    return next;
}

// --- PrimaryPointerRecognizer ---

void PrimaryPointerRecognizer::addPointer(const PointerEvent &event, GestureArena &arena) {
    if (!enabled_ || pointer_) {
        return;
    }
    arena_ = &arena;
    pointer_ = event.pointer;
    down_ = event;
    arena.add(event.pointer, shared_from_this());
}

void PrimaryPointerRecognizer::resolve(bool accept) {
    if (pointer_ && arena_ != nullptr) {
        arena_->resolve(*pointer_, this, accept);
    }
}

void PrimaryPointerRecognizer::stopTracking() {
    if (arena_ != nullptr) {
        arena_->clearDeadline(this);
    }
    pointer_.reset();
}

float PrimaryPointerRecognizer::distanceFromDown(const PointerEvent &event) const {
    const gfx::Vec2 d = event.position - down_.position;
    return std::hypot(d.x, d.y);
}

// --- TapRecognizer ---

void TapRecognizer::addPointer(const PointerEvent &event, GestureArena &arena) {
    if (tracking()) {
        return;
    }
    won_ = false;
    up_ = false;
    downFired_ = false;
    PrimaryPointerRecognizer::addPointer(event, arena);
    if (tracking()) {
        arena.setDeadline(this, event.timeNs + arena.settings.pressDelayNs);
    }
}

void TapRecognizer::handleEvent(const PointerEvent &event) {
    if (!pointer_ || event.pointer != *pointer_) {
        return;
    }
    switch (event.action) {
    case PointerAction::move:
        if (distanceFromDown(event) > arena_->touchSlopPx()) {
            if (won_) {
                const bool fired = downFired_;
                reset();
                if (fired && onTapCancel) {
                    onTapCancel();
                }
            } else {
                resolve(false);
            }
        }
        break;
    case PointerAction::up:
        up_ = true;
        if (won_) {
            fireDown();
            auto tap = onTap;
            reset();
            if (tap) {
                tap();
            }
        }
        break;
    case PointerAction::cancel:
        if (won_) {
            const bool fired = downFired_;
            reset();
            if (fired && onTapCancel) {
                onTapCancel();
            }
        } else {
            resolve(false);
        }
        break;
    case PointerAction::down:
        break;
    }
}

void TapRecognizer::acceptGesture(std::int32_t pointer) {
    if (!pointer_ || pointer != *pointer_) {
        return;
    }
    won_ = true;
    fireDown();
    if (up_) {
        auto tap = onTap;
        reset();
        if (tap) {
            tap();
        }
    }
}

void TapRecognizer::rejectGesture(std::int32_t pointer) {
    if (!pointer_ || pointer != *pointer_) {
        return;
    }
    const bool fired = downFired_;
    reset();
    if (fired && onTapCancel) {
        onTapCancel();
    }
}

void TapRecognizer::didExceedDeadline(std::int64_t) {
    if (tracking()) {
        fireDown();
    }
}

void TapRecognizer::fireDown() {
    if (downFired_) {
        return;
    }
    downFired_ = true;
    if (onTapDown) {
        onTapDown(down_.local);
    }
}

void TapRecognizer::reset() {
    stopTracking();
    won_ = false;
    up_ = false;
    downFired_ = false;
}

// --- LongPressRecognizer ---

void LongPressRecognizer::addPointer(const PointerEvent &event, GestureArena &arena) {
    if (tracking()) {
        return;
    }
    accepted_ = false;
    last_ = event;
    PrimaryPointerRecognizer::addPointer(event, arena);
    if (tracking()) {
        arena.setDeadline(this, event.timeNs + arena.settings.longPressNs);
    }
}

void LongPressRecognizer::handleEvent(const PointerEvent &event) {
    if (!pointer_ || event.pointer != *pointer_) {
        return;
    }
    last_ = event;
    if (!accepted_) {
        if ((event.action == PointerAction::move && distanceFromDown(event) > arena_->touchSlopPx()) ||
            event.action == PointerAction::up || event.action == PointerAction::cancel) {
            resolve(false);
        }
        return;
    }
    switch (event.action) {
    case PointerAction::move:
        if (onLongPressMove) {
            onLongPressMove(event.local, event.position - down_.position);
        }
        break;
    case PointerAction::up:
    case PointerAction::cancel:
        accepted_ = false;
        stopTracking();
        if (onLongPressEnd) {
            onLongPressEnd(event.local);
        }
        break;
    case PointerAction::down:
        break;
    }
}

void LongPressRecognizer::acceptGesture(std::int32_t pointer) {
    if (!pointer_ || pointer != *pointer_ || accepted_) {
        return;
    }
    accepted_ = true;
    if (onLongPressStart) {
        onLongPressStart(last_.local);
    }
}

void LongPressRecognizer::rejectGesture(std::int32_t pointer) {
    if (!pointer_ || pointer != *pointer_) {
        return;
    }
    accepted_ = false;
    stopTracking();
}

void LongPressRecognizer::didExceedDeadline(std::int64_t) {
    if (tracking() && !accepted_) {
        resolve(true);
    }
}

// --- DragRecognizer ---

gfx::Vec2 DragRecognizer::project(gfx::Vec2 delta) const {
    switch (axis_) {
    case DragAxis::horizontal:
        return {delta.x, 0};
    case DragAxis::vertical:
        return {0, delta.y};
    case DragAxis::free:
        break;
    }
    return delta;
}

void DragRecognizer::addPointer(const PointerEvent &event, GestureArena &arena) {
    if (tracking()) {
        return;
    }
    won_ = false;
    accepted_ = false;
    last_ = event.position;
    lastTimeNs_ = event.timeNs;
    pending_ = {};
    velocity_.reset();
    PrimaryPointerRecognizer::addPointer(event, arena);
    if (!tracking()) {
        return;
    }
    velocity_.add(event.timeNs, event.position);
    if (acceptOnDown_) {
        resolve(true);
    }
}

void DragRecognizer::start() {
    accepted_ = true;
    if (onStart) {
        onStart(down_.local);
    }
    // Deliver what moved past the slop, so the content doesn't jump by the slop when it starts.
    const gfx::Vec2 along = project(pending_);
    const float length = std::hypot(along.x, along.y);
    const float slop = acceptOnDown_ ? 0 : arena_->touchSlopPx();
    pending_ = {};
    if (length > slop && onUpdate) {
        onUpdate(along * ((length - slop) / length));
    }
}

void DragRecognizer::handleEvent(const PointerEvent &event) {
    if (!pointer_ || event.pointer != *pointer_) {
        return;
    }
    lastTimeNs_ = event.timeNs;
    switch (event.action) {
    case PointerAction::move: {
        velocity_.add(event.timeNs, event.position);
        const gfx::Vec2 delta = event.position - last_;
        last_ = event.position;
        if (accepted_) {
            if (onUpdate) {
                onUpdate(project(delta));
            }
            break;
        }
        pending_ = pending_ + delta;
        const gfx::Vec2 along = project(pending_);
        if (std::hypot(along.x, along.y) > arena_->touchSlopPx()) {
            if (won_) {
                start();  // Already won by default (the others gave up); now it's a drag.
            } else {
                resolve(true);
            }
        }
        break;
    }
    case PointerAction::up:
    case PointerAction::cancel: {
        const bool dragging = accepted_;
        const bool won = won_;
        const gfx::Vec2 velocity = project(velocity_.velocity(event.timeNs));
        accepted_ = false;
        won_ = false;
        if (dragging || won) {
            stopTracking();
        } else {
            resolve(false);
        }
        if (dragging && event.action == PointerAction::up && onEnd) {
            onEnd(velocity);
        } else if (dragging && event.action == PointerAction::cancel && onCancel) {
            onCancel();
        }
        break;
    }
    case PointerAction::down:
        break;
    }
}

void DragRecognizer::acceptGesture(std::int32_t pointer) {
    if (!pointer_ || pointer != *pointer_ || won_) {
        return;
    }
    won_ = true;
    // Winning because everything else gave up isn't a drag yet: that waits for the slop, unless
    // this drag claimed the pointer on down (catching a fling).
    const gfx::Vec2 along = project(pending_);
    if (acceptOnDown_ || std::hypot(along.x, along.y) > arena_->touchSlopPx()) {
        start();
    }
}

void DragRecognizer::rejectGesture(std::int32_t pointer) {
    if (!pointer_ || pointer != *pointer_) {
        return;
    }
    const bool wasActive = accepted_;
    accepted_ = false;
    won_ = false;
    stopTracking();
    if (wasActive && onCancel) {
        onCancel();
    }
}

// --- Accessibility activation ---

std::uint32_t TapRecognizer::semanticsActions() const {
    return enabled_ && onTap ? static_cast<std::uint32_t>(Action::click) : 0;
}

bool TapRecognizer::activate(std::uint32_t action) {
    if (action != static_cast<std::uint32_t>(Action::click) || !enabled_ || !onTap) {
        return false;
    }
    onTap();
    return true;
}

std::uint32_t LongPressRecognizer::semanticsActions() const {
    return enabled_ && onLongPressStart ? static_cast<std::uint32_t>(Action::longClick) : 0;
}

bool LongPressRecognizer::activate(std::uint32_t action) {
    if (action != static_cast<std::uint32_t>(Action::longClick) || !enabled_ || !onLongPressStart) {
        return false;
    }
    onLongPressStart({});
    if (onLongPressEnd) {
        onLongPressEnd({});
    }
    return true;
}

}  // namespace filepan::ui
