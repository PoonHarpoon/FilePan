// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/scroll.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "filepan/ui/layout.hpp"

namespace filepan::ui {

namespace {

constexpr int kSplineSamples = 100;

// SplineOverScroller's position table.
const std::array<float, kSplineSamples + 1> &splinePositions() {
    static const auto table = [] {
        constexpr float kInflexion = 0.35f;
        constexpr float kStartTension = 0.5f;
        constexpr float kEndTension = 1.0f;
        constexpr float kP1 = kStartTension * kInflexion;
        constexpr float kP2 = 1.0f - kEndTension * (1.0f - kInflexion);
        std::array<float, kSplineSamples + 1> positions{};
        float xMin = 0;
        for (int i = 0; i < kSplineSamples; ++i) {
            const float alpha = static_cast<float>(i) / kSplineSamples;
            float xMax = 1;
            float x;
            float coef;
            while (true) {
                x = xMin + (xMax - xMin) / 2;
                coef = 3 * x * (1 - x);
                const float tx = coef * ((1 - x) * kP1 + x * kP2) + x * x * x;
                if (std::abs(tx - alpha) < 1e-5f) {
                    break;
                }
                if (tx > alpha) {
                    xMax = x;
                } else {
                    xMin = x;
                }
            }
            positions[i] = coef * ((1 - x) * kStartTension + x) + x * x * x;
        }
        positions[kSplineSamples] = 1;
        return positions;
    }();
    return table;
}

// Android's ViewConfiguration fling limits and EdgeEffect's spring.
constexpr float kMinFlingVelocity = 50;
constexpr float kMaxFlingVelocity = 8000;
constexpr float kStretchStiffness = 850;
constexpr float kStretchDamping = 1;
// How much of a fling's velocity becomes stretch when it hits an edge.
constexpr float kAbsorbFactor = 0.3f;
// Visual scale per viewport of stretch.
constexpr float kStretchScale = 0.5f;
constexpr std::int64_t kSmoothScrollNs = 300'000'000;
// A swipe adds the fling it caught when it's at least this fraction as fast (Flutter's
// momentum retention uses the same) and lifts within this long of the catch.
constexpr float kFlywheelMinRatio = 0.5f;
constexpr std::int64_t kFlywheelWindowNs = 300'000'000;

}  // namespace

FlingSimulation::FlingSimulation(float position, float velocity, float friction) : start_(position) {
    static const double kDecelerationRate = std::log(0.78) / std::log(0.9);
    constexpr double kInflexion = 0.35;
    // Gravity * inches per meter * ppi (160 at 1 dp = 1 px) * 0.84, as in OverScroller.
    constexpr double kPhysicalCoefficient = 9.80665 * 39.37 * 160.0 * 0.84;
    if (velocity == 0 || friction <= 0) {
        distance_ = 0;
        duration_ = 0;
        return;
    }
    const double l = std::log(kInflexion * std::abs(velocity) / (friction * kPhysicalCoefficient));
    duration_ = static_cast<float>(std::exp(l / (kDecelerationRate - 1)));
    const double distance =
            friction * kPhysicalCoefficient * std::exp(kDecelerationRate / (kDecelerationRate - 1) * l);
    distance_ = static_cast<float>(velocity > 0 ? distance : -distance);
}

float FlingSimulation::position(float seconds) const {
    if (seconds >= duration_ || duration_ <= 0) {
        return start_ + distance_;
    }
    const auto &table = splinePositions();
    const float fraction = std::max(0.0f, seconds / duration_);
    const int index = std::min(static_cast<int>(kSplineSamples * fraction), kSplineSamples - 1);
    const float tInf = static_cast<float>(index) / kSplineSamples;
    const float tSup = static_cast<float>(index + 1) / kSplineSamples;
    const float velocityCoef = (table[index + 1] - table[index]) / (tSup - tInf);
    const float distanceCoef = table[index] + (fraction - tInf) * velocityCoef;
    return start_ + distanceCoef * distance_;
}

float FlingSimulation::velocity(float seconds) const {
    if (seconds >= duration_ || duration_ <= 0) {
        return 0;
    }
    const auto &table = splinePositions();
    const float fraction = std::max(0.0f, seconds / duration_);
    const int index = std::min(static_cast<int>(kSplineSamples * fraction), kSplineSamples - 1);
    const float velocityCoef = (table[index + 1] - table[index]) * kSplineSamples;
    return velocityCoef * distance_ / duration_;
}

// --- ScrollNode ---

std::shared_ptr<ScrollNode> ScrollNode::create(ScrollAxis axis) { return std::make_shared<ScrollNode>(axis); }

ScrollNode::ScrollNode(ScrollAxis axis)
    : axis_(axis),
      drag_(std::make_shared<DragRecognizer>(axis == ScrollAxis::vertical ? DragAxis::vertical
                                                                          : DragAxis::horizontal)) {
    const auto along = [this](gfx::Vec2 v) { return -(vertical() ? v.y : v.x) / density_; };
    drag_->onStart = [this](gfx::Vec2) {
        // Catching a fling: remember how fast it was going, for a swipe on in the same direction.
        carried_ = 0;
        if (flinging_ && fling_) {
            caughtNs_ = drag_->eventTimeNs();
            const float seconds =
                    flingStartNs_ < 0 ? 0 : std::max(0.0f, static_cast<float>(caughtNs_ - flingStartNs_) * 1e-9f);
            carried_ = fling_->velocity(seconds);
        }
        flinging_ = false;
        springing_ = false;
        smooth_.reset();
        drag_->setAcceptOnDown(false);
    };
    drag_->onUpdate = [this, along](gfx::Vec2 delta) { dragBy(along(delta)); };
    drag_->onEnd = [this, along](gfx::Vec2 velocity) { release(along(velocity)); };
    drag_->onCancel = [this] { release(0); };
    addGestureRecognizer(drag_);
    setClip(true);
    updateSemantics([](Semantics &s) { s.role = Role::scroll; });
}

ScrollNode::~ScrollNode() { frameClock().stop(this); }

float ScrollNode::flingVelocity() const {
    if (!flinging_ || !fling_) {
        return 0;
    }
    const float seconds = flingStartNs_ < 0 ? 0 : static_cast<float>(frameClock().now() - flingStartNs_) * 1e-9f;
    return fling_->velocity(seconds);
}

float ScrollNode::maxOffset() const { return std::max(0.0f, content_ - viewport_); }

void ScrollNode::setOffset(float offset) {
    if (offset == offset_) {
        return;
    }
    const float previous = offset_;
    offset_ = offset;
    offsetSignal_.set(offset);
    invalidateComposite();
    updateScrollSemantics();
    onOffsetChanged(previous);
}

void ScrollNode::scrollTo(float offset, bool animated) {
    const float target = std::clamp(offset, 0.0f, maxOffset());
    stopScrolling();
    if (animated && !frameClock().reducedMotion && target != offset_) {
        smooth_ = SmoothScroll{offset_, target, -1};
        startTicking();
    } else {
        setOffset(target);
    }
}

float ScrollNode::scrollBy(float delta) {
    const float consumed = preScroll_ ? preScroll_(delta) : 0;
    const float target = std::clamp(offset_ + delta - consumed, 0.0f, maxOffset());
    const float moved = target - offset_;
    setOffset(target);
    const float leftover = delta - consumed - moved;
    const float postConsumed = postScroll_ && leftover != 0 ? postScroll_(leftover) : 0;
    return consumed + moved + postConsumed;
}

void ScrollNode::fling(float velocity) { startFling(velocity, -1); }

void ScrollNode::startFling(float velocity, std::int64_t startNs) {
    smooth_.reset();
    velocity = std::clamp(velocity, -kMaxFlingVelocity, kMaxFlingVelocity);
    fling_.emplace(0, velocity);
    flingLast_ = 0;
    flingStartNs_ = startNs;
    flinging_ = true;
    // A touch during the fling catches it rather than tapping what's under the finger.
    drag_->setAcceptOnDown(true);
    startTicking();
}

void ScrollNode::stopScrolling() {
    flinging_ = false;
    smooth_.reset();
    drag_->setAcceptOnDown(false);
}

void ScrollNode::dragBy(float delta) {
    if (stretch_ != 0 && (delta > 0) != (stretch_ > 0)) {
        // Moving back toward the content undoes the stretch first.
        const float remaining = stretch_ + delta;
        if (remaining != 0 && (remaining > 0) == (stretch_ > 0)) {
            stretch_ = remaining;
            invalidateComposite();
            return;
        }
        stretch_ = 0;
        invalidateComposite();
        delta = remaining;
    }
    const float moved = scrollBy(delta);
    const float excess = delta - moved;
    // Only what an edge stopped is overscroll. Far down a long list, offset + delta rounds to
    // a float a little short of what was asked; that isn't a pull past the end, and a stretch
    // left over from it would turn the release into a spring back rather than a fling.
    const bool atEdge = delta > 0 ? offset_ >= maxOffset() : offset_ <= 0;
    if (atEdge && std::abs(excess) > 1e-4f) {
        const float limit = std::max(viewport_, 1.0f);
        const float resistance = 0.5f * (1 - std::min(1.0f, std::abs(stretch_) / limit));
        stretch_ += excess * resistance;
        invalidateComposite();
    }
}

void ScrollNode::release(float velocity) {
    const float carried = std::exchange(carried_, 0.0f);
    // A quick swipe on in the direction of the fling it caught adds to it, as Android's
    // OverScroller flywheel does: repeated swipes speed the list up rather than restarting it
    // from the finger's speed. Not for a slow drag (it means to slow down) or a long hold.
    if (carried != 0 && (velocity > 0) == (carried > 0) && std::abs(velocity) > std::abs(carried) * kFlywheelMinRatio &&
        drag_->eventTimeNs() - caughtNs_ <= kFlywheelWindowNs) {
        velocity += carried;
    }
    if (stretch_ != 0) {
        startSpringBack(0);
    } else if (std::abs(velocity) >= kMinFlingVelocity) {
        // From the lift, not the next frame: the list keeps moving through the frame after
        // the finger leaves rather than standing still for one. Not before the last frame,
        // which already showed where the finger had taken it.
        startFling(velocity, std::max(drag_->eventTimeNs(), frameClock().now()));
    }
}

void ScrollNode::startSpringBack(float velocity) {
    springing_ = true;
    springFrom_ = stretch_;
    springVelocity_ = velocity;
    springStartNs_ = -1;
    startTicking();
}

void ScrollNode::startTicking() {
    if (!ticking_) {
        ticking_ = true;
        frameClock().start(this);
    }
}

bool ScrollNode::tick(std::int64_t nowNs) {
    if (refit_) {
        refit_ = false;
        markLayoutDirty();
    }
    if (smooth_) {
        if (smooth_->startNs < 0) {
            smooth_->startNs = nowNs;
        }
        const float fraction =
                std::min(1.0f, static_cast<float>(nowNs - smooth_->startNs) / static_cast<float>(kSmoothScrollNs));
        const float position = smooth_->from + (smooth_->to - smooth_->from) * easing::kStandard(fraction);
        setOffset(std::clamp(position, 0.0f, maxOffset()));
        if (fraction >= 1) {
            smooth_.reset();
        }
    }
    if (flinging_) {
        if (flingStartNs_ < 0) {
            flingStartNs_ = nowNs;
        }
        const float seconds = static_cast<float>(nowNs - flingStartNs_) * 1e-9f;
        const float position = fling_->position(seconds);
        const float delta = position - flingLast_;
        flingLast_ = position;
        const float moved = scrollBy(delta);
        const bool atEdge = (delta > 0 && offset_ >= maxOffset()) || (delta < 0 && offset_ <= 0);
        if (atEdge && std::abs(moved) < std::abs(delta) - 1e-3f) {
            flinging_ = false;
            drag_->setAcceptOnDown(false);
            startSpringBack(fling_->velocity(seconds) * kAbsorbFactor);
        } else if (fling_->done(seconds)) {
            flinging_ = false;
            drag_->setAcceptOnDown(false);
        }
    }
    if (springing_) {
        if (springStartNs_ < 0) {
            springStartNs_ = nowNs;
        }
        const float seconds = static_cast<float>(nowNs - springStartNs_) * 1e-9f;
        const SpringSimulation spring(kStretchStiffness, kStretchDamping, springFrom_, springVelocity_);
        stretch_ = spring.position(seconds);
        if (std::abs(stretch_) < 0.05f && std::abs(spring.velocity(seconds)) < 1) {
            stretch_ = 0;
            springing_ = false;
        }
        invalidateComposite();
    }
    ticking_ = animating();
    return ticking_;
}

gfx::Size ScrollNode::measureCustom(LayoutEngine &engine, std::optional<float> width,
                                    std::optional<float> height, float availableWidth, float availableHeight) {
    gfx::Size size = Node::measureCustom(engine, width, height, availableWidth, availableHeight);
    const bool mainKnown = vertical() ? height.has_value() : width.has_value();
    const float availableMain = vertical() ? availableHeight : availableWidth;
    if (!mainKnown && (availableMain >= kInfinity || fitContent_)) {
        // Unbounded (inside another scroll along the same axis) or fitting: as long as the
        // content, but no longer than the room there is.
        // Measured as layoutContent() lays the children out.
        const Style &st = style();
        const Edges &pad = st.padding;
        const float cross = std::max(0.0f, vertical() ? size.width - pad.horizontal() : size.height - pad.vertical());
        float extent = vertical() ? pad.vertical() : pad.horizontal();
        bool any = false;
        for (const auto &child : children()) {
            if (!child->style().display) {
                continue;
            }
            const Edges &m = child->style().margin;
            if (vertical()) {
                const float w = std::max(0.0f, cross - m.horizontal());
                extent += m.vertical() + engine.compute(*child, w, std::nullopt, w, kInfinity, false).height + st.gap;
            } else {
                const float h = std::max(0.0f, cross - m.vertical());
                extent += m.horizontal() + engine.compute(*child, std::nullopt, h, kInfinity, h, false).width + st.gap;
            }
            any = true;
        }
        if (any) {
            extent -= st.gap;
        }
        (vertical() ? size.height : size.width) = std::min(extent, availableMain);
        fitAvailable_ = availableMain;
    }
    return size;
}

void ScrollNode::layoutCustom(LayoutEngine &engine, gfx::Size size) {
    density_ = engine.context().density;
    viewport_ = vertical() ? size.height : size.width;
    content_ = layoutContent(engine, size);
    // Fitting, but the content came out another length than it was measured at (it changed
    // in a way the measure cache didn't see): measure again on the next frame.
    if (fitContent_ && (content_ < viewport_ - 0.5f || (content_ > viewport_ + 0.5f && viewport_ < fitAvailable_ - 0.5f))) {
        refit_ = true;
        startTicking();
    }
    if (offset_ > maxOffset() && !flinging_ && !drag_->active()) {
        setOffset(maxOffset());
    }
    updateScrollSemantics();
}

gfx::Rect ScrollNode::revealRect(gfx::Rect rect) {
    const float start = vertical() ? rect.y : rect.x;
    const float extent = vertical() ? rect.height : rect.width;
    float target = smooth_ ? smooth_->to : offset_;
    if (start + extent > target + viewport_) {
        target = start + extent - viewport_;
    }
    if (start < target) {
        target = start;  // Longer than the viewport: its start wins.
    }
    target = std::clamp(target, 0.0f, maxOffset());
    if (target != (smooth_ ? smooth_->to : offset_)) {
        scrollTo(target, true);
    }
    // Where it will be once the scroll lands.
    (vertical() ? rect.y : rect.x) -= target;
    return rect;
}

float ScrollNode::layoutContent(LayoutEngine &engine, gfx::Size size) {
    const Style &st = style();
    const Edges &pad = st.padding;
    const float cross = std::max(0.0f, vertical() ? size.width - pad.horizontal() : size.height - pad.vertical());
    float cursor = vertical() ? pad.top : pad.left;
    bool any = false;
    for (const auto &childPtr : children()) {
        Node &child = *childPtr;
        if (!child.style().display) {
            continue;
        }
        const Edges &m = child.style().margin;
        if (vertical()) {
            const float width = std::max(0.0f, cross - m.horizontal());
            const gfx::Size s = engine.compute(child, width, std::nullopt, width, kInfinity, true);
            LayoutEngine::place(child, {pad.left + m.left, cursor + m.top, s.width, s.height});
            cursor += m.vertical() + s.height + st.gap;
        } else {
            const float height = std::max(0.0f, cross - m.vertical());
            const gfx::Size s = engine.compute(child, std::nullopt, height, kInfinity, height, true);
            LayoutEngine::place(child, {cursor + m.left, pad.top + m.top, s.width, s.height});
            cursor += m.horizontal() + s.width + st.gap;
        }
        any = true;
    }
    if (any) {
        cursor -= st.gap;
    }
    return cursor + (vertical() ? pad.bottom : pad.right);
}

gfx::Transform ScrollNode::childrenTransformPx(float density) const {
    // Whole pixels keep text crisp while scrolling.
    const float offsetPx = std::round(offset_ * density);
    gfx::Transform transform = vertical() ? gfx::Transform::translation({0, -offsetPx})
                                          : gfx::Transform::translation({-offsetPx, 0});
    if (stretch_ != 0 && viewport_ > 0) {
        const float scale = 1 + kStretchScale * std::min(1.0f, std::abs(stretch_) / viewport_);
        // Stretch away from the edge being pulled.
        const float anchor = stretch_ < 0 ? 0 : viewport_ * density;
        const gfx::Vec2 pivot = vertical() ? gfx::Vec2{0, anchor} : gfx::Vec2{anchor, 0};
        const gfx::Transform stretch = vertical() ? gfx::Transform::scale(1, scale) : gfx::Transform::scale(scale, 1);
        transform = gfx::Transform::translation(pivot) * stretch * gfx::Transform::translation(pivot * -1) * transform;
    }
    return transform;
}

void ScrollNode::updateScrollSemantics() {
    std::uint32_t actions = semantics().actions;
    const auto set = [&actions](Action action, bool on) {
        const auto bit = static_cast<std::uint32_t>(action);
        actions = on ? actions | bit : actions & ~bit;
    };
    set(Action::scrollForward, offset_ < maxOffset() - 0.5f);
    set(Action::scrollBackward, offset_ > 0.5f);
    if (actions != semantics().actions) {
        updateSemantics([actions](Semantics &s) { s.actions = actions; });
    }
}

bool ScrollNode::performDefaultAction(Action action, const ActionArgs &args) {
    switch (action) {
    case Action::scrollForward:
        scrollTo(offset_ + viewport_ * 0.8f, true);
        return true;
    case Action::scrollBackward:
        scrollTo(offset_ - viewport_ * 0.8f, true);
        return true;
    default:
        return Node::performDefaultAction(action, args);
    }
}

}  // namespace filepan::ui
