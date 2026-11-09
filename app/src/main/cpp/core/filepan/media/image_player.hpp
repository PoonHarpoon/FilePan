// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "filepan/gfx/display_list.hpp"
#include "filepan/gfx/vulkan/image_store.hpp"
#include "filepan/media/frame_scheduler.hpp"
#include "filepan/media/image_source.hpp"
#include "filepan/ui/animation.hpp"
#include "filepan/ui/ui_loop.hpp"

// Shows one image file through the renderer's ImageStore, on the UI thread.
//
// Decoding happens on the I/O pool, one job at a time with the source handed from job to job.
// A still is decoded once. An animation keeps one frame decoded ahead; the frame clock ticks
// the FrameScheduler while it's visible, and when the next frame is due its pixels replace the
// image's (ImageStore::update) and the following one is decoded. Hidden, it pauses and
// decodes nothing.
namespace filepan::media {

class ImagePlayer final : private ui::FrameClock::Ticker {
public:
    enum class State : std::uint8_t { loading, ready, error };

    ImagePlayer(ui::UiLoop &loop, ui::IoPool &io, gfx::vulkan::ImageStore &images, ImageOpener opener = openImage);
    ~ImagePlayer();
    ImagePlayer(const ImagePlayer &) = delete;
    ImagePlayer &operator=(const ImagePlayer &) = delete;

    // Starts decoding source. Call once. Destroying the player cancels a streamed source.
    void open(const MediaSource &source, const DecodeLimits &limits);
    // Animations play only while visible. Starts out not visible. A streamed source is told it's
    // active while visible.
    void setVisible(bool visible);

    // Called on the UI thread when the state, the image or its pixels changed: repaint.
    void setOnChange(std::function<void()> onChange) { onChange_ = std::move(onChange); }

    [[nodiscard]] State state() const { return state_; }
    [[nodiscard]] const std::string &error() const { return error_; }
    // 0 until the first frame is decoded.
    [[nodiscard]] gfx::ImageId image() const { return image_; }
    [[nodiscard]] const ImageInfo &info() const { return info_; }
    [[nodiscard]] const FrameScheduler &scheduler() const { return scheduler_; }
    [[nodiscard]] bool decoding() const { return busy_; }

private:
    struct Decoded {
        MediaResult<std::optional<ImageFrame>> frame;
        bool looped = false;
    };

    bool tick(std::int64_t nowNs) override;
    void decodeNext();
    void updateTicking();
    void fail(std::string message);
    void changed() {
        if (onChange_) {
            onChange_();
        }
    }

    ui::UiLoop &loop_;
    ui::IoPool &io_;
    gfx::vulkan::ImageStore &images_;
    ImageOpener opener_;
    std::shared_ptr<ByteSource> bytes_;
    std::function<void()> onChange_;

    State state_ = State::loading;
    std::string error_;
    ImageInfo info_;
    gfx::ImageId image_ = 0;
    // Shared with the I/O job that's using it; only one is at a time.
    std::shared_ptr<ImageSource> source_;
    std::shared_ptr<ui::Cancellation> job_;
    bool busy_ = false;
    bool visible_ = false;
    bool ticking_ = false;
    int loopsLeft_ = 0;  // -1 forever.
    std::optional<ImageFrame> next_;
    FrameScheduler scheduler_;
};

}  // namespace filepan::media
