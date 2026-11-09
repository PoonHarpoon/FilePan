// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Golden-image tests for the Vulkan renderer, on lavapipe in the dev shell and CI. Each scene is
// also probed at a few pixels whose values follow from the spec, so a golden regenerated from a
// broken renderer doesn't go unnoticed.
//
//   FILEPAN_UPDATE_GOLDENS=1 ctest -R renderer_test   rewrites host/gfx/golden/*.png
//   FILEPAN_VK_VALIDATION=1 also runs with the Khronos validation layer (it's on by default
//   here when the layer is installed).

#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <numbers>
#include <string>
#include <vector>

#include "filepan/gfx/vulkan/renderer.hpp"
#include "gfx_test_util.hpp"

using namespace filepan::gfx;
using filepan::gfx::vulkan::FrameTiming;
using filepan::gfx::vulkan::Renderer;
using filepan::gfx::vulkan::RenderTarget;
using filepan::test::FakeTextSystem;
using filepan::test::RgbaImage;

namespace {

constexpr Color kVoid = Color::fromRgb(0x070312);
constexpr Color kBone = Color::fromRgb(0xEEF2FF);
constexpr Color kSky = Color::fromRgb(0x38BDF8);
constexpr Color kLavender = Color::fromRgb(0xA78BFA);
constexpr Color kViolet = Color::fromRgb(0x7C3AED);
constexpr Color kBlue = Color::fromRgb(0x60A5FA);
constexpr Color kPink = Color::fromRgb(0xFF4FC4);
constexpr Color kPurple = Color::fromRgb(0x9D5CFF);
constexpr Color kAcid = Color::fromRgb(0xC6FF1A);
constexpr Color kPaper = Color::fromRgb(0xE4E4DD);
constexpr float kPill = 1e6f;

Transform rotation(float radians, Vec2 center) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    return Transform::translation(center) * Transform{c, s, -s, c, 0, 0} *
           Transform::translation({-center.x, -center.y});
}

struct Harness {
    FakeTextSystem text;
    std::unique_ptr<Renderer> renderer;
    std::vector<FrameTiming> timings;
    int failures = 0;

    Harness() {
        filepan::gfx::vulkan::RendererOptions options;
        options.validation = true;  // Used when the layer is installed.
        auto created = Renderer::create(text, options);
        if (!created) {
            filepan::test::skipOrFail(created.error().message);
        }
        renderer = std::move(*created);
        renderer->setFrameTimingCallback([this](const FrameTiming &t) { timings.push_back(t); });
        std::printf("device: %s\n", renderer->deviceName().c_str());
    }

    RgbaImage render(RenderTarget &target, const DisplayList &list, Color clear = kVoid) {
        auto rendered = renderer->render(target, list, clear);
        if (!rendered) {
            std::fprintf(stderr, "render failed: %s\n", rendered.error().message.c_str());
            std::exit(1);
        }
        auto pixels = renderer->readPixels(target);
        EXPECT(pixels.has_value());
        const Size size = target.size();
        RgbaImage image{static_cast<int>(size.width), static_cast<int>(size.height),
                        std::move(*pixels)};
        // FILEPAN_DUMP_RENDERS=1 saves every render, numbered, for debugging probes.
        if (const char *dump = std::getenv("FILEPAN_DUMP_RENDERS"); dump && *dump == '1') {
            const std::filesystem::path dir(FILEPAN_TEST_OUTPUT_DIR);
            std::filesystem::create_directories(dir);
            filepan::test::writePng(dir / ("render-" + std::to_string(renders) + ".png"), image);
        }
        ++renders;
        return image;
    }
    int renders = 0;

    RgbaImage render(int width, int height, const DisplayList &list, Color clear = kVoid) {
        auto target = renderer->createOffscreenTarget(width, height);
        EXPECT(target.has_value());
        return render(**target, list, clear);
    }

    void golden(const std::string &name, const RgbaImage &image) {
        if (!filepan::test::matchesGolden(name, image)) {
            ++failures;
        }
    }
};

std::array<int, 4> pixel(const RgbaImage &image, int x, int y) {
    const std::uint8_t *p = &image.pixels[(static_cast<std::size_t>(y) * image.width + x) * 4];
    return {p[0], p[1], p[2], p[3]};
}

// Expects the pixel to be within tolerance of color (8-bit, opaque unless given).
void expectPixel(const RgbaImage &image, int x, int y, Color color, int tolerance = 2) {
    const auto actual = pixel(image, x, y);
    const int expected[4] = {static_cast<int>(std::lround(color.r * 255)),
                             static_cast<int>(std::lround(color.g * 255)),
                             static_cast<int>(std::lround(color.b * 255)),
                             static_cast<int>(std::lround(color.a * 255))};
    for (int c = 0; c < 4; ++c) {
        if (std::abs(actual[c] - expected[c]) > tolerance) {
            std::fprintf(stderr, "pixel (%d, %d) is %d %d %d %d, expected %d %d %d %d\n", x, y,
                         actual[0], actual[1], actual[2], actual[3], expected[0], expected[1],
                         expected[2], expected[3]);
            std::exit(1);
        }
    }
}

