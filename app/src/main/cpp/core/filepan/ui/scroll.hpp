// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "filepan/ui/animation.hpp"
#include "filepan/ui/gesture.hpp"
#include "filepan/ui/node.hpp"

// Scrolling: Android's fling curve, an overscroll stretch that springs back, and ScrollNode.
namespace filepan::ui {

// Android's OverScroller fling (SplineOverScroller): the same distance and duration for a given
// velocity, so flings feel native. Units are dp and dp/s.
class FlingSimulation {
public:
    FlingSimulation(float position, float velocity, float friction = 0.015f);

    [[nodiscard]] float position(float seconds) const;
    [[nodiscard]] float velocity(float seconds) const;
    [[nodiscard]] float duration() const { return duration_; }
    [[nodiscard]] float finalPosition() const { return start_ + distance_; }
    [[nodiscard]] bool done(float seconds) const { return seconds >= duration_; }

private:
    float start_;
    float distance_;  // Signed.
    float duration_;  // Seconds.
};

enum class ScrollAxis : std::uint8_t { vertical, horizontal };

// Scrolls its children, which it stacks along the axis like a column (or row) of unbounded
// length. Drag to scroll, fling, overscroll stretch, scroll semantics and keyboard paging come
// built in.
//
// The offset is in dp and is a signal (offsetSignal), so a collapsing header can bind to it.
// setPreScroll lets such a header consume part of each scroll delta before the content moves
// (nested scrolling), and setPostScroll gets what the content couldn't use.
class ScrollNode : public Node, private FrameClock::Ticker {
public:
    static std::shared_ptr<ScrollNode> create(ScrollAxis axis = ScrollAxis::vertical);
    explicit ScrollNode(ScrollAxis axis = ScrollAxis::vertical);
    ~ScrollNode() override;

    [[nodiscard]] ScrollAxis axis() const { return axis_; }
    [[nodiscard]] float offset() const { return offset_; }
    [[nodiscard]] const Signal<float> &offsetSignal() const { return offsetSignal_; }
    [[nodiscard]] float viewportExtent() const { return viewport_; }
    [[nodiscard]] float contentExtent() const { return content_; }
    [[nodiscard]] float maxOffset() const;
    // Overscroll stretch in dp: negative past the start, positive past the end.
    [[nodiscard]] float overscroll() const { return stretch_; }
    [[nodiscard]] bool flinging() const { return flinging_; }
    // The fling's velocity in dp/s as of the last frame (positive toward the end); 0 when not
    // flinging.
    [[nodiscard]] float flingVelocity() const;
    [[nodiscard]] bool animating() const { return flinging_ || springing_ || smooth_.has_value(); }

    void scrollTo(float offset, bool animated = false);
    // Scrolls by delta dp through the pre-scroll handler and clamps; returns what it moved.
    float scrollBy(float delta);
    // Starts a fling at velocity dp/s (positive toward the end).
    void fling(float velocity);
    void stopScrolling();

    // Gets each scroll delta (dp, positive toward the end) first; returns how much it consumed.
    using PreScroll = std::function<float(float delta)>;
    void setPreScroll(PreScroll preScroll) { preScroll_ = std::move(preScroll); }
    // Gets what's left after the content hit its edge (before it becomes overscroll); returns
    // how much it consumed. A header that expands only once the list is at the top uses this.
    void setPostScroll(PreScroll postScroll) { postScroll_ = std::move(postScroll); }

    // Without a set size along its axis it takes all the room it's given; fitting, it's as long
    // as its content up to that: a sheet's body that scrolls only once it would be too tall.
    void setFitContent(bool fit) {
        fitContent_ = fit;
        markLayoutDirty();
    }

    [[nodiscard]] bool hasCustomLayout() const override { return true; }
    gfx::Size measureCustom(LayoutEngine &engine, std::optional<float> width, std::optional<float> height,
                            float availableWidth, float availableHeight) override;
    void layoutCustom(LayoutEngine &engine, gfx::Size size) override;
    [[nodiscard]] gfx::Transform childrenTransformPx(float density) const override;
    bool performDefaultAction(Action action, const ActionArgs &args) override;
    // Scrolls (smoothly) just far enough to show rect, in content dp.
    gfx::Rect revealRect(gfx::Rect rect) override;

    // The drag recognizer, so a component can tweak it.
    [[nodiscard]] DragRecognizer &dragRecognizer() { return *drag_; }

protected:
    // Moves the offset (clamped by the caller) and notifies.
    void setOffset(float offset);
    // previous is the offset before this change.
    virtual void onOffsetChanged(float /*previous*/) {}
    // Lays out the content and returns its extent along the axis; VirtualList overrides it.
    virtual float layoutContent(LayoutEngine &engine, gfx::Size size);
    void setContentExtent(float extent) { content_ = extent; }
    void setViewportExtent(float extent) { viewport_ = extent; }
    [[nodiscard]] float density() const { return density_; }
    [[nodiscard]] bool vertical() const { return axis_ == ScrollAxis::vertical; }
    void updateScrollSemantics();

private:
    bool tick(std::int64_t nowNs) override;
    void dragBy(float delta);
    void release(float velocity);
    // startNs < 0: from the next frame.
    void startFling(float velocity, std::int64_t startNs);
    void startSpringBack(float velocity);
    void startTicking();

    struct SmoothScroll {
        float from = 0;
        float to = 0;
        std::int64_t startNs = -1;
    };

    ScrollAxis axis_;
    float offset_ = 0;
    Signal<float> offsetSignal_{0.0f};
    float viewport_ = 0;
    float content_ = 0;
    float density_ = 1;
    float stretch_ = 0;
    PreScroll preScroll_;
    PreScroll postScroll_;
    std::shared_ptr<DragRecognizer> drag_;

    bool flinging_ = false;
    bool fitContent_ = false;
    bool refit_ = false;
    float fitAvailable_ = 0;
    std::optional<FlingSimulation> fling_;
    std::int64_t flingStartNs_ = -1;
    float flingLast_ = 0;
    // The velocity of the fling the current drag caught (dp/s), and when.
    float carried_ = 0;
    std::int64_t caughtNs_ = 0;

    bool springing_ = false;
    float springFrom_ = 0;
    float springVelocity_ = 0;
    std::int64_t springStartNs_ = -1;

    std::optional<SmoothScroll> smooth_;
    bool ticking_ = false;
};

}  // namespace filepan::ui
