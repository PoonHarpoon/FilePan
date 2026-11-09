// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "filepan/gfx/geometry.hpp"
#include "filepan/ui/input.hpp"

// Gesture recognition with an arena, like Flutter's.
//
// When a pointer goes down, UiRoot hit-tests and offers it to every recognizer on the hit path
// (deepest first). Each one that's interested joins that pointer's arena. Recognizers then see
// the pointer's events and compete: one declares victory (a drag passing the touch slop, a long
// press reaching its timeout) and the rest are rejected; one gives up (a tap that moved too far)
// and leaves. When the pointer goes up with the arena still undecided, the first remaining
// member (the deepest) wins, so a tap on a button inside a scrolling list is a tap, and moving
// past the slop turns it into a scroll.
//
// Recognizers are owned by nodes; the arena holds shared references while a pointer is down.
namespace filepan::ui {

class GestureArena;

// Android's defaults (ViewConfiguration), in dp and nanoseconds.
struct GestureSettings {
    float touchSlop = 8;
    std::int64_t pressDelayNs = 100'000'000;     // Before a tap shows its pressed state.
    std::int64_t longPressNs = 400'000'000;
    std::int64_t doubleTapNs = 300'000'000;
    float minFlingVelocity = 50;    // dp/s
    float maxFlingVelocity = 8000;  // dp/s
};

// Estimates pointer velocity with a least-squares quadratic fit over the last 100 ms, like
// Android's default VelocityTracker strategy.
class VelocityTracker {
public:
    void add(std::int64_t timeNs, gfx::Vec2 position);
    void reset() { count_ = 0; }
    // Units per second; zero if the pointer stopped (no samples in the last 40 ms of motion, or
    // none in the 40 ms before atNs: the time of the up, as Android checks).
    [[nodiscard]] gfx::Vec2 velocity(std::optional<std::int64_t> atNs = std::nullopt) const;

private:
    static constexpr std::size_t kCapacity = 20;
    struct Sample {
        std::int64_t timeNs = 0;
        gfx::Vec2 position;
    };
    std::array<Sample, kCapacity> samples_{};
    std::size_t head_ = 0;  // Next write index.
    std::size_t count_ = 0;
};

class GestureRecognizer : public std::enable_shared_from_this<GestureRecognizer> {
public:
    virtual ~GestureRecognizer() = default;

    // A pointer went down on this recognizer's node. Join the arena to compete for it.
    virtual void addPointer(const PointerEvent &event, GestureArena &arena) = 0;
    // An event for a pointer this recognizer joined the arena for.
    virtual void handleEvent(const PointerEvent &event) = 0;
    virtual void acceptGesture(std::int32_t pointer) = 0;
    virtual void rejectGesture(std::int32_t pointer) = 0;
    // A deadline set with GestureArena::setDeadline passed.
    virtual void didExceedDeadline(std::int64_t) {}
    // Accessibility: the semantics actions (Action bits) this recognizer can perform, and
    // performing one without a pointer (TalkBack's double tap is a click).
    [[nodiscard]] virtual std::uint32_t semanticsActions() const { return 0; }
    virtual bool activate(std::uint32_t /*action*/) { return false; }

    void setEnabled(bool enabled) { enabled_ = enabled; }
    [[nodiscard]] bool enabled() const { return enabled_; }

protected:
    bool enabled_ = true;
};

class GestureArena {
public:
    GestureSettings settings;
    float density = 1;

    [[nodiscard]] float touchSlopPx() const { return settings.touchSlop * density; }

    void add(std::int32_t pointer, std::shared_ptr<GestureRecognizer> member);
    // No more members will join (called after the down event was offered to the whole path).
    void close(std::int32_t pointer);
    // The pointer went up: if no one has won, the first remaining member does.
    void sweep(std::int32_t pointer);
    // A member declares victory or gives up.
    void resolve(std::int32_t pointer, GestureRecognizer *member, bool accept);
    // Delivers an event to the pointer's members. localize, if set, fills in each member's
    // local position.
    using Localize = std::function<void(GestureRecognizer &, PointerEvent &)>;
    void dispatch(const PointerEvent &event, const Localize &localize = {});
    // Cancels everything for a pointer.
    void cancel(std::int32_t pointer);

    void setDeadline(GestureRecognizer *member, std::int64_t timeNs);
    void clearDeadline(GestureRecognizer *member);
    void checkDeadlines(std::int64_t nowNs);
    [[nodiscard]] std::optional<std::int64_t> nextDeadline() const;

    [[nodiscard]] bool hasPointer(std::int32_t pointer) const { return entries_.contains(pointer); }
    // Which member won the pointer, if any; for tests.
    [[nodiscard]] GestureRecognizer *winner(std::int32_t pointer) const;

private:
    struct Entry {
        std::vector<std::shared_ptr<GestureRecognizer>> members;
        GestureRecognizer *eagerWinner = nullptr;
        std::shared_ptr<GestureRecognizer> winner;
        bool open = true;
        bool done = false;  // Up or cancel seen: remove once resolved.
    };
    void resolveInFavorOf(std::int32_t pointer, Entry &entry, GestureRecognizer *member);
    void tryRemove(std::int32_t pointer);

