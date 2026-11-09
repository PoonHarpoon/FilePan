// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <utility>
#include <variant>
#include <vector>

#include "filepan/gfx/geometry.hpp"
#include "filepan/ui/reactive.hpp"

// Animation: a frame clock, easing curves, a spring, and AnimatedValue.
//
// The FrameClock ticks its active tickers at the start of each frame, before effects run.
// AnimatedValue<T> exposes its current value as a signal, so binding a node property to it
// (node->bind([=] { node->setOpacity(fade()); })) animates that property with no other code.
// While any ticker is active the clock asks for frames; once all settle it stops, so an idle
// screen costs nothing.
namespace filepan::ui {

class FrameClock {
public:
    class Ticker {
    public:
        // Returns false once finished; it's then removed.
        virtual bool tick(std::int64_t nowNs) = 0;

    protected:
        ~Ticker() = default;
    };

    void start(Ticker *ticker);
    void stop(Ticker *ticker);
    [[nodiscard]] bool active() const { return !tickers_.empty(); }
    // Ticks every active ticker with the frame time.
    void tick(std::int64_t nowNs);
    // The time of the frame being produced, or of the last one.
    [[nodiscard]] std::int64_t now() const { return now_; }

    // Set from Environment::reducedMotion: animations snap instead of moving.
    bool reducedMotion = false;
    // Called when a ticker starts while the clock is idle (UiRoot requests a frame).
    std::function<void()> requestFrame;

private:
    std::vector<Ticker *> tickers_;
    std::int64_t now_ = 0;
    bool ticking_ = false;
};

// The UI thread's frame clock.
FrameClock &frameClock();

// cubic-bezier(x1, y1, x2, y2) as in CSS; y may leave [0, 1] to overshoot.
class CubicBezier {
public:
    constexpr CubicBezier(float x1, float y1, float x2, float y2) : x1_(x1), y1_(y1), x2_(x2), y2_(y2) {}

    [[nodiscard]] float operator()(float t) const;

private:
    float x1_, y1_, x2_, y2_;
};

namespace easing {
inline constexpr CubicBezier kLinear{0, 0, 1, 1};
inline constexpr CubicBezier kStandard{0.2f, 0, 0, 1};
inline constexpr CubicBezier kDecelerate{0, 0, 0, 1};
inline constexpr CubicBezier kAccelerate{0.3f, 0, 1, 1};
// Glass Signal's springy ease (switch knob): overshoots, then settles.
inline constexpr CubicBezier kSpringy{0.3f, 1.4f, 0.5f, 1};
}  // namespace easing

// A damped spring toward 0, unit mass; closed form, so it's exact at any frame time.
class SpringSimulation {
public:
    SpringSimulation(float stiffness, float dampingRatio, float position, float velocity);

    [[nodiscard]] float position(float seconds) const;
    [[nodiscard]] float velocity(float seconds) const;

private:
    float omega_;
    float zeta_;
    float x0_;
    float v0_;
};

struct Spring {
    // Compose's StiffnessMediumLow and no bounce: a quick, calm settle.
    float stiffness = 400;
    float dampingRatio = 1;
};

struct Tween {
    float durationMs = 250;
    CubicBezier easing = easing::kStandard;
    // Still runs under reduced motion. For the short crossfades that replace movement there;
    // anything that moves should snap instead.
    bool crossfade = false;
};

using AnimationSpec = std::variant<Spring, Tween>;

// How a type maps to floats so it can be animated component-wise.
template <typename T>
struct Animatable;

template <>
struct Animatable<float> {
    static constexpr std::size_t kSize = 1;
    static std::array<float, 1> to(float v) { return {v}; }
    static float from(const std::array<float, 1> &a) { return a[0]; }
};

template <>
struct Animatable<gfx::Vec2> {
    static constexpr std::size_t kSize = 2;
    static std::array<float, 2> to(gfx::Vec2 v) { return {v.x, v.y}; }
    static gfx::Vec2 from(const std::array<float, 2> &a) { return {a[0], a[1]}; }
};

template <>
struct Animatable<gfx::Color> {
    static constexpr std::size_t kSize = 4;
    static std::array<float, 4> to(gfx::Color c) { return {c.r, c.g, c.b, c.a}; }
    static gfx::Color from(const std::array<float, 4> &a) { return {a[0], a[1], a[2], a[3]}; }
};

template <>
struct Animatable<gfx::Rect> {
    static constexpr std::size_t kSize = 4;
    static std::array<float, 4> to(gfx::Rect r) { return {r.x, r.y, r.width, r.height}; }
    static gfx::Rect from(const std::array<float, 4> &a) { return {a[0], a[1], a[2], a[3]}; }
};

// A value that animates toward a target, readable as a signal. Not copyable: hold it in the
// component (a shared_ptr if lambdas need it).
template <typename T>
class AnimatedValue final : private FrameClock::Ticker {
public:
    explicit AnimatedValue(T initial, FrameClock &clock = frameClock())
        : clock_(clock), value_(initial), target_(initial) {}
    ~AnimatedValue() { clock_.stop(this); }
    AnimatedValue(const AnimatedValue &) = delete;
    AnimatedValue &operator=(const AnimatedValue &) = delete;

