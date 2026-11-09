// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "filepan/gfx/display_list.hpp"

namespace filepan::gfx::vulkan {

// Images the renderer owns, such as thumbnails. Any thread can add and remove images; the render
// thread uploads them at the start of a frame, and frames that reference an image before it's
// uploaded skip it.
class ImageStore {
public:
    // pixels holds width * height non-premultiplied RGBA8 texels, tightly packed. Returns 0 if
    // the size is invalid.
    ImageId add(int width, int height, std::vector<std::uint8_t> pixels);
    // Replaces an image's pixels, e.g. with an animation's next frame; the size may change.
    // Frames recorded before the update keep drawing the old pixels until the GPU is done with
    // them. Updates queued before the renderer takes them coalesce, so only the newest is
    // uploaded. Ignores unknown and removed ids, and returns false if the size is invalid.
    bool update(ImageId id, int width, int height, std::vector<std::uint8_t> pixels);
    // Frees the image once no frame in flight uses it. Ignores unknown ids.
    void remove(ImageId id);

    struct Upload {
        ImageId id = 0;
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> pixels;
        bool replace = false;  // From update(): reuses the resident image if the size matches.
    };

    // The render thread's side: takes the queued work in the order it was queued.
    struct Pending {
        std::vector<Upload> uploads;
        std::vector<ImageId> removals;
    };
    Pending takePending();
    // Puts uploads that didn't fit this frame's budget back at the front of the queue.
    void requeue(std::vector<Upload> uploads);

private:
    std::atomic<ImageId> nextId_{1};
    std::mutex mutex_;
    Pending pending_;
};

}  // namespace filepan::gfx::vulkan
