// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "filepan/gfx/display_list.hpp"
#include "filepan/gfx/vulkan/external_image_store.hpp"
#include "filepan/media/av_sync.hpp"
#include "filepan/media/byte_source.hpp"
#include "filepan/ui/animation.hpp"
#include "filepan/ui/ui_loop.hpp"

// Video playback for the viewer.
//
// VideoBackend is the decoding side, with threads of its own. On Android: AMediaExtractor
// feeds an AMediaCodec that decodes video into an AImageReader (AIMAGE_FORMAT_PRIVATE, GPU
// sampled), whose AHardwareBuffers go to the renderer as ExternalFrames, and a second codec
// decodes audio into a ring buffer an AAudio stream plays from. Elsewhere there's no backend.
//
// VideoPlayer runs on the UI thread. It opens, seeks and closes the backend on the I/O pool one
// operation at a time, and while it's visible and playing (or owes a frame after a seek) ticks
// on the frame clock: it reads the MediaClock (audio is the master when there is audio), picks
// the decoded frame due now, drops late ones and presents it to the ExternalImageStore. The
// renderer releases each frame's AImage once the GPU is done with it.
namespace filepan::media {

struct VideoInfo {
    int width = 0;  // The coded picture, before rotation.
    int height = 0;
    int rotation = 0;  // Clockwise degrees to show it at: 0, 90, 180 or 270.
    std::int64_t durationUs = 0;
    bool hasAudio = false;
};

// The decoder side. Every method may be called from any thread, but open, seek and close are
// called one at a time.
class VideoBackend {
public:
    virtual ~VideoBackend() = default;

    // Whether its frames are hardware buffers, which need ExternalImageStore::supported().
    [[nodiscard]] virtual bool needsHardwareBuffers() const { return true; }

    // Opens source and prepares the decoders (blocking). Decoding starts with seek. A streamed
    // source is read from the decoder threads, each extractor on its own.
    virtual MediaResult<VideoInfo> open(const MediaSource &source) = 0;
    // Drops what's decoded and decodes on from the sync frame at or before us (blocking, short).
    // Returns that frame's time.
    virtual MediaResult<std::int64_t> seek(std::int64_t us) = 0;
    // Releases the decoders, the audio stream and everything else; open may follow.
    virtual void close() = 0;

    // Starts or pauses the audio output.
    virtual void setPlaying(bool playing) = 0;
    // Silences the audio output; the audio clock keeps running.
    virtual void setMuted(bool muted) { (void)muted; }
    // The player's clock, so the decoder can skip frames too late to bother rendering.
    virtual void setClockHint(std::int64_t us) = 0;
    // Called from a decoder thread when a frame was decoded, or decoding ended.
    virtual void setOnFrame(std::function<void()> onFrame) = 0;

    // The decoded frames waiting, oldest first.
    virtual std::vector<std::int64_t> queuedFrames() = 0;
    // Drops the first drop waiting frames, then takes the next.
    virtual std::optional<gfx::vulkan::ExternalFrame> takeFrame(std::size_t drop) = 0;
    // Video: the last frame was decoded and taken.
    virtual bool videoEnded() = 0;

    // The audio output: the media time heard at a moment, if it's playing.
    virtual std::optional<std::pair<std::int64_t, std::int64_t>> audioPosition() = 0;
    // Audio ran out or failed (or there is none).
    virtual bool audioEnded() = 0;
};

// The platform's backend: Android's media NDK, or null where there's none.
std::shared_ptr<VideoBackend> createVideoBackend();

class VideoPlayer final : private ui::FrameClock::Ticker {
public:
    enum class State : std::uint8_t { opening, paused, playing, ended, error };

    // backend: null means video can't play here (state() goes to error). images must have
    // hardware buffer support for Android's backend; the caller checks.
    VideoPlayer(ui::UiLoop &loop, ui::IoPool &io, gfx::vulkan::ExternalImageStore &images,
                std::shared_ptr<VideoBackend> backend);
    ~VideoPlayer();
    VideoPlayer(const VideoPlayer &) = delete;
    VideoPlayer &operator=(const VideoPlayer &) = delete;

    // Opens source and shows its first frame, paused. Call once. A streamed source is told it's
    // active while visible or playing, and destroying the player cancels it.
    void open(const MediaSource &source);
    void play();
    void pause();
    void togglePlaying();
    void setMuted(bool muted);
    [[nodiscard]] bool muted() const { return muted_; }
    // Seeks to the sync frame at or before us; playing goes on playing.
    void seek(std::int64_t us);
    // Frames are only picked while visible; hiding pauses.
    void setVisible(bool visible);
    // The app went to the background: releases the decoders and the audio stream, keeping the
    // position. resume() reopens there, paused.
    void suspend();
    void resume();

    // Called on the UI thread when the state, position or frame changed: repaint.
    void setOnChange(std::function<void()> onChange) { onChange_ = std::move(onChange); }

    [[nodiscard]] State state() const { return state_; }
    [[nodiscard]] const std::string &error() const { return error_; }
    [[nodiscard]] const VideoInfo &info() const { return info_; }
    [[nodiscard]] std::int64_t positionUs() const { return position_; }
    [[nodiscard]] gfx::ExternalImageId image() const { return image_; }
    // A frame has been shown since opening.
    [[nodiscard]] bool hasFrame() const { return hasFrame_; }
    [[nodiscard]] bool suspended() const { return !wantOpen_; }
    // Playing, but stalled on a streamed source waiting for the network: show it's buffering.
    [[nodiscard]] bool waiting() const;
    // Frames dropped as late, for diagnostics and tests.
    [[nodiscard]] std::uint64_t droppedFrames() const { return dropped_; }
    [[nodiscard]] std::uint64_t shownFrames() const { return shown_; }

    // Tolerance for picking a frame: about half a vsync.
    static constexpr std::int64_t kPickToleranceUs = 8'000;

private:
    bool tick(std::int64_t nowNs) override;
    // Starts the next backend operation the wanted state calls for, if none is running.
    void drive();
    void updateTicking();
    // Tells a streamed source whether it's needed now.
    void updateActive();
    void fail(std::string message);
    void changed() {
        if (onChange_) {
            onChange_();
        }
    }

    ui::UiLoop &loop_;
    ui::IoPool &io_;
    gfx::vulkan::ExternalImageStore &images_;
    std::shared_ptr<VideoBackend> backend_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::function<void()> onChange_;

    MediaSource source_;
    bool requested_ = false;  // open was called.
    bool active_ = false;     // What the streamed source was last told.
    State state_ = State::opening;
    std::string error_;
    VideoInfo info_;
    gfx::ExternalImageId image_ = 0;
    MediaClock clock_;

    // What the backend should be doing, and what it is.
    bool wantOpen_ = false;
    bool opened_ = false;
    bool busy_ = false;
    std::optional<std::int64_t> pendingSeek_;
    bool seeking_ = false;
    bool wantPlaying_ = false;
    bool muted_ = false;

    bool visible_ = false;
    bool ticking_ = false;
    bool owesFrame_ = false;  // Show the first frame after a seek even while paused.
    bool hasFrame_ = false;
    std::int64_t position_ = 0;
    std::uint64_t dropped_ = 0;
    std::uint64_t shown_ = 0;
};

}  // namespace filepan::media