// src over dst for an opaque dst, in sRGB space like the renderer.
Color over(Color src, Color dst) {
    return {src.r * src.a + dst.r * (1 - src.a), src.g * src.a + dst.g * (1 - src.a),
            src.b * src.a + dst.b * (1 - src.a), 1};
}

float luminance(const RgbaImage &image, int x, int y) {
    const auto p = pixel(image, x, y);
    return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
}

void testFills(Harness &h) {
    DisplayList list;
    list.fill(Rect{8, 8, 40, 24}, kSky);
    list.fill(RoundedRect{{56, 8, 64, 40}, {0, 8, 20, 4}}, kLavender);
    list.fill(RoundedRect{{8, 40, 40, 16}, CornerRadii::uniform(kPill)}, kPink);
    list.fill(RoundedRect{{64, 56, 40, 40}, CornerRadii::uniform(kPill)}, kAcid.withAlpha(0.5f));
    list.fill(Rect{8, 100, 100, 1}, kBone);
    list.fill(Rect{8.5f, 108.5f, 20, 10}, kBone);
    list.fill(RoundedRect{{40, 108, 3, 14}, CornerRadii::uniform(kPill)}, kSky);
    const RgbaImage image = h.render(128, 128, list);
    expectPixel(image, 28, 20, kSky);
    expectPixel(image, 8, 40, kVoid);                          // Outside the pill's corner.
    expectPixel(image, 28, 48, kPink);
    expectPixel(image, 84, 76, over(kAcid.withAlpha(0.5f), kVoid));
    expectPixel(image, 57, 9, kLavender);                      // Square top-left corner.
    expectPixel(image, 119, 47, kVoid);                        // Rounded bottom-right corner.
    expectPixel(image, 50, 100, kBone);                        // A crisp 1px line.
    expectPixel(image, 8, 110, over(kBone.withAlpha(0.5f), kVoid), 3);  // Half-covered edge.
    h.golden("fills", image);
}

void testGradients(Harness &h) {
    DisplayList list;
    const GradientStops tri{{0, kPink}, {0.5f, kPurple}, {1, kBlue}};
    list.fill(RoundedRect{{4, 4, 56, 56}, CornerRadii::uniform(8)},
              LinearGradient{{4, 4}, {60, 60}, tri});
    list.fill(Rect{68, 4, 56, 56},
              RadialGradient{{96, 32}, 28, {{0, kSky}, {1, kSky.withAlpha(0)}}});
    list.fill(RoundedRect{{4, 68, 56, 56}, CornerRadii::uniform(kPill)},
              ConicGradient{{32, 96},
                            0,
                            {{0, Color::fromRgb(0xFF2E5F)},
                             {0.33f, Color::fromRgb(0xFFD21A)},
                             {0.66f, Color::fromRgb(0x22E6D2)},
                             {1, Color::fromRgb(0xFF2E5F)}}});
    list.fill(Rect{68, 68, 56, 56}, Color::fromRgb(0x0A1740));
    list.fill(Rect{68, 68, 56, 56}, Stripes{-std::numbers::pi_v<float> / 4, 6, 12, 0, kBone});
    const RgbaImage image = h.render(128, 128, list);
    expectPixel(image, 32, 32, kPurple, 8);                       // Midway along the diagonal.
    EXPECT(pixel(image, 10, 30)[0] > pixel(image, 50, 30)[0] + 40);  // Pink fades to blue.
    EXPECT(pixel(image, 10, 30)[2] < pixel(image, 50, 30)[2]);
    expectPixel(image, 96, 32, kSky, 6);
    expectPixel(image, 69, 5, kVoid, 2);  // Outside the radius.
    h.golden("gradients", image);
}

void testStrokes(Harness &h) {
    DisplayList list;
    list.stroke(RoundedRect{{8, 8, 48, 32}, {}}, 1, kBone);
    list.stroke(RoundedRect{{64, 8, 56, 32}, CornerRadii::uniform(12)}, 4, kSky);
    list.stroke(RoundedRect{{8, 48, 112, 20}, CornerRadii::uniform(kPill)}, 1.5f,
                LinearGradient{{8, 0}, {120, 0}, {{0, kPink}, {0.5f, kPurple}, {1, kBlue}}});
    list.stroke(RoundedRect{{8, 76, 44, 44}, CornerRadii::uniform(kPill)}, 10,
                ConicGradient{{30, 98}, 0, {{0, kViolet}, {0.5f, kBlue}, {1, kViolet}}});
    list.stroke(RoundedRect{{64, 76, 24, 24}, CornerRadii::uniform(6)}, 20, kAcid);
    list.stroke(RoundedRect{{96, 76, 24, 44}, {16, 0, 16, 0}}, 2, kLavender);
    const RgbaImage image = h.render(128, 128, list);
    expectPixel(image, 30, 24, kVoid);  // Inside the 1px border.
    expectPixel(image, 8, 24, kBone);   // On it.
    expectPixel(image, 92, 9, kSky);
    expectPixel(image, 92, 24, kVoid);
    expectPixel(image, 76, 88, kAcid);  // A stroke wider than half the shape fills it.
    h.golden("strokes", image);
}

