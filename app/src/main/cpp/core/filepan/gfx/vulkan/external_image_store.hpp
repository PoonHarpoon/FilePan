// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

#include "filepan/gfx/display_list.hpp"
#include "filepan/gfx/geometry.hpp"

struct AHardwareBuffer;

namespace filepan::gfx::vulkan {

// A frame produced outside the renderer, such as a video decoder's image: a hardware buffer
// the renderer imports and samples (through a YCbCr conversion for video formats), or, for
// tests and hosts without hardware buffers, plain RGBA pixels it uploads.
//
// Owns the producer's buffer until it's released: release runs once, when the frame is reset,
// replaced or destroyed. The renderer holds a presented frame until the GPU is done with every
// frame that sampled it, so release usually runs on the render thread.
class ExternalFrame {
public:
    ExternalFrame() = default;
    // buffer must stay valid until release runs. crop is the area to show, in buffer px; an
    // empty crop shows all of it.
    ExternalFrame(AHardwareBuffer *buffer, Rect crop, std::function<void()> release)
        : buffer_(buffer), crop_(crop), release_(std::move(release)) {}
    // Non-premultiplied RGBA8, tightly packed.
    static ExternalFrame fromPixels(int width, int height, std::vector<std::uint8_t> pixels, Rect crop = {},
                                    std::function<void()> release = {});
    ~ExternalFrame() { reset(); }
    ExternalFrame(ExternalFrame &&other) noexcept { *this = std::move(other); }
    ExternalFrame &operator=(ExternalFrame &&other) noexcept;
    ExternalFrame(const ExternalFrame &) = delete;
    ExternalFrame &operator=(const ExternalFrame &) = delete;

    // Releases the buffer now.
    void reset();

    [[nodiscard]] bool empty() const { return buffer_ == nullptr && !pixels_; }
    [[nodiscard]] AHardwareBuffer *buffer() const { return buffer_; }
    [[nodiscard]] const Rect &crop() const { return crop_; }
    [[nodiscard]] const std::shared_ptr<const std::vector<std::uint8_t>> &pixels() const { return pixels_; }
    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }

private:
    AHardwareBuffer *buffer_ = nullptr;
    std::shared_ptr<const std::vector<std::uint8_t>> pixels_;
    int width_ = 0;
    int height_ = 0;
    Rect crop_;
    std::function<void()> release_;
};

// Images whose content comes from outside the renderer, a frame at a time: video. Any thread
// can create, present and destroy; the render thread takes the work at the start of a frame.
// DisplayList::externalImage draws the newest frame presented before that render; until one
// is, nothing is drawn.
class ExternalImageStore {
public:
    // Whether hardware buffers can be drawn: Android with VK_ANDROID_external_memory_
    // android_hardware_buffer and sampler YCbCr conversion. Without it, hardware-buffer frames
    // are released without being drawn; pixel frames always work.
    [[nodiscard]] bool supported() const { return supported_.load(std::memory_order_acquire); }

    ExternalImageId create();
    // Shows frame for id from the next render on. The frame it replaces is released once the GPU
    // is done with it, or right away if it was never drawn. A frame for an unknown or destroyed
    // id is released at once.
    void present(ExternalImageId id, ExternalFrame frame);
    // Drops id, releasing its frame once no frame in flight uses it. Ignores unknown ids.
    void destroy(ExternalImageId id);

    // The render thread's side, in the order it was queued. A null frame is a destroy.
    struct Op {
        ExternalImageId id = 0;
        std::optional<ExternalFrame> frame;
    };
    std::vector<Op> takePending();
    void setSupported(bool supported) { supported_.store(supported, std::memory_order_release); }

private:
    std::atomic<bool> supported_{false};
    std::mutex mutex_;
    ExternalImageId nextId_ = 1;
    std::unordered_set<ExternalImageId> live_;
    std::vector<Op> pending_;
};

}  // namespace filepan::gfx::vulkan
