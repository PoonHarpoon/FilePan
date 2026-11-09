// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// The pure part of video thumbnails: YUV to RGB with each matrix and range, the rotation and
// crop math, rendering from planar and interleaved planes, and the seek time.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

#include "filepan/media/video_thumbnail.hpp"
#include "test_util.hpp"

namespace {

using filepan::media::codecBufferFrame;
using filepan::media::CodecBufferLayout;
using filepan::media::displayToSource;
using filepan::media::grabVideoThumbnail;
using filepan::media::normalizeRotation;
using filepan::media::renderVideoThumbnail;
using filepan::media::RgbaImage;
using filepan::media::thumbnailLayout;
using filepan::media::thumbnailSeekTimeUs;
using filepan::media::YuvColor;
using filepan::media::yuvColorFor;
using filepan::media::YuvFrame;
using filepan::media::YuvMatrix;
using filepan::media::yuvToRgb;

constexpr YuvColor k601Limited{YuvMatrix::bt601, false};
constexpr YuvColor k601Full{YuvMatrix::bt601, true};
constexpr YuvColor k709Limited{YuvMatrix::bt709, false};
constexpr YuvColor k709Full{YuvMatrix::bt709, true};

bool near(std::array<std::uint8_t, 3> rgb, int r, int g, int b, int tolerance = 2) {
    return std::abs(rgb[0] - r) <= tolerance && std::abs(rgb[1] - g) <= tolerance && std::abs(rgb[2] - b) <= tolerance;
}

// The 8-bit code values of an RGB color (0-1) under a matrix and range, from the definitions.
std::array<float, 3> encode(float r, float g, float b, float kr, float kb, bool full) {
    const float y = kr * r + (1 - kr - kb) * g + kb * b;
    const float cb = (b - y) / (2 * (1 - kb));
    const float cr = (r - y) / (2 * (1 - kr));
    if (full) {
        return {y * 255, 128 + cb * 255, 128 + cr * 255};
    }
    return {16 + y * 219, 128 + cb * 224, 128 + cr * 224};
}

void testConversion() {
    // Black, white and gray in each range.
    EXPECT(near(yuvToRgb(16, 128, 128, k601Limited), 0, 0, 0, 0));
    EXPECT(near(yuvToRgb(235, 128, 128, k601Limited), 255, 255, 255, 0));
    EXPECT(near(yuvToRgb(0, 128, 128, k709Full), 0, 0, 0, 0));
    EXPECT(near(yuvToRgb(255, 128, 128, k709Full), 255, 255, 255, 0));
    EXPECT(near(yuvToRgb(128, 128, 128, k601Full), 128, 128, 128, 0));
    EXPECT(near(yuvToRgb(128, 128, 128, k601Limited), 130, 130, 130, 1));
    // Out-of-range codes clamp.
    EXPECT(near(yuvToRgb(0, 128, 128, k601Limited), 0, 0, 0, 0));
    EXPECT(near(yuvToRgb(255, 128, 128, k601Limited), 255, 255, 255, 0));

    // Primaries and a mixed color round-trip under each matrix and range.
    struct Case {
        YuvColor color;
        float kr, kb;
    };
    const Case cases[] = {{k601Limited, 0.299f, 0.114f},
                          {k601Full, 0.299f, 0.114f},
                          {k709Limited, 0.2126f, 0.0722f},
                          {k709Full, 0.2126f, 0.0722f},
                          {{YuvMatrix::bt2020, false}, 0.2627f, 0.0593f}};
    const std::array<std::array<float, 3>, 5> colors{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {0.8f, 0.4f, 0.2f}}};
    for (const Case &c : cases) {
        for (const auto &rgb : colors) {
            const auto yuv = encode(rgb[0], rgb[1], rgb[2], c.kr, c.kb, c.color.fullRange);
            EXPECT(near(yuvToRgb(yuv[0], yuv[1], yuv[2], c.color), static_cast<int>(std::lround(rgb[0] * 255)),
                        static_cast<int>(std::lround(rgb[1] * 255)), static_cast<int>(std::lround(rgb[2] * 255))));
        }
    }
    // Known code values: BT.601 limited red is about (81, 90, 240), BT.709's (63, 102, 240).
    EXPECT(near(yuvToRgb(81, 90, 240, k601Limited), 255, 0, 0, 3));
    EXPECT(near(yuvToRgb(63, 102, 240, k709Limited), 255, 0, 0, 3));
    // The wrong matrix visibly shifts the color.
    const auto wrong = yuvToRgb(81, 90, 240, k709Limited);
    EXPECT(!near(wrong, 255, 0, 0, 10));
    // So does the wrong range.
    EXPECT(!near(yuvToRgb(81, 90, 240, k601Full), 255, 0, 0, 10));