void testShadows(Harness &h) {
    DisplayList list;
    const RoundedRect card{{16, 16, 64, 44}, CornerRadii::uniform(14)};
    list.shadow(card, Shadow{{0, 8}, 16, 0, Color{0, 0, 0, 0.5f}, false});
    list.fill(card, Color::fromRgb(0xFFFFFF));
    const RoundedRect hard{{96, 16, 48, 44}, CornerRadii::uniform(8)};
    list.shadow(hard, Shadow{{4, 4}, 0, 2, kPink, false});
    list.fill(hard, kBone);
    // A sunken track: inset 0 2px 6px rgba(0,0,0,.7).
    const RoundedRect track{{16, 80, 128, 20}, CornerRadii::uniform(kPill)};
    list.fill(track, Color::fromRgb(0x2A2A30));
    list.shadow(track, Shadow{{0, 2}, 6, 0, Color{0, 0, 0, 0.7f}, true});
    // A glass pill's specular edge: inset 0 1px 0 rgba(255,255,255,.8) and a bottom shade.
    const RoundedRect pill{{16, 112, 60, 28}, CornerRadii::uniform(kPill)};
    list.fill(pill, Color::fromRgb(0x30304A));
    list.shadow(pill, Shadow{{0, 1}, 0, 0, Color{1, 1, 1, 0.8f}, true});
    list.shadow(pill, Shadow{{0, -1}, 0, 0, Color{0, 0, 0, 0.45f}, true});
    // The glass panel's drop: 0 30px 60px -30px rgba(0,0,0,.9).
    const RoundedRect panel{{92, 108, 56, 40}, CornerRadii::uniform(12)};
    list.shadow(panel, Shadow{{0, 30}, 60, -30, Color{0, 0, 0, 0.9f}, false});
    list.fill(panel, Color::fromRgb(0xF5F5F0));
    const RgbaImage image = h.render(160, 160, list, kPaper);
    expectPixel(image, 48, 38, Color::fromRgb(0xFFFFFF));  // The shadow stays outside the shape.
    expectPixel(image, 4, 4, kPaper);                      // Far from any shadow.
    expectPixel(image, 147, 40, kPink);                    // Hard spread shadow, offset.
    EXPECT(luminance(image, 48, 70) < luminance(image, 48, 4) - 20);  // Under the card.
    EXPECT(luminance(image, 80, 82) < luminance(image, 80, 97) - 10);  // Inset: darker on top.
    h.golden("shadows", image);
}

void drawBusyBackground(DisplayList &list) {
    list.fill(Rect{0, 0, 128, 128}, Color::fromRgb(0x0A1740));
    list.fill(Rect{0, 0, 128, 128}, Stripes{0, 4, 8, 0, kBone.withAlpha(0.8f)});
    list.fill(RoundedRect{{10, 10, 50, 50}, CornerRadii::uniform(kPill)}, kPink);
    list.fill(RoundedRect{{70, 60, 50, 50}, CornerRadii::uniform(kPill)}, kAcid);
}

void testLiveBlur(Harness &h) {
    DisplayList list;
    drawBusyBackground(list);
    const RoundedRect glass{{24, 24, 80, 80}, CornerRadii::uniform(20)};
    list.backdropBlur(glass, 6, 1.5f);
    list.fill(glass, LinearGradient{{0, 24}, {0, 104}, {{0, Color{1, 1, 1, 0.085f}},
                                                        {1, Color{1, 1, 1, 0.025f}}}});
    list.stroke(glass, 1, Color{1, 1, 1, 0.1f});
    const RgbaImage image = h.render(128, 128, list);
    // The stripes (4px on, 4px off) are smoothed out under the glass but not outside it.
    float insideMin = 255;
    float insideMax = 0;
    float outsideMin = 255;
    float outsideMax = 0;
    for (int x = 40; x < 56; ++x) {
        insideMin = std::min(insideMin, luminance(image, x, 64));
        insideMax = std::max(insideMax, luminance(image, x, 64));
        outsideMin = std::min(outsideMin, luminance(image, x, 116));
        outsideMax = std::max(outsideMax, luminance(image, x, 116));
    }
    EXPECT(outsideMax - outsideMin > 100);
    EXPECT(insideMax - insideMin < 12);
    h.golden("blur_live", image);
}

