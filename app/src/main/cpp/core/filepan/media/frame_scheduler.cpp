// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/media/frame_scheduler.hpp"

#include <algorithm>

namespace filepan::media {

void FrameScheduler::start(std::int64_t durationNs, std::int64_t nowNs) {
    started_ = true;
    paused_ = false;
    finished_ = false;
    next_.reset();
    deadline_ = nowNs + effectiveDuration(durationNs);
    shown_ = 1;
}

void FrameScheduler::nextReady(std::int64_t durationNs) { next_ = effectiveDuration(durationNs); }

void FrameScheduler::finish() {
    finished_ = true;
    next_.reset();
}

bool FrameScheduler::tick(std::int64_t nowNs) {
    if (!wantsTicks() || nowNs < deadline_) {
        return false;
    }
    // On time (give or take vsync): the frame starts at its deadline. Late (a slow decode, or
    // a stall): it starts now.
    const std::int64_t shownAt = nowNs - deadline_ < kResyncNs ? deadline_ : nowNs;
    deadline_ = shownAt + *next_;
    next_.reset();
    ++shown_;
    return true;
}

void FrameScheduler::pause(std::int64_t nowNs) {
    if (!started_ || paused_) {
        return;
    }
    paused_ = true;
    remaining_ = std::max<std::int64_t>(deadline_ - nowNs, 0);
}

void FrameScheduler::resume(std::int64_t nowNs) {
    if (!paused_) {
        return;
    }
    paused_ = false;
    deadline_ = nowNs + remaining_;
}

}  // namespace filepan::media
