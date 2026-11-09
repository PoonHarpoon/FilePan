// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "filepan/gfx/display_list.hpp"
#include "filepan/gfx/text.hpp"
#include "png.hpp"
#include "test_util.hpp"

namespace filepan::test {

// Stands in for the real text system, which is written elsewhere: monospace "shaping" and
// procedural glyphs, so the renderer's text path can be tested without fonts.
//   Face 1: alpha8 glyphs, a stem plus a crossbar that both depend on the glyph id, with the
//           subpixel offset applied analytically.
//   Face 2: rgba8 "emoji", a premultiplied colored disc.
class FakeTextSystem : public gfx::TextSystem {
public:
    static constexpr gfx::FaceKey kAlphaFace = 1;
    static constexpr gfx::FaceKey kColorFace = 2;

    gfx::ShapedText shape(std::string_view utf8, const gfx::TextStyle &style,
                          const gfx::TextLayoutOptions &) override {
        gfx::ShapedText text;
        const float advance = style.size * 0.6f;
        const float baseline = std::round(style.size * 0.8f);
        float x = 0;
        for (unsigned char c : utf8) {
            text.glyphs.push_back({c, {x, baseline}});
            x += advance + style.letterSpacing * style.size;
        }
        text.runs.push_back({kAlphaFace, style.size, false, 0,
                             static_cast<std::uint32_t>(text.glyphs.size())});
        text.size = {x, style.size};
        text.lines.push_back({0, baseline, style.size, x, 0, static_cast<std::uint32_t>(utf8.size())});
        return text;
    }

    bool rasterize(gfx::FaceKey face, std::uint32_t glyph, float size, float subpixelX,
                   gfx::GlyphBitmap &out) override {
        ++rasterizeCalls;
        if (glyph == ' ') {
            return false;
        }
        if (face == kColorFace) {
            const int d = std::max(2, static_cast<int>(std::lround(size * 0.8f)));
            out.width = d;
            out.height = d;
            out.left = 0;
            out.top = static_cast<int>(std::lround(size * 0.8f));
            out.format = gfx::GlyphFormat::rgba8;
            out.pixels.assign(static_cast<std::size_t>(d) * d * 4, 0);
            const float radius = d * 0.5f;
            for (int y = 0; y < d; ++y) {
                for (int x = 0; x < d; ++x) {
                    const float dx = x + 0.5f - radius;
                    const float dy = y + 0.5f - radius;
                    const float a = std::clamp(radius - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
                    const float t = static_cast<float>(y) / d;
                    std::uint8_t *p = &out.pixels[(static_cast<std::size_t>(y) * d + x) * 4];
                    p[0] = static_cast<std::uint8_t>(std::lround(255 * a * (1 - t)));
                    p[1] = static_cast<std::uint8_t>(std::lround(255 * a * 0.6f));
                    p[2] = static_cast<std::uint8_t>(std::lround(255 * a * t));
                    p[3] = static_cast<std::uint8_t>(std::lround(255 * a));
                }
            }
            return true;
        }
        // A stem at a glyph-dependent x and a crossbar at a glyph-dependent height.
        const int width = std::max(3, static_cast<int>(std::lround(size * 0.5f))) + 1;
        const int height = std::max(3, static_cast<int>(std::lround(size * 0.7f)));
        out.width = width;
        out.height = height;
        out.left = 0;
        out.top = height;
        out.format = gfx::GlyphFormat::alpha8;
        out.pixels.assign(static_cast<std::size_t>(width) * height, 0);
        const float stem0 = size * (0.05f + 0.15f * static_cast<float>(glyph % 3)) + subpixelX;
        const float stem1 = stem0 + std::max(1.0f, size * 0.1f);
        const float bar0 = static_cast<float>(height) * (0.2f + 0.2f * static_cast<float>(glyph % 4));
        const float bar1 = bar0 + std::max(1.0f, size * 0.08f);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const float stem = std::clamp(std::min(x + 1.0f, stem1) - std::max<float>(x, stem0),
                                              0.0f, 1.0f);
                const float barX = std::clamp(
                        std::min(x + 1.0f, size * 0.5f + subpixelX) - std::max<float>(x, subpixelX),
                        0.0f, 1.0f);
                const float barY = std::clamp(std::min(y + 1.0f, bar1) - std::max<float>(y, bar0),
                                              0.0f, 1.0f);
                const float coverage = std::max(stem, barX * barY);
                out.pixels[static_cast<std::size_t>(y) * width + x] =
                        static_cast<std::uint8_t>(std::lround(coverage * 255));
            }
        }
        return true;
    }

    int rasterizeCalls = 0;
};