void testBackgroundBlur(Harness &h) {
    auto target = h.renderer->createOffscreenTarget(128, 128);
    EXPECT(target.has_value());
    const auto frame = [](Color glow, float scroll) {
        DisplayList list;
        list.fill(Rect{0, 0, 128, 128}, RadialGradient{{0, 0}, 120, {{0, glow}, {1, glow.withAlpha(0)}}});
        list.fill(Rect{0, 0, 128, 128},
                  RadialGradient{{128, 128}, 100, {{0, kViolet.withAlpha(0.6f)}, {1, kViolet.withAlpha(0)}}});
        for (int i = 0; i < 128; i += 16) {
            list.fill(Rect{static_cast<float>(i), 0, 1, 128}, kBone.withAlpha(0.3f));
        }
        list.backgroundEnd();
        // Scrolling content, which the background blur must not see.
        for (int i = 0; i < 6; ++i) {
            list.fill(RoundedRect{{8, scroll + i * 24.0f, 112, 18}, CornerRadii::uniform(6)}, kAcid);
        }
        const RoundedRect glass{{16, 40, 96, 48}, CornerRadii::uniform(16)};
        list.backdropBlur(glass, 10, 1.6f, BlurSource::background);
        list.fill(glass, Color{1, 1, 1, 0.06f});
        return list;
    };

    h.timings.clear();
    const RgbaImage first = h.render(**target, frame(kPink.withAlpha(0.6f), 0));
    const RgbaImage second = h.render(**target, frame(kPink.withAlpha(0.6f), 7));
    EXPECT(h.timings.size() == 2);
    EXPECT(h.timings[0].blurs == 1);
    EXPECT(h.timings[1].blurs == 0);  // Reused: only the content scrolled.
    // Under the glass, the acid rows never show through the background blur.
    for (int x = 20; x < 108; x += 4) {
        const auto p = pixel(second, x, 64);
        EXPECT(p[1] < 200 || p[0] > 150);
    }
    const RgbaImage third = h.render(**target, frame(kSky.withAlpha(0.6f), 7));
    EXPECT(h.timings.size() == 3 && h.timings[2].blurs == 1);  // The background changed.
    h.golden("blur_background", first);
    h.golden("blur_background_changed", third);
}

void testBlurAccuracy(Harness &h) {
    // A hard edge blurred with standard deviation sigma crosses 15.9% and 84.1% two sigma apart.
    for (float sigma : {3.0f, 10.0f, 22.0f}) {
        DisplayList list;
        list.fill(Rect{0, 0, 128, 32}, Color{0, 0, 0, 1});
        list.fill(Rect{128, 0, 128, 32}, Color{1, 1, 1, 1});
        list.backdropBlur(RoundedRect{{0, 0, 256, 32}, {}}, sigma);
        const RgbaImage image = h.render(256, 32, list);
        float low = -1;
        float high = -1;
        for (int x = 1; x < 256; ++x) {
            const float a = static_cast<float>(pixel(image, x - 1, 16)[0]) / 255;
            const float b = static_cast<float>(pixel(image, x, 16)[0]) / 255;
            if (low < 0 && a < 0.1587f && b >= 0.1587f) {
                low = static_cast<float>(x) - 0.5f + (0.1587f - a) / (b - a) - 0.5f;
            }
            if (high < 0 && a < 0.8413f && b >= 0.8413f) {
                high = static_cast<float>(x) - 0.5f + (0.8413f - a) / (b - a) - 0.5f;
            }
        }
        const float measured = (high - low) / 2;
        std::printf("  blur sigma %.1f measured %.2f\n", sigma, measured);
        EXPECT(low > 0 && high > 0);
        EXPECT(std::abs(measured - sigma) < sigma * 0.2f + 0.5f);
    }
}

void testClipTransformOpacity(Harness &h) {
    auto icon = std::make_shared<DisplayList>();
    icon->fill(RoundedRect{{0, 0, 16, 16}, CornerRadii::uniform(4)}, kSky);
    icon->fill(Rect{4, 7, 8, 2}, kVoid);

    DisplayList list;
    list.pushClip(RoundedRect{{8, 8, 112, 112}, CornerRadii::uniform(24)});
    list.fill(Rect{0, 0, 128, 70}, Stripes{std::numbers::pi_v<float> / 4, 3, 9, 0,
                                            Color::fromRgb(0x0A1740)});
    list.pushTransform(rotation(std::numbers::pi_v<float> / 9, {64, 44}));
    list.fill(RoundedRect{{24, 20, 80, 48}, CornerRadii::uniform(10)}, kLavender);
    list.pushClip(RoundedRect{{32, 28, 64, 32}, CornerRadii::uniform(kPill)});
    list.fill(RoundedRect{{48, 16, 56, 56}, CornerRadii::uniform(kPill)}, kPink);
    list.pop();
    list.pop();
    // Group opacity: the overlap of the two discs is no darker than either.
    list.pushOpacity(0.5f);
    list.fill(RoundedRect{{16, 76, 36, 36}, CornerRadii::uniform(kPill)}, kAcid);
    list.fill(RoundedRect{{36, 76, 36, 36}, CornerRadii::uniform(kPill)}, kAcid);
    list.pop();
    // A single command's opacity folds into its color.
    list.pushOpacity(0.5f);
    list.fill(Rect{80, 76, 32, 16}, kAcid);
    list.pop();
    list.pushTransform(Transform::translation({80, 96}));
    list.draw(icon);
    list.pop();
    list.pushTransform(Transform::translation({100, 96}) * Transform::scale(1.25f, 1.25f));
    list.draw(icon);
    list.pop();
    list.pop();
    const RgbaImage image = h.render(128, 128, list);
    expectPixel(image, 2, 2, kVoid);                                // Clipped away.
    expectPixel(image, 10, 10, kVoid);                              // The clip's rounded corner.
    const Color half = over(kAcid.withAlpha(0.5f), kVoid);
    expectPixel(image, 24, 94, half);
    expectPixel(image, 44, 94, half);                               // The overlap.
    expectPixel(image, 96, 84, half);                               // Folded.
    expectPixel(image, 82, 98, kSky);                               // Nested list.
    h.golden("clip_transform_opacity", image);
}

