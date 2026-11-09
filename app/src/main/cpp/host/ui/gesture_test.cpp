// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <cmath>
#include <memory>
#include <vector>

#include "filepan/ui/gesture.hpp"
#include "filepan/ui/scroll.hpp"
#include "ui_harness.hpp"

using namespace filepan;
using namespace filepan::ui;
using filepan::test::UiHarness;

namespace {

struct TapLog {
    int downs = 0;
    int taps = 0;
    int cancels = 0;
};

std::shared_ptr<TapRecognizer> tapInto(Node &node, TapLog &log) {
    auto tap = std::make_shared<TapRecognizer>();
    tap->onTapDown = [&log](gfx::Vec2) { ++log.downs; };
    tap->onTap = [&log] { ++log.taps; };
    tap->onTapCancel = [&log] { ++log.cancels; };
    node.addGestureRecognizer(tap);
    return tap;
}

struct LongPressLog {
    int starts = 0;
    int ends = 0;
    std::vector<gfx::Vec2> moves;
};

void longPressInto(Node &node, LongPressLog &log) {
    auto press = std::make_shared<LongPressRecognizer>();
    press->onLongPressStart = [&log](gfx::Vec2) { ++log.starts; };
    press->onLongPressMove = [&log](gfx::Vec2, gfx::Vec2 offset) { log.moves.push_back(offset); };
    press->onLongPressEnd = [&log](gfx::Vec2) { ++log.ends; };
    node.addGestureRecognizer(press);
}

// A full-screen vertical scroll with a 60dp row at the top of 5000dp of content.
struct ScrollScreen {
    std::shared_ptr<ScrollNode> scroll = ScrollNode::create();
    std::shared_ptr<Box> content = Box::create({.height = 5000});
    std::shared_ptr<Box> row = Box::create({.height = 60});

