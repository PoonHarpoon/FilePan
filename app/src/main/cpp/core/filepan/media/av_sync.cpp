// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/media/av_sync.hpp"

#include <algorithm>

namespace filepan::media {

void MediaClock::reset(std::int64_t mediaUs) {
    playing_ = false;
    pausedUs_ = mediaUs;
    last_ = mediaUs;
    report_.reset();
    audioGone_ = false;
}

void MediaClock::play(std::int64_t nowNs) {
    if (playing_) {
        return;
    }
    playing_ = true;
    playNs_ = nowNs;
    anchor(pausedUs_, nowNs);
    report_.reset();  // The output restarts; wait for its first report.
}

void MediaClock::pause(std::int64_t nowNs) {
    if (!playing_) {
        return;
    }
    pausedUs_ = now(nowNs);
    playing_ = false;
    report_.reset();
}

void MediaClock::audioPosition(std::int64_t mediaUs, std::int64_t atNs) {
    if (playing_ && audioDriven()) {
        report_ = {mediaUs, atNs};
    }
}

void MediaClock::audioEnded(std::int64_t nowNs) {
    if (audioGone_) {
        return;
    }
    const std::int64_t at = now(nowNs);
    audioGone_ = true;
    anchor(at, nowNs);
}

std::int64_t MediaClock::now(std::int64_t nowNs) {
    if (!playing_) {
        return pausedUs_;
    }
    std::int64_t value;
    if (!audioDriven()) {
        value = monotonic(nowNs);
    } else if (report_) {
        const auto [mediaUs, atNs] = *report_;
        value = std::min(mediaUs + (nowNs - atNs) / 1000, mediaUs + kMaxExtrapolationUs);
    } else if (nowNs - playNs_ < kAudioStartTimeoutNs) {
        value = pausedUs_;  // The output is starting: hold.
    } else {
        // It never started: carry on without it.
        audioGone_ = true;
        anchor(last_, nowNs);
        value = last_;
    }
    last_ = std::max(last_, value);
    return last_;
}

std::int64_t audioMediaTime(const AudioMark &mark, std::int64_t framePosition, int sampleRate) {
    if (sampleRate <= 0) {
        return mark.mediaUs;
    }
    return mark.mediaUs + (framePosition - mark.streamFrame) * 1'000'000 / sampleRate;
}

FramePick pickFrame(std::span<const std::int64_t> queuePtsUs, std::int64_t clockUs, std::int64_t toleranceUs) {
    FramePick pick;
    const std::int64_t due = clockUs + toleranceUs;
    for (std::size_t i = 0; i < queuePtsUs.size() && queuePtsUs[i] <= due; ++i) {
        pick.show = i;
    }
    pick.drop = pick.show.value_or(0);
    return pick;
}

bool tooLateToRender(std::int64_t ptsUs, std::int64_t clockUs, bool firstAfterSeek) {
    return !firstAfterSeek && ptsUs < clockUs - kLateFrameUs;
}

}  // namespace filepan::media