void testImages(Harness &h) {
    auto &store = h.renderer->images();
    std::vector<std::uint8_t> checker(32 * 32 * 4);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            std::uint8_t *p = &checker[(y * 32 + x) * 4];
            const bool on = ((x / 8) + (y / 8)) % 2 == 0;
            p[0] = on ? 255 : 30;
            p[1] = on ? 79 : 200;
            p[2] = on ? 196 : 250;
            p[3] = static_cast<std::uint8_t>(y < 16 ? 255 : 255 - (y - 16) * 12);
        }
    }
    const ImageId id = store.add(32, 32, checker);
    const ImageId removed = store.add(32, 32, checker);
    store.remove(removed);
    EXPECT(id != 0);
    EXPECT(store.add(0, 4, {}) == 0);

    DisplayList list;
    list.image(id, Rect{8, 8, 48, 48}, CornerRadii::uniform(12));
    list.image(id, Rect{64, 8, 56, 56}, CornerRadii::uniform(kPill), 0.5f);
    list.image(id, Rect{8, 72, 112, 48}, {}, 1);
    list.image(removed, Rect{0, 0, 128, 128});  // Never drawn.
    list.image(9999, Rect{0, 0, 128, 128});     // Unknown: skipped.
    const RgbaImage image = h.render(128, 128, list);
    // Probes sit where bilinear filtering only mixes texels of one checker square.
    expectPixel(image, 12, 12, Color::fromRgb(0xFF4FC4));
    expectPixel(image, 8, 8, kVoid);  // Rounded corner.
    expectPixel(image, 85, 15, over(Color::fromRgb(0x1EC8FA, 0.5f), kVoid), 3);
    h.golden("images", image);

    store.remove(id);
    h.render(16, 16, DisplayList{});  // Processes the removal.
}

std::vector<std::uint8_t> solid(int width, int height, Color color) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = static_cast<std::uint8_t>(std::lround(color.r * 255));
        pixels[i + 1] = static_cast<std::uint8_t>(std::lround(color.g * 255));
        pixels[i + 2] = static_cast<std::uint8_t>(std::lround(color.b * 255));
        pixels[i + 3] = 255;
    }
    return pixels;
}

void testImageUpdates(Harness &h) {
    auto &store = h.renderer->images();
    auto target = h.renderer->createOffscreenTarget(64, 32);
    EXPECT(target.has_value());
    DisplayList list;
    const ImageId id = store.add(8, 8, solid(8, 8, kPink));
    list.image(id, Rect{0, 0, 32, 32});
    // Coalesces with the add before the renderer takes it.
    EXPECT(store.update(id, 8, 8, solid(8, 8, kSky)));
    expectPixel(h.render(**target, list), 16, 16, kSky);

    // Same size, with the previous frame still in flight: render without reading back first.
    store.update(id, 8, 8, solid(8, 8, kAcid));
    EXPECT(h.renderer->render(**target, list, kVoid).has_value());
    store.update(id, 8, 8, solid(8, 8, kLavender));
    expectPixel(h.render(**target, list), 16, 16, kLavender);

    // A new size replaces the image; the old one is freed once frames in flight are done.
    store.update(id, 4, 2, solid(4, 2, kBone));
    EXPECT(h.renderer->render(**target, list, kVoid).has_value());
    const RgbaImage resized = h.render(**target, list);
    expectPixel(resized, 16, 16, kBone);
    expectPixel(resized, 48, 16, kVoid);

    // Updates of removed or unknown images are ignored, as are invalid sizes.
    EXPECT(!store.update(id, 0, 4, {}));
    EXPECT(!store.update(id, 4, 4, std::vector<std::uint8_t>(8)));
    EXPECT(store.update(9999, 2, 2, solid(2, 2, kPink)));
    store.remove(id);
    store.update(id, 4, 2, solid(4, 2, kPink));
    const RgbaImage removed = h.render(**target, list);
    expectPixel(removed, 16, 16, kVoid);
    const ImageId gone = store.add(2, 2, solid(2, 2, kPink));
    store.remove(gone);
    store.update(gone, 2, 2, solid(2, 2, kPink));
    DisplayList goneList;
    goneList.image(gone, Rect{0, 0, 64, 32});
    expectPixel(h.render(**target, goneList), 16, 16, kVoid);
}

