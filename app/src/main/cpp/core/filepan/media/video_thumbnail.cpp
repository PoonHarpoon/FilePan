// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// The pure part of video thumbnails; the Android frame grab is in
// video_thumbnail_android.cpp.

#include "filepan/media/video_thumbnail.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace filepan::media {

YuvColor yuvColorFor(std::int32_t standard, std::int32_t range, int width, int height) {
    YuvColor color;
    switch (standard) {
    case kColorStandardBt709:
        color.matrix = YuvMatrix::bt709;
        break;
    case kColorStandardBt601Pal:
    case kColorStandardBt601Ntsc:
        color.matrix = YuvMatrix::bt601;
        break;
    case kColorStandardBt2020:
        color.matrix = YuvMatrix::bt2020;
        break;
    default:
        color.matrix = width >= 1280 || height >= 720 ? YuvMatrix::bt709 : YuvMatrix::bt601;
        break;
    }
    color.fullRange = range == kColorRangeFull;
    return color;
}

namespace {

struct Coefficients {
    float kr;
    float kb;
};

constexpr Coefficients coefficients(YuvMatrix matrix) {
    switch (matrix) {
    case YuvMatrix::bt709:
        return {0.2126f, 0.0722f};
    case YuvMatrix::bt2020:
        return {0.2627f, 0.0593f};
    case YuvMatrix::bt601:
        break;
    }
    return {0.299f, 0.114f};
}

std::uint8_t toByte(float value) { return static_cast<std::uint8_t>(std::clamp(value * 255.0f + 0.5f, 0.0f, 255.0f)); }

}  // namespace

std::array<std::uint8_t, 3> yuvToRgb(float y, float u, float v, YuvColor color) {
    float luma = 0;
    float cb = 0;
    float cr = 0;
    if (color.fullRange) {
        luma = y / 255.0f;
        cb = (u - 128.0f) / 255.0f;
        cr = (v - 128.0f) / 255.0f;
    } else {
        luma = (y - 16.0f) / 219.0f;
        cb = (u - 128.0f) / 224.0f;
        cr = (v - 128.0f) / 224.0f;
    }
    const auto [kr, kb] = coefficients(color.matrix);
    const float kg = 1.0f - kr - kb;
    const float r = luma + 2.0f * (1.0f - kr) * cr;
    const float b = luma + 2.0f * (1.0f - kb) * cb;
    const float g = (luma - kr * r - kb * b) / kg;
    return {toByte(r), toByte(g), toByte(b)};
}

int normalizeRotation(int degrees) { return ((degrees % 360) + 360) % 360 / 90 * 90; }

ThumbnailLayout thumbnailLayout(int width, int height, int rotation, int sizePx) {
    ThumbnailLayout layout;
    if (width <= 0 || height <= 0 || sizePx <= 0) {
        return layout;
    }
    const bool sideways = normalizeRotation(rotation) % 180 != 0;
    layout.displayWidth = sideways ? height : width;
    layout.displayHeight = sideways ? width : height;
    // The same rounding as the image thumbnails (thumbnails.cpp).
    layout.scale = std::min(1.0f, static_cast<float>(sizePx) /
                                          static_cast<float>(std::min(layout.displayWidth, layout.displayHeight)));
    layout.scaledWidth = std::max(1, static_cast<int>(static_cast<float>(layout.displayWidth) * layout.scale + 0.5f));
    layout.scaledHeight =
            std::max(1, static_cast<int>(static_cast<float>(layout.displayHeight) * layout.scale + 0.5f));
    layout.side = std::min(layout.scaledWidth, layout.scaledHeight);
    layout.offsetX = (layout.scaledWidth - layout.side) / 2;
    layout.offsetY = (layout.scaledHeight - layout.side) / 2;
    return layout;
}

SourcePoint displayToSource(float x, float y, int width, int height, int rotation) {
    const auto w = static_cast<float>(width);
    const auto h = static_cast<float>(height);
    switch (normalizeRotation(rotation)) {
    case 90:  // The source's top edge is the display's right edge.
        return {y, h - x};
    case 180:
        return {w - x, h - y};
    case 270:  // The source's top edge is the display's left edge.
        return {w - y, x};
    default:
        return {x, y};
    }
}

