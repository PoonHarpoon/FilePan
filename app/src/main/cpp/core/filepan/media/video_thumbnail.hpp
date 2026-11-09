// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Video thumbnails: one decoded frame of a local video, scaled and cropped to a square.
//
// On Android, grabVideoThumbnail runs an AMediaExtractor (the video track only) into an
// AMediaCodec that renders into an AImageReader (AIMAGE_FORMAT_YUV_420_888, CPU readable), takes
// the first frame from a sync frame near thumbnailSeekTimeUs, and converts it here. If that gives
// nothing, it decodes again into the codec's own buffers (flexible YUV, no surface) and reads the
// frame from there. It's independent of playback (VideoBackend) and bounded by a deadline; at
// most kMaxConcurrentGrabs run at once, so a folder of videos doesn't take every hardware decoder.
// A grab that gives nothing logs one line saying why (tag "FilePan"). Elsewhere it returns
// nullopt.
//
// The rest is the pure part, built and tested on the host: the color conversion, the rotation and
// crop math, the codec buffer layout and the seek time.
namespace filepan::media {

// The YCbCr matrix of a picture (its primaries' luma coefficients).
enum class YuvMatrix : std::uint8_t { bt601, bt709, bt2020 };

struct YuvColor {
    YuvMatrix matrix = YuvMatrix::bt601;
    bool fullRange = false;  // 0-255, rather than 16-235 luma and 16-240 chroma.

    friend bool operator==(const YuvColor &, const YuvColor &) = default;
};

// MediaFormat's color-standard and color-range values (AMEDIAFORMAT_KEY_COLOR_STANDARD and
// _COLOR_RANGE); 0 when the format doesn't say.
inline constexpr std::int32_t kColorStandardBt709 = 1;
inline constexpr std::int32_t kColorStandardBt601Pal = 2;
inline constexpr std::int32_t kColorStandardBt601Ntsc = 4;
inline constexpr std::int32_t kColorStandardBt2020 = 6;
inline constexpr std::int32_t kColorRangeFull = 1;
inline constexpr std::int32_t kColorRangeLimited = 2;

// The color of a frame from its format's standard and range. Without a standard, HD pictures
// (720 lines or more) are BT.709 and smaller ones BT.601, as decoders assume; without a range,
// limited.
YuvColor yuvColorFor(std::int32_t standard, std::int32_t range, int width, int height);

// One pixel to 8-bit RGB. y, u and v are 8-bit code values, fractional for averaged samples.
std::array<std::uint8_t, 3> yuvToRgb(float y, float u, float v, YuvColor color);

// A 4:2:0 picture as AImage gives its planes: any row and pixel strides, so I420, NV12 and NV21
// all fit. Chroma planes are half the size, rounded up.
struct YuvPlane {
    const std::uint8_t *data = nullptr;
    std::int32_t rowStride = 0;
    std::int32_t pixelStride = 1;
};

struct YuvFrame {
    YuvPlane y;
    YuvPlane u;  // Cb
    YuvPlane v;  // Cr
    int width = 0;  // The buffer.
    int height = 0;
    // The visible part (AImage_getCropRect), within the buffer; empty means all of it.
    int cropLeft = 0;
    int cropTop = 0;
    int cropRight = 0;
    int cropBottom = 0;
};

// 0, 90, 180 or 270: degrees rounded down to a quarter turn, in [0, 360).
int normalizeRotation(int degrees);

// How a width x height picture shown at rotation (clockwise) becomes a square thumbnail: like
// image thumbnails, scaled so the shorter displayed side is sizePx (never up), then the center
// square of side px cut out at (offsetX, offsetY) of the scaled picture.
struct ThumbnailLayout {
    int displayWidth = 0;  // After rotation.
    int displayHeight = 0;
    float scale = 1;  // Displayed pixels to scaled pixels.
    int scaledWidth = 0;
    int scaledHeight = 0;
    int side = 0;
    int offsetX = 0;
    int offsetY = 0;
};
ThumbnailLayout thumbnailLayout(int width, int height, int rotation, int sizePx);

// A point of the displayed (rotated) picture, in continuous pixel coordinates, in the
// unrotated width x height picture.
struct SourcePoint {
    float x = 0;
    float y = 0;
};
SourcePoint displayToSource(float x, float y, int width, int height, int rotation);

struct RgbaImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;  // Opaque RGBA8, tightly packed.
};

// The frame's visible part, rotated, scaled with a box filter and center-cropped to the
// thumbnailLayout square, in RGB. nullopt for an empty frame.
std::optional<RgbaImage> renderVideoThumbnail(const YuvFrame &frame, YuvColor color, int rotation, int sizePx);

// Where to take the thumbnail from: min(1 s, 10% of the duration), so it's past a fade in but
// still early in short clips; 0 when the duration is unknown.
std::int64_t thumbnailSeekTimeUs(std::int64_t durationUs);

// MediaCodec's color formats (MediaCodecInfo.CodecCapabilities) that codecBufferFrame knows
// without a MediaImage2.
inline constexpr std::int32_t kColorFormatYuv420Planar = 19;
inline constexpr std::int32_t kColorFormatYuv420PackedPlanar = 20;
inline constexpr std::int32_t kColorFormatYuv420SemiPlanar = 21;
inline constexpr std::int32_t kColorFormatYuv420PackedSemiPlanar = 39;
inline constexpr std::int32_t kColorFormatQcomYuv420SemiPlanar32m = 0x7FA30C04;
inline constexpr std::int32_t kColorFormatYuv420Flexible = 0x7F420888;

// How a decoder's output buffer (no surface) holds its picture, from the buffer's format.
struct CodecBufferLayout {
    // The format's "image-data": a MediaImage2 (media/hardware/VideoAPI.h) with the exact plane
    // offsets and strides. Empty when the format doesn't have it; then the rest describes it.
    std::span<const std::uint8_t> imageData;
    std::int32_t colorFormat = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t stride = 0;       // 0: the width.
    std::int32_t sliceHeight = 0;  // 0: the height.
    // AMEDIAFORMAT_KEY_DISPLAY_CROP, right and bottom inclusive; none when right < left.
    std::int32_t cropLeft = 0;
    std::int32_t cropTop = 0;
    std::int32_t cropRight = -1;
    std::int32_t cropBottom = -1;
};

// The 4:2:0 picture in buffer (an output buffer from its offset, its size long), or nullopt when
// the layout is unknown or doesn't fit. 16-bit samples (P010) are read by their high byte.
std::optional<YuvFrame> codecBufferFrame(std::span<const std::uint8_t> buffer, const CodecBufferLayout &layout);

// Grabs that may run at once; the rest wait their turn before their deadline starts.
inline constexpr int kMaxConcurrentGrabs = 2;

// Decodes a frame of the local video at path (see above). Blocks for at most about timeoutMs
// once its turn comes; creating a hardware codec alone can take hundreds of milliseconds.
std::optional<RgbaImage> grabVideoThumbnail(const std::string &path, int sizePx, int timeoutMs = 5000);

}  // namespace filepan::media