    // From the format's keys.
    EXPECT((yuvColorFor(1, 2, 640, 480) == k709Limited));
    EXPECT((yuvColorFor(2, 1, 1920, 1080) == k601Full));
    EXPECT((yuvColorFor(4, 0, 1920, 1080) == k601Limited));
    EXPECT((yuvColorFor(6, 2, 3840, 2160) == YuvColor{YuvMatrix::bt2020, false}));
    // Unspecified: by size.
    EXPECT((yuvColorFor(0, 0, 640, 480) == k601Limited));
    EXPECT((yuvColorFor(0, 0, 1280, 720) == k709Limited));
    EXPECT((yuvColorFor(0, 0, 720, 1280) == k709Limited));  // A portrait HD picture.
    EXPECT((yuvColorFor(0, 1, 320, 240) == k601Full));
}

void testGeometry() {
    EXPECT(normalizeRotation(0) == 0 && normalizeRotation(90) == 90 && normalizeRotation(-90) == 270 &&
           normalizeRotation(450) == 90 && normalizeRotation(359) == 270 && normalizeRotation(-180) == 180);

    // Landscape: the shorter side to 256, the center cut out.
    auto layout = thumbnailLayout(1920, 1080, 0, 256);
    EXPECT(layout.displayWidth == 1920 && layout.displayHeight == 1080);
    EXPECT(layout.scaledWidth == 455 && layout.scaledHeight == 256 && layout.side == 256);
    EXPECT(layout.offsetX == 99 && layout.offsetY == 0);
    // Rotated a quarter turn it's portrait.
    layout = thumbnailLayout(1920, 1080, 90, 256);
    EXPECT(layout.displayWidth == 1080 && layout.displayHeight == 1920);
    EXPECT(layout.scaledWidth == 256 && layout.scaledHeight == 455 && layout.offsetX == 0 && layout.offsetY == 99);
    layout = thumbnailLayout(1920, 1080, 180, 256);
    EXPECT(layout.displayWidth == 1920 && layout.offsetX == 99);
    layout = thumbnailLayout(1920, 1080, 270, 256);
    EXPECT(layout.displayWidth == 1080 && layout.offsetY == 99);
    // Never scaled up.
    layout = thumbnailLayout(100, 50, 0, 256);
    EXPECT(layout.scale == 1 && layout.side == 50 && layout.offsetX == 25 && layout.offsetY == 0);
    EXPECT(thumbnailLayout(0, 50, 0, 256).side == 0 && thumbnailLayout(10, 10, 0, 0).side == 0);

    // Display corners back in a 4 x 2 source.
    auto at = [](float x, float y, int rotation, float sx, float sy) {
        const auto p = displayToSource(x, y, 4, 2, rotation);
        return p.x == sx && p.y == sy;
    };
    EXPECT(at(0, 0, 0, 0, 0) && at(4, 2, 0, 4, 2));
    // 90 clockwise: the display is 2 x 4; its top-right corner is the source's top-left.
    EXPECT(at(2, 0, 90, 0, 0) && at(0, 0, 90, 0, 2) && at(0, 4, 90, 4, 2) && at(2, 4, 90, 4, 0));
    EXPECT(at(4, 2, 180, 0, 0) && at(0, 0, 180, 4, 2));
    // 270 clockwise: its bottom-left corner is the source's top-left.
    EXPECT(at(0, 4, 270, 0, 0) && at(0, 0, 270, 4, 0) && at(2, 0, 270, 4, 2));
}