std::optional<RgbaImage> renderVideoThumbnail(const YuvFrame &frame, YuvColor color, int rotation, int sizePx) {
    if (frame.width <= 0 || frame.height <= 0 || frame.y.data == nullptr || frame.u.data == nullptr ||
        frame.v.data == nullptr) {
        return std::nullopt;
    }
    int left = std::clamp(frame.cropLeft, 0, frame.width);
    int top = std::clamp(frame.cropTop, 0, frame.height);
    int right = std::clamp(frame.cropRight, 0, frame.width);
    int bottom = std::clamp(frame.cropBottom, 0, frame.height);
    if (right <= left || bottom <= top) {
        left = 0;
        top = 0;
        right = frame.width;
        bottom = frame.height;
    }
    const int width = right - left;
    const int height = bottom - top;
    const ThumbnailLayout layout = thumbnailLayout(width, height, rotation, sizePx);
    if (layout.side <= 0) {
        return std::nullopt;
    }
    // Scaled pixels per displayed pixel, per axis (rounding makes them differ slightly).
    const float scaleX = static_cast<float>(layout.scaledWidth) / static_cast<float>(layout.displayWidth);
    const float scaleY = static_cast<float>(layout.scaledHeight) / static_cast<float>(layout.displayHeight);
    // A box filter: up to 4 x 4 samples per output pixel, enough when shrinking a 4K frame.
    const int samplesX = std::clamp(static_cast<int>(std::ceil(1.0f / scaleX)), 1, 4);
    const int samplesY = std::clamp(static_cast<int>(std::ceil(1.0f / scaleY)), 1, 4);
    const int chromaWidth = (frame.width + 1) / 2;
    const int chromaHeight = (frame.height + 1) / 2;
    auto at = [](const YuvPlane &plane, int x, int y) {
        return plane.data[static_cast<std::ptrdiff_t>(y) * plane.rowStride +
                          static_cast<std::ptrdiff_t>(x) * plane.pixelStride];
    };

    RgbaImage image{layout.side, layout.side, {}};
    image.rgba.resize(static_cast<std::size_t>(layout.side) * static_cast<std::size_t>(layout.side) * 4);
    std::uint8_t *out = image.rgba.data();
    const float samples = static_cast<float>(samplesX * samplesY);
    for (int oy = 0; oy < layout.side; ++oy) {
        for (int ox = 0; ox < layout.side; ++ox) {
            int ySum = 0;
            int uSum = 0;
            int vSum = 0;
            for (int j = 0; j < samplesY; ++j) {
                const float dy = (static_cast<float>(oy + layout.offsetY) +
                                  (static_cast<float>(j) + 0.5f) / static_cast<float>(samplesY)) /
                                 scaleY;
                for (int i = 0; i < samplesX; ++i) {
                    const float dx = (static_cast<float>(ox + layout.offsetX) +
                                      (static_cast<float>(i) + 0.5f) / static_cast<float>(samplesX)) /
                                     scaleX;
                    const SourcePoint source = displayToSource(dx, dy, width, height, rotation);
                    const int sx = left + std::clamp(static_cast<int>(std::floor(source.x)), 0, width - 1);
                    const int sy = top + std::clamp(static_cast<int>(std::floor(source.y)), 0, height - 1);
                    const int cx = std::min(sx / 2, chromaWidth - 1);
                    const int cy = std::min(sy / 2, chromaHeight - 1);
                    ySum += at(frame.y, sx, sy);
                    uSum += at(frame.u, cx, cy);
                    vSum += at(frame.v, cx, cy);
                }
            }
            // Averaging before the (affine) conversion is the same as averaging RGB, short of
            // the clamping.
            const auto rgb = yuvToRgb(static_cast<float>(ySum) / samples, static_cast<float>(uSum) / samples,
                                      static_cast<float>(vSum) / samples, color);
            out[0] = rgb[0];
            out[1] = rgb[1];
            out[2] = rgb[2];
            out[3] = 255;
            out += 4;
        }
    }
    return image;
}

namespace {

// MediaImage2 (media/hardware/VideoAPI.h), packed: type, plane count, width, height, bit depth,
// bits allocated, then per plane offset, column and row increments and the two subsamplings.
constexpr std::size_t kMediaImage2Words = 6 + 4 * 5;
constexpr std::uint32_t kMediaImageTypeYuv = 1;

std::uint32_t word(std::span<const std::uint8_t> bytes, std::size_t index) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + index * 4, 4);
    return value;
}

// Whether a plane of w x h samples, each `bytes` long, fits in a buffer of size bytes from offset.
bool fits(std::size_t size, std::int64_t offset, int w, int h, std::int64_t rowStride, std::int64_t pixelStride,
          int bytes) {
    if (offset < 0 || rowStride <= 0 || pixelStride <= 0 || w <= 0 || h <= 0) {
        return false;
    }
    const std::int64_t last = offset + static_cast<std::int64_t>(h - 1) * rowStride +
                              static_cast<std::int64_t>(w - 1) * pixelStride + bytes;
    return last <= static_cast<std::int64_t>(size);
}

}  // namespace