// Pixel frames stand in for hardware buffers: same slots, release timing and pipeline.
void testExternalImages(Harness &h) {
    using filepan::gfx::vulkan::ExternalFrame;
    auto &store = h.renderer->externalImages();
    EXPECT(!store.supported());  // No hardware buffers on a host.
    auto target = h.renderer->createOffscreenTarget(64, 32);
    EXPECT(target.has_value());
    int released[8] = {};
    const auto frame = [&](int index, Color left, Color right, filepan::gfx::Rect crop = {}) {
        // 4x2: the left half one color, the right half another.
        std::vector<std::uint8_t> pixels = solid(4, 2, left);
        const std::vector<std::uint8_t> other = solid(4, 2, right);
        for (int y = 0; y < 2; ++y) {
            std::copy_n(other.begin(), 8, pixels.begin() + (y * 4 + 2) * 4);
        }
        return ExternalFrame::fromPixels(4, 2, std::move(pixels), crop, [&released, index] { ++released[index]; });
    };

    const auto id = store.create();
    DisplayList list;
    list.fill(Rect{0, 0, 64, 32}, kPink);
    list.externalImage(id, Rect{0, 0, 32, 32});
    list.fill(Rect{40, 0, 8, 32}, kSky);  // After the external draw: the UI pipeline again.
    // Nothing presented yet: nothing drawn.
    RgbaImage image = h.render(**target, list);
    expectPixel(image, 16, 16, kPink);
    expectPixel(image, 44, 16, kSky);

    // The crop shows only the right half.
    store.present(id, frame(0, kAcid, kLavender, {2, 0, 2, 2}));
    image = h.render(**target, list);
    expectPixel(image, 2, 16, kLavender);
    expectPixel(image, 30, 16, kLavender);
    expectPixel(image, 44, 16, kSky);
    EXPECT(released[0] == 0);

    // A new frame: the old one is released only once the frames that drew it are done.
    store.present(id, frame(1, kBone, kBone));
    EXPECT(h.renderer->render(**target, list, kVoid).has_value());
    EXPECT(released[0] == 0);
    EXPECT(h.renderer->render(**target, list, kVoid).has_value());
    EXPECT(released[0] == 0);
    EXPECT(h.renderer->render(**target, list, kVoid).has_value());
    EXPECT(released[0] == 1);

    // Two frames before a render: the first is never drawn and goes at once.
    store.present(id, frame(2, kAcid, kAcid));
    store.present(id, frame(3, kViolet, kViolet));
    EXPECT(h.renderer->render(**target, list, kVoid).has_value());
    EXPECT(released[2] == 1);
    EXPECT(released[3] == 0);
    image = h.render(**target, list);  // Reading back waits for the GPU, releasing frame 1.
    EXPECT(released[1] == 1);
    expectPixel(image, 16, 16, kViolet);

    // Rounded and translucent, under a clip.
    DisplayList styled;
    styled.pushClip({Rect{0, 0, 64, 24}, {}});
    styled.externalImage(id, Rect{0, 0, 32, 32}, CornerRadii::uniform(8), 0.5f);
    styled.pop();
    image = h.render(**target, styled);
    expectPixel(image, 16, 12, over(kViolet.withAlpha(0.5f), kVoid), 3);
    expectPixel(image, 1, 1, kVoid);    // Rounded corner.
    expectPixel(image, 16, 28, kVoid);  // Clipped.

    // Destroying releases the frame on screen once the GPU is done; later frames go at once.
    store.destroy(id);
    store.present(id, frame(4, kBone, kBone));
    EXPECT(released[4] == 1);
    image = h.render(**target, list);
    EXPECT(released[3] == 1);
    expectPixel(image, 16, 16, kPink);
    store.present(9999, frame(5, kBone, kBone));
    EXPECT(released[5] == 1);
}

void testText(Harness &h) {
    FakeTextSystem &text = h.text;
    DisplayList list;
    list.text(filepan::test::fakeText(text, "FILEPAN 0123", 16), {8, 8}, kBone);
    list.text(filepan::test::fakeText(text, "abc", 16), {8.25f, 30}, kSky);
    list.text(filepan::test::fakeText(text, "abc", 16), {8.5f, 50}, kSky.withAlpha(0.7f));
    list.text(filepan::test::fakeText(text, "abc", 16), {8.75f, 70}, kPink);
    list.text(filepan::test::fakeText(text, "XY", 20, FakeTextSystem::kColorFace), {80, 30},
              kBone.withAlpha(0.8f));
    list.pushTransform(rotation(-std::numbers::pi_v<float> / 8, {100, 70}));
    list.text(filepan::test::fakeText(text, "ROT", 14), {80, 62}, kAcid);
    list.pop();
    list.pushOpacity(0.5f);
    list.text(filepan::test::fakeText(text, "fade", 12), {60, 80}, kBone);
    list.pop();
    auto target = h.renderer->createOffscreenTarget(160, 96);
    EXPECT(target.has_value());
    const RgbaImage image = h.render(**target, list);
    const int calls = text.rasterizeCalls;
    EXPECT(calls > 0);
    const RgbaImage again = h.render(**target, list);
    EXPECT(text.rasterizeCalls == calls);  // Glyphs come from the atlas the second time.
    EXPECT(filepan::test::compareImages(image, again, 0).pixelsOverTolerance == 0);
    // The first glyph is drawn, and nothing lands left of the text.
    float brightest = 0;
    for (int y = 8; y < 24; ++y) {
        for (int x = 8; x < 18; ++x) {
            brightest = std::max(brightest, luminance(image, x, y));
        }
    }
    EXPECT(brightest > 150);
    expectPixel(image, 4, 16, kVoid);
    h.golden("text", image);
}

