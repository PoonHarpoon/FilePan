// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <optional>

// Times an animated image's frames (GIF, animated WebP, AVIF) on the UI thread's frame clock.
// Plain logic with the clock passed in, so tests can drive it.
//
// The decoder runs a frame ahead: the frame on screen has a deadline, and the next one waits,
// decoded, until it's due. A frame decoded too late to make its deadline shows as soon as it's
// ready, and the schedule restarts from then rather than rushing to catch up.
namespace filepan::media {

class FrameScheduler {
public:
    // Frames this short are a GIF quirk (0 and 10 ms are common); browsers show them for
    // 100 ms, and so does this.
    static constexpr std::int64_t kMinFrameNs = 20'000'000;
    static constexpr std::int64_t kShortFrameNs = 100'000'000;
    // A tick less late than this keeps to the schedule: frame times stay exact on average
    // even though they snap to vsync.
    static constexpr std::int64_t kResyncNs = 50'000'000;

    [[nodiscard]] static std::int64_t effectiveDuration(std::int64_t durationNs) {
        return durationNs < kMinFrameNs ? kShortFrameNs : durationNs;
    }

    // The first frame went on screen at nowNs and lasts durationNs.
    void start(std::int64_t durationNs, std::int64_t nowNs);
    // The next frame is decoded; it lasts durationNs once shown.
    void nextReady(std::int64_t durationNs);
    // The animation has no more frames: the current one stays.
    void finish();

    // Whether the waiting frame is due at nowNs. If so, it's on screen from now on.
    bool tick(std::int64_t nowNs);

    void pause(std::int64_t nowNs);
    void resume(std::int64_t nowNs);

    [[nodiscard]] bool started() const { return started_; }
    [[nodiscard]] bool paused() const { return paused_; }
    [[nodiscard]] bool finished() const { return finished_; }
    [[nodiscard]] bool hasNext() const { return next_.has_value(); }
    // Whether ticking can do anything: the clock must run until the next frame is shown.
    [[nodiscard]] bool wantsTicks() const { return started_ && !paused_ && !finished_ && next_.has_value(); }
    // When the current frame's time is up.
    [[nodiscard]] std::int64_t deadline() const { return deadline_; }
    // How many frames have been shown, the first included.
    [[nodiscard]] std::uint64_t shown() const { return shown_; }

private:
    bool started_ = false;
    bool paused_ = false;
    bool finished_ = false;
    std::int64_t deadline_ = 0;
    std::int64_t remaining_ = 0;  // While paused: what was left of the current frame.
    std::optional<std::int64_t> next_;
    std::uint64_t shown_ = 0;
};

}  // namespace filepan::media