std::optional<YuvFrame> codecBufferFrame(std::span<const std::uint8_t> buffer, const CodecBufferLayout &layout) {
    YuvFrame frame;
    std::int64_t offsets[3] = {};
    std::int64_t rowStrides[3] = {};
    std::int64_t pixelStrides[3] = {};
    int sampleBytes = 1;
    if (layout.imageData.size() >= kMediaImage2Words * 4) {
        const auto image = layout.imageData;
        const std::uint32_t allocated = word(image, 5);
        if (word(image, 0) != kMediaImageTypeYuv || word(image, 1) < 3 || (allocated != 8 && allocated != 16)) {
            return std::nullopt;
        }
        frame.width = static_cast<int>(word(image, 2));
        frame.height = static_cast<int>(word(image, 3));
        sampleBytes = static_cast<int>(allocated / 8);
        for (std::size_t i = 0; i < 3; ++i) {
            const std::size_t at = 6 + i * 5;
            const std::uint32_t subsampling = i == 0 ? 1 : 2;
            if (word(image, at + 3) != subsampling || word(image, at + 4) != subsampling) {
                return std::nullopt;  // Not 4:2:0.
            }
            offsets[i] = word(image, at);
            pixelStrides[i] = static_cast<std::int32_t>(word(image, at + 1));
            rowStrides[i] = static_cast<std::int32_t>(word(image, at + 2));
        }
    } else {
        frame.width = layout.width;
        frame.height = layout.height;
        const std::int64_t stride = layout.stride > 0 ? layout.stride : layout.width;
        const std::int64_t slice = layout.sliceHeight > 0 ? layout.sliceHeight : layout.height;
        const std::int64_t chroma = stride * slice;
        switch (layout.colorFormat) {
        case kColorFormatYuv420Planar:
        case kColorFormatYuv420PackedPlanar:
            offsets[1] = chroma;
            offsets[2] = chroma + (stride / 2) * ((slice + 1) / 2);
            rowStrides[0] = stride;
            rowStrides[1] = rowStrides[2] = stride / 2;
            pixelStrides[0] = pixelStrides[1] = pixelStrides[2] = 1;
            break;
        case kColorFormatYuv420SemiPlanar:
        case kColorFormatYuv420PackedSemiPlanar:
        case kColorFormatQcomYuv420SemiPlanar32m:
            offsets[1] = chroma;
            offsets[2] = chroma + 1;
            rowStrides[0] = rowStrides[1] = rowStrides[2] = stride;
            pixelStrides[0] = 1;
            pixelStrides[1] = pixelStrides[2] = 2;
            break;
        default:
            return std::nullopt;  // Flexible without a MediaImage2, or a vendor's own.
        }
    }
    if (frame.width <= 0 || frame.height <= 0) {
        return std::nullopt;
    }
    YuvPlane *planes[] = {&frame.y, &frame.u, &frame.v};
    for (int i = 0; i < 3; ++i) {
        const int w = i == 0 ? frame.width : (frame.width + 1) / 2;
        const int h = i == 0 ? frame.height : (frame.height + 1) / 2;
        if (!fits(buffer.size(), offsets[i], w, h, rowStrides[i], pixelStrides[i], sampleBytes)) {
            return std::nullopt;
        }
        // 16-bit samples are little-endian with their bits at the top: the high byte is enough.
        planes[i]->data = buffer.data() + offsets[i] + (sampleBytes - 1);
        planes[i]->rowStride = static_cast<std::int32_t>(rowStrides[i]);
        planes[i]->pixelStride = static_cast<std::int32_t>(pixelStrides[i]);
    }
    if (layout.cropRight >= layout.cropLeft && layout.cropBottom >= layout.cropTop && layout.cropLeft >= 0 &&
        layout.cropTop >= 0 && layout.cropRight < frame.width && layout.cropBottom < frame.height) {
        frame.cropLeft = layout.cropLeft;
        frame.cropTop = layout.cropTop;
        frame.cropRight = layout.cropRight + 1;
        frame.cropBottom = layout.cropBottom + 1;
    }
    return frame;
}

std::int64_t thumbnailSeekTimeUs(std::int64_t durationUs) {
    if (durationUs <= 0) {
        return 0;
    }
    return std::min<std::int64_t>(1'000'000, durationUs / 10);
}

#ifndef __ANDROID__

std::optional<RgbaImage> grabVideoThumbnail(const std::string &, int, int) { return std::nullopt; }

#endif

}  // namespace filepan::media