void testDense(Harness &h) {
    // Thousands of instances outgrow both frame slots' initial buffers, frame after frame.
    auto target = h.renderer->createOffscreenTarget(128, 128);
    EXPECT(target.has_value());
    RgbaImage image;
    for (int frame = 0; frame < 3; ++frame) {
        DisplayList list;
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const float t = static_cast<float>(x + y + frame) / 128;
                list.fill(RoundedRect{{x * 2.0f, y * 2.0f, 2, 2}, {}},
                          Color{t, 1 - t, 0.5f, 1});
            }
        }
        image = h.render(**target, list);
        EXPECT(h.timings.back().drawCalls <= 1);
    }
    expectPixel(image, 0, 0, Color{2.0f / 128, 126.0f / 128, 0.5f, 1}, 40);
    h.golden("dense", image);
}

}  // namespace

// One target turned between portrait and landscape, as a window is on rotation: every render
// after a resize matches one into a fresh target of that size, so no scratch image (layers, blur
// levels, cached background blurs) is kept from the old size.
void testResize(Harness &h) {
    const auto frame = [](int width, int height, float scroll) {
        const float w = static_cast<float>(width);
        const float h = static_cast<float>(height);
        DisplayList list;
        list.fill(Rect{0, 0, w, h}, RadialGradient{{0, 0}, w, {{0, kPink.withAlpha(0.6f)}, {1, kPink.withAlpha(0)}}});
        for (int i = 0; i < width; i += 12) {
            list.fill(Rect{static_cast<float>(i), 0, 4, h}, kBone.withAlpha(0.5f));
        }
        list.backgroundEnd();
        for (int i = 0; i < 12; ++i) {
            list.fill(RoundedRect{{8, scroll + i * 24.0f, w - 16, 18}, CornerRadii::uniform(6)}, kAcid);
        }
        // A bar across the top on the background, and a sheet at the bottom on what's behind it.
        const RoundedRect bar{{0, 0, w, 36}, CornerRadii::uniform(0)};
        list.backdropBlur(bar, 10, 1.6f, BlurSource::background);
        list.fill(bar, Color{1, 1, 1, 0.06f});
        const RoundedRect sheet{{0, h - 60, w, 60}, CornerRadii{20, 20, 0, 0}};
        list.backdropBlur(sheet, 6, 1.5f);
        list.fill(sheet, Color{1, 1, 1, 0.08f});
        return list;
    };
    const auto same = [](const RgbaImage &a, const RgbaImage &b) {
        if (a.width != b.width || a.height != b.height) {
            return false;
        }
        int worst = 0;
        for (std::size_t i = 0; i < a.pixels.size(); ++i) {
            worst = std::max(worst, std::abs(static_cast<int>(a.pixels[i]) - static_cast<int>(b.pixels[i])));
        }
        if (worst > 1) {
            std::fprintf(stderr, "resized render differs by up to %d\n", worst);
        }
        return worst <= 1;
    };
    constexpr int kShort = 96;
    constexpr int kLong = 212;
    auto target = h.renderer->createOffscreenTarget(kShort, kLong);
    EXPECT(target.has_value());
    for (int turn = 0; turn < 4; ++turn) {
        const bool landscape = turn % 2 == 1;
        const int width = landscape ? kLong : kShort;
        const int height = landscape ? kShort : kLong;
        (*target)->resize(width, height);
        EXPECT((*target)->size().width == static_cast<float>(width) &&
               (*target)->size().height == static_cast<float>(height));
        // Twice: the second reuses the background blur cached at this size.
        for (float scroll : {0.0f, 5.0f}) {
            const RgbaImage resized = h.render(**target, frame(width, height, scroll));
            const RgbaImage fresh = h.render(width, height, frame(width, height, scroll));
            EXPECT(same(resized, fresh));
        }
    }
}