    // Tracked read of the current value.
    const T &get() const { return value_.get(); }
    const T &operator()() const { return get(); }
    [[nodiscard]] const T &peek() const { return value_.peek(); }
    [[nodiscard]] const T &target() const { return target_; }
    [[nodiscard]] bool animating() const { return running_; }
    [[nodiscard]] const Signal<T> &signal() const { return value_; }

    void snapTo(T value) {
        stopRunning();
        target_ = value;
        velocity_ = {};
        value_.set(std::move(value));
    }

    // Starts from the current value (and, for springs, velocity). Under reduced motion it snaps,
    // unless spec is a crossfade tween.
    void animateTo(T target, AnimationSpec spec = Spring{}, std::function<void()> onEnd = {}) {
        const auto *tween = std::get_if<Tween>(&spec);
        if (clock_.reducedMotion && (tween == nullptr || !tween->crossfade)) {
            snapTo(std::move(target));
            if (onEnd) {
                onEnd();
            }
            return;
        }
        target_ = target;
        spec_ = spec;
        onEnd_ = std::move(onEnd);
        from_ = Traits::to(value_.peek());
        to_ = Traits::to(target_);
        startNs_ = -1;
        // A spring keeps its momentum when retargeted mid-flight; a tween starts at rest.
        startVelocity_ = std::holds_alternative<Spring>(spec_) && running_ ? velocity_ : Array{};
        if (!running_) {
            running_ = true;
            clock_.start(this);
        }
    }

private:
    using Traits = Animatable<T>;
    using Array = std::array<float, Traits::kSize>;

    bool tick(std::int64_t nowNs) override {
        if (!running_) {
            return false;
        }
        if (startNs_ < 0) {
            startNs_ = nowNs;
        }
        const float seconds = static_cast<float>(nowNs - startNs_) * 1e-9f;
        Array next{};
        bool done = true;
        if (const auto *tween = std::get_if<Tween>(&spec_)) {
            const float duration = tween->durationMs / 1000;
            const float fraction = duration > 0 ? std::min(1.0f, seconds / duration) : 1;
            const float eased = tween->easing(fraction);
            for (std::size_t i = 0; i < Traits::kSize; ++i) {
                next[i] = from_[i] + (to_[i] - from_[i]) * eased;
            }
            done = fraction >= 1;
        } else {
            const auto &spring = std::get<Spring>(spec_);
            for (std::size_t i = 0; i < Traits::kSize; ++i) {
                const SpringSimulation simulation(spring.stiffness, spring.dampingRatio, from_[i] - to_[i],
                                                  startVelocity_[i]);
                const float offset = simulation.position(seconds);
                const float velocity = simulation.velocity(seconds);
                next[i] = to_[i] + offset;
                velocity_[i] = velocity;
                // Settled once within a thousandth of the distance (or a tiny absolute amount).
                const float threshold = std::max(1e-3f, std::abs(from_[i] - to_[i]) * 1e-3f);
                if (std::abs(offset) > threshold || std::abs(velocity) > threshold * 10) {
                    done = false;
                }
            }
        }
        if (done) {
            next = to_;
            velocity_ = {};
        }
        value_.set(Traits::from(next));
        if (done) {
            running_ = false;
            if (auto onEnd = std::move(onEnd_)) {
                onEnd_ = nullptr;
                onEnd();
            }
            return running_;  // onEnd may have started another animation.
        }
        return true;
    }

    void stopRunning() {
        if (running_) {
            running_ = false;
            clock_.stop(this);
        }
    }

    FrameClock &clock_;
    Signal<T> value_;
    T target_;
    AnimationSpec spec_ = Spring{};
    std::function<void()> onEnd_;
    Array from_{};
    Array to_{};
    Array velocity_{};
    Array startVelocity_{};
    std::int64_t startNs_ = -1;
    bool running_ = false;
};

}  // namespace filepan::ui
