// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

// Audio/video sync, as plain logic with the clock passed in so tests can drive it. Media times
// are microseconds (container timestamps), clock times monotonic nanoseconds.
namespace filepan::media {

// The playback clock. With audio, audio is the master: the output reports which sample is
// heard when, and the clock runs on from the latest report; if audio stalls, the clock stops
// soon after so video waits for it. Without audio, or once it has ended or never started, the
// clock runs on the monotonic clock. While playing it never goes backwards.
class MediaClock {
public:
    // How far past the latest audio report the clock runs before holding.
    static constexpr std::int64_t kMaxExtrapolationUs = 200'000;
    // How long play waits for the first audio report before running without it.
    static constexpr std::int64_t kAudioStartTimeoutNs = 500'000'000;

    explicit MediaClock(bool audio = false) : audio_(audio) {}

    // Paused at mediaUs: after opening or seeking.
    void reset(std::int64_t mediaUs);
    void play(std::int64_t nowNs);
    void pause(std::int64_t nowNs);
    // The audio output: the sample at mediaUs is heard at atNs. Ignored while paused.
    void audioPosition(std::int64_t mediaUs, std::int64_t atNs);
    // Audio ran out (it's shorter than the video) or failed: monotonic from here on.
    void audioEnded(std::int64_t nowNs);

    // The media time at nowNs.
    std::int64_t now(std::int64_t nowNs);

    [[nodiscard]] bool playing() const { return playing_; }
    [[nodiscard]] bool audioDriven() const { return audio_ && !audioGone_; }

private:
    std::int64_t monotonic(std::int64_t nowNs) const { return anchorUs_ + (nowNs - anchorNs_) / 1000; }
    void anchor(std::int64_t mediaUs, std::int64_t nowNs) {
        anchorUs_ = mediaUs;
        anchorNs_ = nowNs;
    }

    bool audio_;
    bool audioGone_ = false;
    bool playing_ = false;
    std::int64_t pausedUs_ = 0;
    std::int64_t anchorUs_ = 0;
    std::int64_t anchorNs_ = 0;
    std::int64_t playNs_ = 0;
    std::optional<std::pair<std::int64_t, std::int64_t>> report_;  // (mediaUs, atNs).
    std::int64_t last_ = 0;
};

// Where the audio output is in media time. The output marks which media time it wrote at
// which stream frame; the stream reports which frame it was playing when. Between the two, time
// runs at the sample rate.
struct AudioMark {
    std::int64_t streamFrame = 0;
    std::int64_t mediaUs = 0;
};
std::int64_t audioMediaTime(const AudioMark &mark, std::int64_t framePosition, int sampleRate);

// Which decoded video frame to show. queue holds the waiting frames' presentation times,
// oldest first. The newest frame due by clockUs + toleranceUs (about half a vsync, so a frame
// shows on the vsync nearest its time) is shown; the ones before it are late and dropped; the
// ones after it wait.
struct FramePick {
    std::optional<std::size_t> show;
    std::size_t drop = 0;  // Frames at the front of the queue to drop.
};
FramePick pickFrame(std::span<const std::int64_t> queuePtsUs, std::int64_t clockUs, std::int64_t toleranceUs);

// Whether a decoded frame is so late it isn't worth handing to the GPU at all: the decoder
// drops it without rendering. Never the first frame after a seek (something must show).
bool tooLateToRender(std::int64_t ptsUs, std::int64_t clockUs, bool firstAfterSeek);
inline constexpr std::int64_t kLateFrameUs = 80'000;

}  // namespace filepan::media