// Pre-rotation, as for a turned phone: a target whose image holds the frame turned a quarter (or
// two, or three) clockwise renders the same frame, turned. Once without a live blur over the
// frame itself, drawn turned directly, and once with one, drawn unturned and composited.
void testPreRotation(Harness &h) {
    constexpr int kWidth = 96;
    constexpr int kHeight = 212;
    const auto frame = [](bool live) {
        const float w = kWidth;
        const float hh = kHeight;
        DisplayList list;
        list.fill(Rect{0, 0, w, hh}, RadialGradient{{0, 0}, w, {{0, kPink.withAlpha(0.6f)}, {1, kPink.withAlpha(0)}}});
        for (int i = 0; i < kWidth; i += 12) {
            list.fill(Rect{static_cast<float>(i), 0, 4, hh}, kBone.withAlpha(0.5f));
        }
        list.backgroundEnd();
        for (int i = 0; i < 8; ++i) {
            list.fill(RoundedRect{{8, 40 + i * 20.0f, w - 16, 14}, CornerRadii::uniform(6)}, kAcid);
        }
        // Clips, a transform and a layer, which all look things up by position.
        list.pushClip(RoundedRect{{10, 50, 60, 60}, CornerRadii::uniform(18)});
        list.pushTransform(rotation(std::numbers::pi_v<float> / 9, {40, 80}));
        list.fill(RoundedRect{{14, 60, 60, 30}, CornerRadii::uniform(8)}, kLavender);
        list.pop();
        list.pop();
        list.pushOpacity(0.5f);
        list.fill(RoundedRect{{20, 120, 30, 30}, CornerRadii::uniform(kPill)}, kSky);
        list.fill(RoundedRect{{36, 120, 30, 30}, CornerRadii::uniform(kPill)}, kSky);
        list.pop();
        // A bar at the top on the background, and a sheet at the bottom on what's behind it.
        const RoundedRect bar{{0, 0, w, 36}, CornerRadii::uniform(0)};
        list.backdropBlur(bar, 10, 1.6f, BlurSource::background);
        list.fill(bar, Color{1, 1, 1, 0.06f});
        if (live) {
            const RoundedRect sheet{{0, hh - 60, w, 60}, CornerRadii{20, 20, 0, 0}};
            list.backdropBlur(sheet, 6, 1.5f);
            list.fill(sheet, Color{1, 1, 1, 0.08f});
        }
        return list;
    };
    for (bool live : {false, true}) {
        const RgbaImage upright = h.render(kWidth, kHeight, frame(live));
        for (int turns = 1; turns < 4; ++turns) {
            auto target = h.renderer->createOffscreenTarget(kWidth, kHeight, turns);
            EXPECT(target.has_value());
            EXPECT((*target)->size().width == kWidth && (*target)->size().height == kHeight);
            // Twice, so the second frame reuses the cached background blur.
            for (int pass = 0; pass < 2; ++pass) {
                EXPECT(h.renderer->render(**target, frame(live), kVoid).has_value());
                auto pixels = h.renderer->readPixels(**target);
                EXPECT(pixels.has_value());
                // The image is turned: kHeight x kWidth for odd turns. A quarter turn clockwise
                // takes the frame's pixel (x, y) to (kHeight - 1 - y, x).
                const int imageWidth = turns % 2 != 0 ? kHeight : kWidth;
                int worst = 0;
                for (int y = 0; y < kHeight; ++y) {
                    for (int x = 0; x < kWidth; ++x) {
                        int ix = x;
                        int iy = y;
                        if (turns == 1) {
                            ix = kHeight - 1 - y;
                            iy = x;
                        } else if (turns == 2) {
                            ix = kWidth - 1 - x;
                            iy = kHeight - 1 - y;
                        } else {
                            ix = y;
                            iy = kWidth - 1 - x;
                        }
                        const std::uint8_t *got =
                                &(*pixels)[(static_cast<std::size_t>(iy) * imageWidth + ix) * 4];
                        const auto want = pixel(upright, x, y);
                        int difference = 0;
                        for (int c = 0; c < 4; ++c) {
                            difference = std::max(difference, std::abs(int{got[c]} - want[c]));
                        }
                        worst = std::max(worst, difference);
                    }
                }
                // Only the dither, which hashes the image's pixel position, may differ.
                if (worst > 2) {
                    std::fprintf(stderr, "%s, %d turn(s): differs by up to %d\n", live ? "live" : "direct",
                                 turns, worst);
                }
                EXPECT(worst <= 2);
            }
        }
    }
}

int main() {
    Harness h;
    const std::pair<const char *, void (*)(Harness &)> tests[] = {
            {"fills", testFills},
            {"gradients", testGradients},
            {"strokes", testStrokes},
            {"shadows", testShadows},
            {"blur_live", testLiveBlur},
            {"blur_background", testBackgroundBlur},
            {"blur_accuracy", testBlurAccuracy},
            {"clip_transform_opacity", testClipTransformOpacity},
            {"images", testImages},
            {"image_updates", testImageUpdates},
            {"external_images", testExternalImages},
            {"text", testText},
            {"dense", testDense},
            {"resize", testResize},
            {"pre_rotation", testPreRotation},
    };
    for (const auto &[name, test] : tests) {
        std::printf("%s\n", name);
        test(h);
    }
    h.renderer->waitIdle();
    const std::uint32_t validationErrors = h.renderer->validationErrorCount();
    if (validationErrors > 0) {
        std::fprintf(stderr, "%u validation errors\n", validationErrors);
        return 1;
    }
    if (h.failures > 0) {
        std::fprintf(stderr, "%d golden mismatches\n", h.failures);
        return 1;
    }
    return 0;
}