    std::unordered_map<std::int32_t, Entry> entries_;
    struct Deadline {
        std::weak_ptr<GestureRecognizer> member;
        GestureRecognizer *key = nullptr;
        std::int64_t timeNs = 0;
    };
    std::vector<Deadline> deadlines_;
};

// Tracks a single pointer through the arena; the base of tap, long-press and drag.
class PrimaryPointerRecognizer : public GestureRecognizer {
public:
    void addPointer(const PointerEvent &event, GestureArena &arena) override;

protected:
    void resolve(bool accept);
    [[nodiscard]] bool tracking() const { return pointer_.has_value(); }
    void stopTracking();
    [[nodiscard]] float distanceFromDown(const PointerEvent &event) const;

    GestureArena *arena_ = nullptr;
    std::optional<std::int32_t> pointer_;
    PointerEvent down_;
};

class TapRecognizer final : public PrimaryPointerRecognizer {
public:
    // The pressed state: after a short delay, or right away when the tap wins.
    std::function<void(gfx::Vec2 local)> onTapDown;
    std::function<void()> onTap;
    // The press ended without a tap (it moved, or something else won).
    std::function<void()> onTapCancel;

    void addPointer(const PointerEvent &event, GestureArena &arena) override;
    void handleEvent(const PointerEvent &event) override;
    void acceptGesture(std::int32_t pointer) override;
    void rejectGesture(std::int32_t pointer) override;
    void didExceedDeadline(std::int64_t nowNs) override;
    [[nodiscard]] std::uint32_t semanticsActions() const override;
    bool activate(std::uint32_t action) override;

private:
    void fireDown();
    void reset();

    bool won_ = false;
    bool up_ = false;
    bool downFired_ = false;
};

// Wins when the pointer stays down for the long-press timeout without moving past the slop.
// After winning it keeps reporting moves, so long-press-then-drag can select ranges.
class LongPressRecognizer final : public PrimaryPointerRecognizer {
public:
    std::function<void(gfx::Vec2 local)> onLongPressStart;
    // offset is from where the long press started, in px.
    std::function<void(gfx::Vec2 local, gfx::Vec2 offset)> onLongPressMove;
    std::function<void(gfx::Vec2 local)> onLongPressEnd;

    void addPointer(const PointerEvent &event, GestureArena &arena) override;
    void handleEvent(const PointerEvent &event) override;
    void acceptGesture(std::int32_t pointer) override;
    void rejectGesture(std::int32_t pointer) override;
    void didExceedDeadline(std::int64_t nowNs) override;
    [[nodiscard]] std::uint32_t semanticsActions() const override;
    bool activate(std::uint32_t action) override;

private:
    bool accepted_ = false;
    PointerEvent last_;
};

enum class DragAxis : std::uint8_t { horizontal, vertical, free };

// Wins once the pointer moves past the touch slop along its axis. Scroll containers use one.
class DragRecognizer final : public PrimaryPointerRecognizer {
public:
    explicit DragRecognizer(DragAxis axis) : axis_(axis) {}

    std::function<void(gfx::Vec2 local)> onStart;
    // delta in window px since the last update (only the axis component for an axis drag).
    std::function<void(gfx::Vec2 delta)> onUpdate;
    // velocity in px/s.
    std::function<void(gfx::Vec2 velocity)> onEnd;
    std::function<void()> onCancel;

    // Claims the pointer on down: a scroll that's flinging catches the touch.
    void setAcceptOnDown(bool accept) { acceptOnDown_ = accept; }

    void addPointer(const PointerEvent &event, GestureArena &arena) override;
    void handleEvent(const PointerEvent &event) override;
    void acceptGesture(std::int32_t pointer) override;
    void rejectGesture(std::int32_t pointer) override;

    [[nodiscard]] DragAxis axis() const { return axis_; }
    [[nodiscard]] bool active() const { return accepted_; }
    // The time of the event being handled (or the last one): in onStart the down's when a
    // fling was caught, in onEnd the up's.
    [[nodiscard]] std::int64_t eventTimeNs() const { return lastTimeNs_; }

private:
    [[nodiscard]] gfx::Vec2 project(gfx::Vec2 delta) const;
    void start();

    DragAxis axis_;
    bool acceptOnDown_ = false;
    bool won_ = false;       // Won the arena.
    bool accepted_ = false;  // Dragging: won and past the slop.
    gfx::Vec2 last_;
    std::int64_t lastTimeNs_ = 0;
    gfx::Vec2 pending_;  // Movement before winning, delivered past the slop on win.
    VelocityTracker velocity_;
};

}  // namespace filepan::ui