// A planar 4:2:0 picture.
struct Picture {
    int width;
    int height;
    std::vector<std::uint8_t> y;
    std::vector<std::uint8_t> u;
    std::vector<std::uint8_t> v;

    Picture(int w, int h, std::uint8_t luma = 16)
        : width(w), height(h), y(static_cast<std::size_t>(w * h), luma),
          u(static_cast<std::size_t>(((w + 1) / 2) * ((h + 1) / 2)), 128),
          v(static_cast<std::size_t>(((w + 1) / 2) * ((h + 1) / 2)), 128) {}

    void setY(int x, int row, std::uint8_t value) { y[static_cast<std::size_t>(row * width + x)] = value; }

    [[nodiscard]] YuvFrame frame() const {
        const int chroma = (width + 1) / 2;
        return {{y.data(), width, 1}, {u.data(), chroma, 1}, {v.data(), chroma, 1}, width, height};
    }
};

std::uint8_t red(const RgbaImage &image, int x, int y) {
    return image.rgba[static_cast<std::size_t>((y * image.width + x) * 4)];
}

void testRender() {
    // A 2 x 2 picture, white top-left: each rotation moves that corner.
    Picture corner(2, 2);
    corner.setY(0, 0, 235);
    auto white = [&](int rotation) {
        auto image = renderVideoThumbnail(corner.frame(), k601Limited, rotation, 2);
        EXPECT(image && image->width == 2 && image->height == 2 && image->rgba.size() == 16);
        EXPECT(image->rgba[3] == 255);
        int found = -1;
        for (int i = 0; i < 4; ++i) {
            if (red(*image, i % 2, i / 2) == 255) {
                EXPECT(found == -1);
                found = i;
            } else {
                EXPECT(red(*image, i % 2, i / 2) == 0);
            }
        }
        return found;
    };
    EXPECT(white(0) == 0);    // Top-left.
    EXPECT(white(90) == 1);   // Top-right.
    EXPECT(white(180) == 3);  // Bottom-right.
    EXPECT(white(270) == 2);  // Bottom-left.

    // A 4 x 2 landscape picture is center-cropped: of black, white, white, black only the white
    // middle is left.
    Picture wide(4, 2);
    for (int row = 0; row < 2; ++row) {
        wide.setY(1, row, 235);
        wide.setY(2, row, 235);
    }
    auto image = renderVideoThumbnail(wide.frame(), k601Limited, 0, 8);
    EXPECT(image && image->width == 2 && image->height == 2);
    for (int i = 0; i < 4; ++i) {
        EXPECT(red(*image, i % 2, i / 2) == 255);
    }
    // Turned sideways it's 2 x 4 and the crop takes the middle rows of the display: still white.
    image = renderVideoThumbnail(wide.frame(), k601Limited, 90, 8);
    EXPECT(image && image->width == 2 && red(*image, 0, 0) == 255 && red(*image, 1, 1) == 255);

    // The frame's crop rect: only its right half (white) shows.
    Picture halves(4, 2);
    for (int row = 0; row < 2; ++row) {
        halves.setY(2, row, 235);
        halves.setY(3, row, 235);
    }
    YuvFrame cropped = halves.frame();
    cropped.cropLeft = 2;
    cropped.cropTop = 0;
    cropped.cropRight = 4;
    cropped.cropBottom = 2;
    image = renderVideoThumbnail(cropped, k601Limited, 0, 8);
    EXPECT(image && image->width == 2);
    for (int i = 0; i < 4; ++i) {
        EXPECT(red(*image, i % 2, i / 2) == 255);
    }

    // Shrinking averages: black and white columns become mid gray.
    Picture stripes(8, 8);
    for (int row = 0; row < 8; ++row) {
        for (int x = 1; x < 8; x += 2) {
            stripes.setY(x, row, 235);
        }
    }
    image = renderVideoThumbnail(stripes.frame(), k601Limited, 0, 2);
    EXPECT(image && image->width == 2);
    for (int i = 0; i < 4; ++i) {
        EXPECT(std::abs(red(*image, i % 2, i / 2) - 128) <= 2);
    }

    // Interleaved chroma (NV12: Cb Cr Cb Cr, pixel stride 2) in a padded buffer, BT.601 red.
    const int width = 4;
    const int height = 4;
    const int stride = 8;
    std::vector<std::uint8_t> luma(static_cast<std::size_t>(stride * height), 81);
    std::vector<std::uint8_t> uv(static_cast<std::size_t>(stride * height / 2), 0);
    for (int row = 0; row < height / 2; ++row) {
        for (int x = 0; x < width / 2; ++x) {
            uv[static_cast<std::size_t>(row * stride + x * 2)] = 90;
            uv[static_cast<std::size_t>(row * stride + x * 2 + 1)] = 240;
        }
    }
    const YuvFrame nv12{{luma.data(), stride, 1}, {uv.data(), stride, 2}, {uv.data() + 1, stride, 2}, width, height};
    image = renderVideoThumbnail(nv12, k601Limited, 0, 4);
    EXPECT(image && image->width == 4);
    for (std::size_t i = 0; i < image->rgba.size(); i += 4) {
        EXPECT(image->rgba[i] >= 250 && image->rgba[i + 1] <= 5 && image->rgba[i + 2] <= 5 && image->rgba[i + 3] == 255);
    }
    // NV21 swaps them: the same bytes read as Cr first is a different color.
    const YuvFrame nv21{{luma.data(), stride, 1}, {uv.data() + 1, stride, 2}, {uv.data(), stride, 2}, width, height};
    image = renderVideoThumbnail(nv21, k601Limited, 0, 4);
    EXPECT(image && image->rgba[0] < 40 && image->rgba[2] >= 250);

    // Nothing to draw.
    EXPECT(!renderVideoThumbnail(YuvFrame{}, k601Limited, 0, 4));
    EXPECT(!renderVideoThumbnail(corner.frame(), k601Limited, 0, 0));
}

