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

#include "filepan/media/byte_source.hpp"

// Full-size image decoding for the viewer: stills and animations (GIF, animated WebP, AVIF).
//
// On Android this is AImageDecoder, which applies EXIF orientation itself (the sizes it reports
// are already oriented), converts to sRGB, and composites animation frames over the previous
// ones as their blend and dispose ops say, so long as each frame is decoded into the same
// buffer. A host build has no decoder: openImage fails, and tests supply their own ImageSource.
namespace filepan::media {

// How large a decode may get, in px per side.
struct DecodeLimits {
    // Stills: about twice the screen's long side, so zooming in stays sharp, but no more than
    // the GPU's texture limit.
    int maxDimension = 4096;
    // Animations upload every frame whole, so they stay smaller.
    int maxAnimatedDimension = 2048;
};

// Limits for a screen of this size (px): twice its long side, capped at maxTexture (the GPU's
// limit, or 4096 when it's unknown).
DecodeLimits decodeLimitsFor(int screenWidth, int screenHeight, int maxTexture = 4096);

// width x height scaled down to fit maxDimension on its longer side, keeping the aspect ratio;
// never scaled up, and never below 1 px.
std::pair<int, int> fitWithin(int width, int height, int maxDimension);

struct ImageInfo {
    int width = 0;  // The full image, oriented.
    int height = 0;
    int decodedWidth = 0;  // What the frames are.
    int decodedHeight = 0;
    bool animated = false;
    // Animations: how many times to play it again after the first time; -1 forever.
    int repeatCount = -1;
};

struct ImageFrame {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;  // Non-premultiplied RGBA8, tightly packed.
    std::int64_t durationNs = 0;     // Animations: how long it's shown. 0 for stills.
};

// One image file, frame by frame. Used from one thread at a time (each I/O job hands it on).
class ImageSource {
public:
    virtual ~ImageSource() = default;
    [[nodiscard]] virtual const ImageInfo &info() const = 0;
    // The first frame, then each next one of an animation; nullopt past the last.
    virtual MediaResult<std::optional<ImageFrame>> next() = 0;
    // Back to before the first frame, to loop.
    virtual MediaResult<void> rewind() = 0;
};

using ImageOpener = std::function<MediaResult<std::unique_ptr<ImageSource>>(const MediaSource &source,
                                                                           const DecodeLimits &limits)>;

// The largest streamed image openImage reads into memory.
inline constexpr std::uint64_t kMaxStreamedImageBytes = 256ull << 20;

// AImageDecoder on Android; fails on a host. A streamed source is read whole into memory first,
// and the decoder works from that buffer (it reads it again to rewind and advance animations).
MediaResult<std::unique_ptr<ImageSource>> openImage(const MediaSource &source, const DecodeLimits &limits);

}  // namespace filepan::media