    explicit ScrollScreen(UiHarness &h) {
        content->appendChild(row);
        scroll->appendChild(content);
        h.root->setContent(scroll);
        h.frame();
    }
};

void testTap() {
    UiHarness h;
    auto button = Box::create({.width = 100, .height = 48});
    auto root = Box::create();
    root->appendChild(button);
    TapLog log;
    tapInto(*button, log);
    h.root->setContent(root);
    h.frame();

    h.down(50, 20);
    h.advance(40);
    h.up(52, 21);
    EXPECT(log.taps == 1 && log.downs == 1 && log.cancels == 0);

    // Up outside the slop: no tap.
    h.down(50, 20);
    h.move(50, 40);
    h.up(50, 40);
    EXPECT(log.taps == 1);
}

void testScrollStealsTap() {
    UiHarness h;
    ScrollScreen screen(h);
    TapLog log;
    tapInto(*screen.row, log);

    h.down(100, 30);
    h.advance(120);  // The press shows (tap down) after its delay.
    EXPECT(log.downs == 1);
    h.move(100, 26);  // Within the slop: nothing yet.
    EXPECT(screen.scroll->offset() == 0);
    h.move(100, 10);  // 20dp: the scroll wins; the tap is cancelled.
    EXPECT(log.cancels == 1);
    EXPECT(test::near(screen.scroll->offset(), 12));  // The slop isn't scrolled.
    h.move(100, -70);
    EXPECT(test::near(screen.scroll->offset(), 92));
    h.up(100, -70);
    EXPECT(log.taps == 0);
}

void testLongPressBeatsTap() {
    UiHarness h;
    ScrollScreen screen(h);
    TapLog tap;
    LongPressLog press;
    tapInto(*screen.row, tap);
    longPressInto(*screen.row, press);

    // A quick tap is a tap.
    h.down(100, 30);
    h.advance(50);
    h.up(100, 30);
    EXPECT(tap.taps == 1 && press.starts == 0);

    // Held: a long press; the tap's pressed state is cancelled.
    h.down(100, 30);
    h.advance(450);
    EXPECT(press.starts == 1 && tap.cancels == 1);
    // Then dragging selects instead of scrolling.
    h.move(100, 60);
    h.move(100, 130);
    EXPECT(press.moves.size() == 2 && test::near(press.moves.back().y, 200));  // px
    EXPECT(screen.scroll->offset() == 0);
    h.up(100, 130);
    EXPECT(press.ends == 1 && tap.taps == 1);
}

void testHorizontalVersusVertical() {
    UiHarness h;
    // A horizontal pager around a vertical list: the axis that passes the slop first wins.
    auto pager = Box::create();
    auto horizontal = std::make_shared<DragRecognizer>(DragAxis::horizontal);
    float pagerDelta = 0;
    horizontal->onUpdate = [&](gfx::Vec2 d) { pagerDelta += d.x; };
    pager->addGestureRecognizer(horizontal);
    auto scroll = ScrollNode::create();
    scroll->setStyle({.grow = 1});
    scroll->appendChild(Box::create({.height = 5000}));
    pager->appendChild(scroll);
    h.root->setContent(pager);
    h.frame();

    h.down(200, 400);
    h.move(230, 402);
    h.move(260, 404);
    h.up(260, 404);
    EXPECT(pagerDelta > 0 && scroll->offset() == 0);

    pagerDelta = 0;
    h.down(200, 400);
    h.move(202, 370);
    h.up(202, 370);
    EXPECT(pagerDelta == 0 && scroll->offset() > 0);
}

void testFlingAndCatch() {
    UiHarness h;
    ScrollScreen screen(h);
    TapLog log;
    tapInto(*screen.row, log);

    // Swipe up 300dp over ~50ms.
    h.down(100, 600);
    for (int i = 1; i <= 6; ++i) {
        h.now += 8'000'000;
        h.move(100, 600 - 50.0f * static_cast<float>(i));
    }
    h.up(100, 300);
    EXPECT(screen.scroll->flinging());
    float last = screen.scroll->offset();
    bool monotonic = true;
    for (int frame = 0; frame < 20; ++frame) {
        h.advance(8);
        monotonic = monotonic && screen.scroll->offset() >= last;
        last = screen.scroll->offset();
    }
    EXPECT(monotonic && last > 400);
    EXPECT(screen.scroll->flinging());

    // A touch during the fling catches it and doesn't tap what's under the finger.
    h.down(100, 30);
    EXPECT(!screen.scroll->flinging());
    const float caught = screen.scroll->offset();
    h.up(100, 30);
    h.advance(100);
    EXPECT(log.taps == 0 && screen.scroll->offset() == caught);
    EXPECT(!h.root->needsFrame());
}

void testOverscrollSpringsBack() {
    UiHarness h;
    ScrollScreen screen(h);
    h.down(100, 100);
    h.move(100, 200);
    h.move(100, 300);
    EXPECT(screen.scroll->offset() == 0 && screen.scroll->overscroll() < -10);
    // Moving back undoes the stretch before scrolling.
    h.move(100, 290);
    EXPECT(screen.scroll->offset() == 0);
    h.up(100, 290);
    h.advance(1000);
    EXPECT(screen.scroll->overscroll() == 0);
    EXPECT(!h.root->needsFrame());
}

void testVelocityTracker() {
    VelocityTracker tracker;
    for (int i = 0; i <= 10; ++i) {
        const auto t = static_cast<std::int64_t>(i) * 8'000'000;
        tracker.add(t, {1000.0f * static_cast<float>(i) * 0.008f, 0});
    }
    EXPECT(std::abs(tracker.velocity().x - 1000) < 1);
    // Decelerating motion: the velocity at the end, not the average.
    VelocityTracker slowing;
    for (int i = 0; i <= 10; ++i) {
        const float t = static_cast<float>(i) * 0.008f;
        slowing.add(static_cast<std::int64_t>(i) * 8'000'000, {0, 2000 * t - 5000 * t * t});
    }
    EXPECT(std::abs(slowing.velocity().y - (2000 - 10000 * 0.08f)) < 5);
    // A pause before lifting means no fling.
    tracker.add(200'000'000, {80, 0});
    EXPECT(tracker.velocity().x == 0);
}

void testFlingMonotonic() {
    for (float velocity : {300.0f, 2500.0f, 8000.0f, -4000.0f}) {
        const FlingSimulation fling(100, velocity);
        const float sign = velocity > 0 ? 1.0f : -1.0f;
        float lastPosition = 100;
        float lastSpeed = std::abs(velocity) * 10;
        for (float t = 0; t <= fling.duration() + 0.05f; t += 1.0f / 120) {
            const float position = fling.position(t);
            const float speed = std::abs(fling.velocity(t));
            EXPECT((position - lastPosition) * sign >= -1e-3f);
            EXPECT(speed <= lastSpeed + 1e-2f);
            lastPosition = position;
            lastSpeed = speed;
        }
        EXPECT(test::near(fling.position(fling.duration()), fling.finalPosition()));
        EXPECT(fling.velocity(fling.duration()) == 0);
        EXPECT(fling.duration() > 0.2f && fling.duration() < 5);
    }
    // Faster flings go further.
    EXPECT(FlingSimulation(0, 4000).finalPosition() > FlingSimulation(0, 2000).finalPosition());
}

// --- Android-like touch input ---
//
// The panel samples at 240 Hz. Android hands the app every sample since the last frame in one
// batch (historical samples, each with its own event time) shortly before the frame, and the
// app draws at 120 Hz unless a frame is dropped.

constexpr std::int64_t kMs = 1'000'000;
constexpr std::int64_t kSampleNs = 1'000'000'000 / 240;

struct Sample {
    std::int64_t timeNs;
    PointerAction action;
    float y;  // dp
};

// A vertical swipe that speeds up until it lifts, as a flick does: y(t) = from - distance (t/T)^2,
// lifting at speed 2 distance / T. Starts at startNs; a pause (no samples: a resting finger
// reports nothing) before the up if holdMs > 0.
std::vector<Sample> swipe(std::int64_t startNs, float from, float distance, std::int64_t durationMs,
                          std::int64_t holdMs = 0) {
    std::vector<Sample> samples;
    samples.push_back({startNs, PointerAction::down, from});
    const std::int64_t duration = durationMs * kMs;
    std::int64_t t = kSampleNs;
    for (; t <= duration; t += kSampleNs) {
        const float f = static_cast<float>(t) / static_cast<float>(duration);
        samples.push_back({startNs + t, PointerAction::move, from - distance * f * f});
    }
    const float last = samples.back().y;
    samples.push_back({startNs + t - kSampleNs + holdMs * kMs + 2 * kMs, PointerAction::up, last});
    return samples;
}

// Plays samples and frames until untilNs: before each frame, every sample due by then is
// dispatched with its own time. frameGap gives the time to the next frame (8 ms at 120 Hz;
// more when frames drop).
class Player {
public:
    explicit Player(UiHarness &h) : h_(h) {}