void testSeekTime() {
    EXPECT(thumbnailSeekTimeUs(0) == 0);
    EXPECT(thumbnailSeekTimeUs(-5) == 0);  // Unknown.
    EXPECT(thumbnailSeekTimeUs(3'000'000) == 300'000);  // 10% of a short clip.
    EXPECT(thumbnailSeekTimeUs(10'000'000) == 1'000'000);
    EXPECT(thumbnailSeekTimeUs(3'600'000'000) == 1'000'000);  // At most a second in.
    EXPECT(thumbnailSeekTimeUs(40'000) == 4'000);
}

void testHostStub() { EXPECT(!grabVideoThumbnail("/nonexistent.mp4", 128)); }

void putWord(std::vector<std::uint8_t> &bytes, std::size_t index, std::uint32_t value) {
    std::memcpy(bytes.data() + index * 4, &value, 4);
}

// A MediaImage2: YUV, 3 planes, the size and bits, then each plane's offset, column and row
// increments.
std::vector<std::uint8_t> mediaImage(int w, int h, int bits, std::array<std::array<std::uint32_t, 3>, 3> planes,
                                     std::uint32_t chromaSubsampling = 2) {
    std::vector<std::uint8_t> bytes(26 * 4);
    putWord(bytes, 0, 1);
    putWord(bytes, 1, 3);
    putWord(bytes, 2, static_cast<std::uint32_t>(w));
    putWord(bytes, 3, static_cast<std::uint32_t>(h));
    putWord(bytes, 4, static_cast<std::uint32_t>(bits == 16 ? 10 : 8));
    putWord(bytes, 5, static_cast<std::uint32_t>(bits));
    for (std::size_t i = 0; i < 3; ++i) {
        const std::size_t at = 6 + i * 5;
        putWord(bytes, at, planes[i][0]);
        putWord(bytes, at + 1, planes[i][1]);
        putWord(bytes, at + 2, planes[i][2]);
        putWord(bytes, at + 3, i == 0 ? 1 : chromaSubsampling);
        putWord(bytes, at + 4, i == 0 ? 1 : chromaSubsampling);
    }
    return bytes;
}

void testCodecBuffer() {
    // I420 by color format, stride 8 and slice height 4 for a 6 x 4 picture.
    std::vector<std::uint8_t> i420(8 * 4 + 4 * 2 * 2, 0);
    i420[8 + 1] = 77;           // Y at (1, 1).
    i420[32 + 4 + 2] = 99;      // U at (2, 1).
    i420[32 + 8 + 4 + 2] = 55;  // V at (2, 1).
    CodecBufferLayout layout{.colorFormat = filepan::media::kColorFormatYuv420Planar,
                             .width = 6,
                             .height = 4,
                             .stride = 8,
                             .sliceHeight = 4};
    auto frame = codecBufferFrame(i420, layout);
    EXPECT(frame && frame->width == 6 && frame->height == 4);
    EXPECT(frame->y.data[frame->y.rowStride + 1] == 77);
    EXPECT(frame->u.data[frame->u.rowStride + 2 * frame->u.pixelStride] == 99);
    EXPECT(frame->v.data[frame->v.rowStride + 2 * frame->v.pixelStride] == 55);
    EXPECT(frame->cropRight == 0);  // No crop given.
    // Too short a buffer is refused.
    EXPECT(!codecBufferFrame(std::span(i420).first(40), layout));
    // Flexible without a MediaImage2 says nothing about the layout.
    layout.colorFormat = filepan::media::kColorFormatYuv420Flexible;
    EXPECT(!codecBufferFrame(i420, layout));

    // NV12: interleaved chroma after the luma, with a crop (inclusive) of the whole 4 x 2.
    std::vector<std::uint8_t> nv12(4 * 2 + 4, 0);
    nv12[8] = 10;   // U at (0, 0).
    nv12[9] = 20;   // V at (0, 0).
    nv12[11] = 40;  // V at (1, 0).
    CodecBufferLayout semi{.colorFormat = filepan::media::kColorFormatYuv420SemiPlanar,
                           .width = 4,
                           .height = 2,
                           .cropLeft = 0,
                           .cropTop = 0,
                           .cropRight = 3,
                           .cropBottom = 1};
    frame = codecBufferFrame(nv12, semi);
    EXPECT(frame && frame->u.pixelStride == 2 && frame->u.data[0] == 10 && frame->v.data[0] == 20 &&
           frame->v.data[2] == 40);
    EXPECT(frame->cropRight == 4 && frame->cropBottom == 2);
    // A crop outside the picture is ignored.
    semi.cropRight = 9;
    frame = codecBufferFrame(nv12, semi);
    EXPECT(frame && frame->cropRight == 0);

    // A MediaImage2 wins over the color format, and P010 is read by its high bytes.
    std::vector<std::uint8_t> p010(2 * 2 * 2 + 4, 0);
    p010[1] = 0xEB;      // Y (0, 0), high byte.
    p010[8 + 1] = 0x40;  // U, high byte.
    p010[8 + 3] = 0xC0;  // V, high byte.
    const auto image = mediaImage(2, 2, 16, {{{0, 2, 4}, {8, 4, 4}, {10, 4, 4}}});
    CodecBufferLayout described{.imageData = image, .colorFormat = filepan::media::kColorFormatYuv420Flexible};
    frame = codecBufferFrame(p010, described);
    EXPECT(frame && frame->width == 2 && frame->y.data[0] == 0xEB && frame->u.data[0] == 0x40 &&
           frame->v.data[0] == 0xC0);
    EXPECT(renderVideoThumbnail(*frame, k601Limited, 0, 2));
    // A plane past the end, and 4:4:4, are refused.
    EXPECT(!codecBufferFrame(std::span(p010).first(10), described));
    const auto full = mediaImage(2, 2, 8, {{{0, 1, 2}, {4, 1, 2}, {8, 1, 2}}}, 1);
    described.imageData = full;
    EXPECT(!codecBufferFrame(p010, described));
}

}  // namespace

int main() {
    testConversion();
    testGeometry();
    testRender();
    testSeekTime();
    testHostStub();
    testCodecBuffer();
    std::printf("video_thumbnail_test passed\n");
    return 0;
}