// Text from the fake shaper, in face, as a shared ShapedText for TextCommand.
inline std::shared_ptr<const gfx::ShapedText> fakeText(FakeTextSystem &text, std::string_view s,
                                                       float size,
                                                       gfx::FaceKey face = FakeTextSystem::kAlphaFace) {
    gfx::TextStyle style;
    style.size = size;
    auto shaped = text.shape(s, style, {});
    shaped.runs[0].face = face;
    shaped.runs[0].color = face == FakeTextSystem::kColorFace;
    return std::make_shared<const gfx::ShapedText>(std::move(shaped));
}

// Exits with ctest's skip code when there's no Vulkan device, except on CI, where lavapipe is
// always there and a missing device is a failure.
[[noreturn]] inline void skipOrFail(const std::string &why) {
    const char *ci = std::getenv("CI");
    if (ci != nullptr && *ci != '\0' && std::string_view(ci) != "false") {
        std::fprintf(stderr, "FAIL: no Vulkan device on CI: %s\n", why.c_str());
        std::exit(1);
    }
    std::fprintf(stderr, "SKIP: no Vulkan device (%s). The dev shell provides lavapipe.\n",
                 why.c_str());
    std::exit(77);
}

inline bool updatingGoldens() {
    const char *update = std::getenv("FILEPAN_UPDATE_GOLDENS");
    return update != nullptr && std::string_view(update) == "1";
}

struct ImageDiff {
    int maxDifference = 0;
    int pixelsOverTolerance = 0;
};

inline ImageDiff compareImages(const RgbaImage &a, const RgbaImage &b, int tolerance,
                               RgbaImage *diff = nullptr) {
    ImageDiff result;
    if (diff != nullptr) {
        *diff = {a.width, a.height, std::vector<std::uint8_t>(a.pixels.size(), 255)};
    }
    for (std::size_t i = 0; i < a.pixels.size(); i += 4) {
        int pixelMax = 0;
        for (int c = 0; c < 4; ++c) {
            pixelMax = std::max(pixelMax, std::abs(int{a.pixels[i + c]} - int{b.pixels[i + c]}));
        }
        result.maxDifference = std::max(result.maxDifference, pixelMax);
        if (pixelMax > tolerance) {
            ++result.pixelsOverTolerance;
        }
        if (diff != nullptr) {
            const auto level = static_cast<std::uint8_t>(std::min(255, pixelMax * 8));
            diff->pixels[i] = 255;
            diff->pixels[i + 1] = static_cast<std::uint8_t>(255 - level);
            diff->pixels[i + 2] = static_cast<std::uint8_t>(255 - level);
        }
    }
    return result;
}

// Compares a render with golden/<name>.png: every channel within kTolerance, except for at
// most 0.2% of pixels (antialiasing differences across Mesa versions), none of which may be
// off by more than kMaxOutlier. FILEPAN_UPDATE_GOLDENS=1 rewrites the golden instead.
// On failure, the render and a diff go to the build directory.
inline bool matchesGolden(const std::string &name, const RgbaImage &actual) {
    constexpr int kTolerance = 3;
    constexpr int kMaxOutlier = 40;
    const std::filesystem::path golden = std::filesystem::path(FILEPAN_GOLDEN_DIR) / (name + ".png");
    if (updatingGoldens()) {
        std::filesystem::create_directories(golden.parent_path());
        EXPECT(writePng(golden, actual));
        std::printf("  updated %s\n", golden.c_str());
        return true;
    }
    const std::filesystem::path outDir(FILEPAN_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(outDir);
    const auto expected = readPng(golden);
    if (!expected) {
        writePng(outDir / (name + ".actual.png"), actual);
        std::fprintf(stderr, "  %s: missing golden %s (run with FILEPAN_UPDATE_GOLDENS=1)\n",
                     name.c_str(), golden.c_str());
        return false;
    }
    if (expected->width != actual.width || expected->height != actual.height) {
        std::fprintf(stderr, "  %s: size %dx%d, golden %dx%d\n", name.c_str(), actual.width,
                     actual.height, expected->width, expected->height);
        return false;
    }
    RgbaImage diff;
    const ImageDiff result = compareImages(actual, *expected, kTolerance, &diff);
    const int allowed = actual.width * actual.height / 500;
    if (result.pixelsOverTolerance > allowed || result.maxDifference > kMaxOutlier) {
        writePng(outDir / (name + ".actual.png"), actual);
        writePng(outDir / (name + ".diff.png"), diff);
        std::fprintf(stderr,
                     "  %s: %d pixels off by more than %d (allowed %d), max difference %d; "
                     "see %s\n",
                     name.c_str(), result.pixelsOverTolerance, kTolerance, allowed,
                     result.maxDifference, outDir.c_str());
        return false;
    }
    return true;
}

}  // namespace filepan::test