    std::function<std::int64_t(int frame)> frameGap = [](int) { return 8 * kMs + kMs / 3; };
    // Called after every frame, with the offset the last frame showed and this one's.
    std::function<void(float before, float after)> onFrame;

    void play(std::vector<Sample> samples, std::int64_t untilNs) {
        std::size_t next = 0;
        while (h_.now < untilNs) {
            const std::int64_t frameNs = h_.now + frameGap(frames_++);
            const float before = scroll->offset();
            for (; next < samples.size() && samples[next].timeNs <= frameNs; ++next) {
                h_.root->dispatchPointer({.pointer = 0,
                                          .action = samples[next].action,
                                          .position = {200 * UiHarness::kDensity, samples[next].y * UiHarness::kDensity},
                                          .timeNs = samples[next].timeNs});
            }
            h_.now = frameNs;
            h_.loop.runPending();
            h_.frame();
            if (onFrame) {
                onFrame(before, scroll->offset());
            }
        }
    }

    std::shared_ptr<ScrollNode> scroll;

private:
    UiHarness &h_;
    int frames_ = 0;
};

struct LongScroll {
    std::shared_ptr<ScrollNode> scroll = ScrollNode::create();
    explicit LongScroll(UiHarness &h, float offset = 500'000) {
        scroll->appendChild(Box::create({.height = 1'000'000}));
        h.root->setContent(scroll);
        h.frame();
        scroll->scrollTo(offset);
        h.frame();
    }
};

// Far down a long list (a few dozen rows is enough), a swipe still flings: float rounding of
// the offset isn't mistaken for a pull past the end.
void testFlingFarDownTheList() {
    for (const float offset : {3'000.0f, 30'000.0f, 300'000.0f}) {
        UiHarness h;
        LongScroll screen(h, offset);
        Player player(h);
        player.scroll = screen.scroll;
        bool stretched = false;
        player.onFrame = [&](float, float) { stretched = stretched || screen.scroll->overscroll() != 0; };
        const std::int64_t t = h.now + 20 * kMs;
        player.play(swipe(t, 700, 160, 80), t + 100 * kMs);
        EXPECT(!stretched);
        EXPECT(screen.scroll->flinging() && screen.scroll->flingVelocity() > 3000);
    }
}

// Fast repeated swipes, each starting while the last fling is still moving: every one keeps the
// list going at least as fast as the swipe itself, same-direction swipes build on the fling
// they caught (up to the limit), and the list never stalls between frames.
void testRepeatedFlings() {
    UiHarness h;
    LongScroll screen(h);
    Player player(h);
    player.scroll = screen.scroll;
    float slowest = 1e9f;
    bool tracking = false;
    player.onFrame = [&](float before, float after) {
        if (tracking) {
            slowest = std::min(slowest, after - before);
        }
    };

    // 160dp in 80ms: 4000 dp/s at the lift.
    constexpr float kSwipeVelocity = 4000;
    std::int64_t t = h.now + 20 * kMs;
    player.play(swipe(t, 700, 160, 80), t + 100 * kMs);
    EXPECT(screen.scroll->flinging());
    const float first = screen.scroll->flingVelocity();
    EXPECT(first > kSwipeVelocity * 0.85f && first < kSwipeVelocity * 1.15f);

    float previous = first;
    for (const std::int64_t gapMs : {150, 250}) {
        // Caught while moving fast; the new swipe is the same flick again.
        t = h.now + gapMs * kMs;
        player.play({}, t);
        EXPECT(screen.scroll->flinging());
        const float caught = screen.scroll->flingVelocity();
        EXPECT(caught > 1000);
        tracking = true;
        slowest = 1e9f;
        player.play(swipe(t, 700, 160, 80), t + 100 * kMs);
        tracking = false;
        EXPECT(screen.scroll->flinging());
        const float boosted = screen.scroll->flingVelocity();
        // Faster than the swipe alone, faster than it was; never past the limit.
        EXPECT(boosted > kSwipeVelocity * 0.85f + caught * 0.5f);
        EXPECT(boosted > previous * 0.9f);
        EXPECT(boosted <= 8000);
        // Every frame from the catch through the release moved the list on: a stall would be
        // a frame with nothing (or a step back).
        EXPECT(slowest > 2);
        previous = boosted;
    }

    // Against the fling: no boost, the swipe's own speed the other way.
    t = h.now + 150 * kMs;
    player.play({}, t);
    player.play(swipe(t, 300, -160, 80), t + 100 * kMs);
    EXPECT(screen.scroll->flinging());
    const float reversed = screen.scroll->flingVelocity();
    EXPECT(reversed < -kSwipeVelocity * 0.85f && reversed > -kSwipeVelocity * 1.15f);

    // Caught and held before swiping on: a fresh start at the swipe's own speed.
    t = h.now + 150 * kMs;
    player.play({}, t);
    EXPECT(screen.scroll->flinging());
    auto held = swipe(t + 400 * kMs, 300, -160, 80);
    held.front().timeNs = t;
    player.play(held, t + 500 * kMs);
    const float fresh = screen.scroll->flingVelocity();
    EXPECT(fresh < -kSwipeVelocity * 0.85f && fresh > -kSwipeVelocity * 1.15f);

    // A slow drag on the way catches it and slows it down.
    t = h.now + 100 * kMs;
    player.play({}, t);
    player.play(swipe(t, 300, -40, 80), t + 100 * kMs);
    EXPECT(screen.scroll->flingVelocity() > -kSwipeVelocity * 0.5f);
}

// The first frame after the finger lifts carries the fling on; it doesn't wait a frame.
void testReleaseDoesNotStall() {
    UiHarness h;
    LongScroll screen(h);
    Player player(h);
    player.scroll = screen.scroll;
    std::vector<float> steps;
    player.onFrame = [&](float before, float after) { steps.push_back(after - before); };
    const std::int64_t t = h.now + 20 * kMs;
    player.play(swipe(t, 700, 160, 80), t + 150 * kMs);
    // From about 30ms into the swipe on, every frame moves at least a few dp.
    for (std::size_t i = 6; i < steps.size(); ++i) {
        EXPECT(steps[i] > 5);
    }
}

// A finger that stops before lifting doesn't fling, though its last samples were fast.
void testPauseBeforeLift() {
    UiHarness h;
    LongScroll screen(h);
    Player player(h);
    player.scroll = screen.scroll;
    const std::int64_t t = h.now + 20 * kMs;
    player.play(swipe(t, 700, 160, 80, 60), t + 200 * kMs);
    EXPECT(!screen.scroll->flinging());
    // A short rest (under Android's 40ms) still flings.
    const std::int64_t t2 = h.now + 20 * kMs;
    player.play(swipe(t2, 700, 160, 80, 20), t2 + 150 * kMs);
    EXPECT(screen.scroll->flinging() && screen.scroll->flingVelocity() > 2000);
}

// Dropped frames (a heavy frame binding rows, uploading thumbnails) don't slow the fling: it
// follows the clock, landing where it would at 120 Hz.
void testFlingSurvivesDroppedFrames() {
    const auto run = [](bool drops) {
        UiHarness h;
        LongScroll screen(h);
        Player player(h);
        player.scroll = screen.scroll;
        if (drops) {
            player.frameGap = [](int frame) { return (frame % 5 == 4 ? 60 : 8) * kMs; };
        } else {
            player.frameGap = [](int) { return 4 * kMs; };
        }
        const std::int64_t t = h.now + 20 * kMs;
        const std::int64_t start = h.now;
        player.play(swipe(t, 700, 160, 80), t + 400 * kMs);
        // Both end at the same moment, near enough.
        return std::pair{screen.scroll->offset(), h.now - start};
    };
    const auto [smooth, smoothNs] = run(false);
    const auto [dropped, droppedNs] = run(true);
    EXPECT(smooth > 500'000 + 600);
    // Off by at most the time either overran the end, at the fling's speed by then.
    const float slack = 3000.0f * static_cast<float>(std::abs(droppedNs - smoothNs)) * 1e-9f + 30;
    EXPECT(std::abs(dropped - smooth) < slack);
}

}  // namespace

int main() {
    testTap();
    testScrollStealsTap();
    testLongPressBeatsTap();
    testHorizontalVersusVertical();
    testFlingAndCatch();
    testOverscrollSpringsBack();
    testVelocityTracker();
    testFlingMonotonic();
    testFlingFarDownTheList();
    testRepeatedFlings();
    testReleaseDoesNotStall();
    testPauseBeforeLift();
    testFlingSurvivesDroppedFrames();
    return 0;
}
