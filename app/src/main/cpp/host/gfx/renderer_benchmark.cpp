// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Renders a phone-sized frame of about 2000 mixed commands, like a scrolling file list under
// glass chrome, and reports CPU and GPU time. Not a ctest; run it by hand:
//
//   build/host/gfx/renderer_benchmark [frames] [width] [height]
//
// On lavapipe the GPU numbers measure a CPU rasterizer, so only the CPU side says much about a
// phone.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <numbers>
#include <variant>
#include <string>
#include <vector>

#include "filepan/gfx/vulkan/renderer.hpp"
#include "gfx_test_util.hpp"

using namespace filepan::gfx;
using filepan::gfx::vulkan::FrameTiming;
using filepan::gfx::vulkan::Renderer;
using filepan::test::FakeTextSystem;

namespace {

DisplayList buildFrame(FakeTextSystem &text, const std::vector<ImageId> &thumbnails, float scroll,
                       int width, int height, int &commands) {
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    DisplayList list;
    // Background: ground, glows, grid.
    list.fill(Rect{0, 0, w, h}, Color::fromRgb(0x070312));
    list.fill(Rect{0, 0, w, h}, RadialGradient{{0, 0}, 780, {{0, Color::fromRgb(0x38BDF8, 0.45f)},
                                                              {1, Color::fromRgb(0x38BDF8, 0)}}});
    list.fill(Rect{0, 0, w, h}, RadialGradient{{w, h}, 700, {{0, Color::fromRgb(0xA78BFA, 0.4f)},
                                                             {1, Color::fromRgb(0xA78BFA, 0)}}});
    for (float x = 0; x < w; x += 48) {
        list.fill(Rect{x, 0, 1, h}, Color::fromRgb(0xEEF2FF, 0.04f));
    }
    list.backgroundEnd();

    // A dense grid view: glass cards with a thumbnail, two lines of text, a tag and a barcode,
    // 11 commands each, in a scrolling clip.
    const auto name = filepan::test::fakeText(text, "holiday_2024.jpg", 20);
    const auto meta = filepan::test::fakeText(text, "4.2 MB  12:04", 16);
    const auto tag = filepan::test::fakeText(text, "NEW", 14);
    constexpr int kColumns = 5;
    const float cellWidth = (w - 48) / kColumns;
    const float rowHeight = 56;
    const float listTop = 200;
    const float listBottom = h - 200;
    list.pushClip(RoundedRect{{0, listTop, w, listBottom - listTop}, {}});
    list.pushTransform(Transform::translation({0, -scroll}));
    int index = 0;
    // Only rows overlapping the viewport are recorded, as the UI framework would.
    const int firstRow = static_cast<int>(scroll / rowHeight);
    const int lastRow = static_cast<int>((scroll + listBottom - listTop) / rowHeight);
    for (int row = firstRow; row <= lastRow; ++row) {
        const float y = listTop + static_cast<float>(row) * rowHeight;
        for (int column = 0; column < kColumns; ++column, ++index) {
            const float x = 24 + static_cast<float>(column) * cellWidth;
            const RoundedRect card{{x + 4, y + 4, cellWidth - 8, rowHeight - 8},
                                   CornerRadii::uniform(14)};
            list.shadow(card, Shadow{{0, 4}, 12, -3, Color{0, 0, 0, 0.5f}, false});
            list.fill(card, LinearGradient{{0, y}, {0, y + rowHeight},
                                           {{0, Color{1, 1, 1, 0.085f}}, {1, Color{1, 1, 1, 0.025f}}}});
            list.stroke(card, 1, Color{1, 1, 1, 0.1f});
            list.shadow(card, Shadow{{0, 1}, 0, 0, Color{1, 1, 1, 0.22f}, true});
            list.image(thumbnails[static_cast<std::size_t>(index) % thumbnails.size()],
                       Rect{x + 10, y + 10, 36, 36}, CornerRadii::uniform(8));
            list.text(name, {x + 54, y + 8}, Color::fromRgb(0xEEF2FF));
            list.text(meta, {x + 54, y + 30}, Color::fromRgb(0xEEF2FF, 0.7f));
            list.fill(RoundedRect{{x + cellWidth - 44, y + 10, 32, 16}, CornerRadii::uniform(4)},
                      Color::fromRgb(0x38BDF8));
            list.text(tag, {x + cellWidth - 40, y + 11}, Color::fromRgb(0x070312));
            list.fill(RoundedRect{{x + cellWidth - 44, y + 32, 32, 12}, {}},
                      Stripes{-std::numbers::pi_v<float> / 4, 3, 6, 0,
                              Color::fromRgb(0xEEF2FF, 0.5f)});
        }
    }
    list.pop();
    list.pop();

    // Chrome: a live-blurred top bar, a background-blurred bottom bar, a FAB with a glow.
    const RoundedRect top{{16, 40, w - 32, 140}, CornerRadii::uniform(28)};
    list.backdropBlur(top, 22, 1.5f);
    list.fill(top, Color{1, 1, 1, 0.06f});
    list.stroke(top, 2, Color{1, 1, 1, 0.1f});
    list.text(filepan::test::fakeText(text, "DOWNLOADS", 64), {48, 70}, Color::fromRgb(0xEEF2FF));
    const RoundedRect bottom{{16, h - 180, w - 32, 140}, CornerRadii::uniform(28)};
    list.backdropBlur(bottom, 22, 1.5f, BlurSource::background);
    list.fill(bottom, Color{1, 1, 1, 0.06f});
    const RoundedRect fab{{w - 200, h - 360, 144, 144}, CornerRadii::uniform(1e6f)};
    list.shadow(fab, Shadow{{0, 12}, 40, 0, Color::fromRgb(0x38BDF8, 0.5f), false});
    list.fill(fab, LinearGradient{{0, h - 360}, {0, h - 216}, {{0, Color::fromRgb(0x7DD3FC)},
                                                               {0.5f, Color::fromRgb(0x38BDF8)},
                                                               {1, Color::fromRgb(0x0284C7)}}});

    commands = 0;
    const std::function<void(const DisplayList &)> countCommands = [&](const DisplayList &l) {
        for (const auto &command : l.commands()) {
            ++commands;
            if (const auto *nested = std::get_if<DisplayListCommand>(&command)) {
                countCommands(*nested->list);
            }
        }
    };
    countCommands(list);
    return list;
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values.empty() ? 0 : values[values.size() / 2];
}

}  // namespace

int main(int argc, char **argv) {
    const int frames = argc > 1 ? std::atoi(argv[1]) : 60;
    const int width = argc > 2 ? std::atoi(argv[2]) : 1080;
    const int height = argc > 3 ? std::atoi(argv[3]) : 2400;
    FakeTextSystem text;
    auto renderer = Renderer::create(text);
    if (!renderer) {
        std::fprintf(stderr, "no Vulkan device: %s\n", renderer.error().message.c_str());
        return 77;
    }
    std::vector<FrameTiming> timings;
    (*renderer)->setFrameTimingCallback([&](const FrameTiming &t) { timings.push_back(t); });
    auto target = (*renderer)->createOffscreenTarget(width, height);
    if (!target) {
        std::fprintf(stderr, "%s\n", target.error().message.c_str());
        return 1;
    }
    std::vector<ImageId> thumbnails;
    for (int i = 0; i < 16; ++i) {
        std::vector<std::uint8_t> pixels(128 * 128 * 4);
        for (std::size_t p = 0; p < pixels.size(); p += 4) {
            pixels[p] = static_cast<std::uint8_t>(i * 16);
            pixels[p + 1] = static_cast<std::uint8_t>(p / 512);
            pixels[p + 2] = 180;
            pixels[p + 3] = 255;
        }
        thumbnails.push_back((*renderer)->images().add(128, 128, std::move(pixels)));
    }

    std::vector<double> buildMs;
    std::vector<double> wallMs;
    int commands = 0;
    for (int i = 0; i < frames + 5; ++i) {
        const auto start = std::chrono::steady_clock::now();
        const DisplayList list =
                buildFrame(text, thumbnails, static_cast<float>(i) * 7.0f, width, height, commands);
        const auto built = std::chrono::steady_clock::now();
        if (auto result = (*renderer)->render(**target, list, Color::fromRgb(0x070312)); !result) {
            std::fprintf(stderr, "%s\n", result.error().message.c_str());
            return 1;
        }
        // Measures each frame alone rather than pipelined, for steadier numbers.
        (*renderer)->waitIdle();
        const auto done = std::chrono::steady_clock::now();
        if (i >= 5) {
            buildMs.push_back(std::chrono::duration<double, std::milli>(built - start).count());
            wallMs.push_back(std::chrono::duration<double, std::milli>(done - built).count());
        }
    }
    std::vector<double> recordMs;
    std::vector<double> gpuMs;
    const FrameTiming &last = timings.back();
    for (std::size_t i = 5; i < timings.size(); ++i) {
        recordMs.push_back(static_cast<double>(timings[i].recordNs) / 1e6);
        if (timings[i].gpuNs >= 0) {
            gpuMs.push_back(static_cast<double>(timings[i].gpuNs) / 1e6);
        }
    }
    std::printf("device:            %s\n", (*renderer)->deviceName().c_str());
    std::printf("target:            %dx%d, %d frames\n", width, height, frames);
    std::printf("commands:          %d (instances %u, draw calls %u, render passes %u, blurs %u)\n",
                commands, last.instances, last.drawCalls, last.renderPasses, last.blurs);
    std::printf("record list (UI):  %.3f ms median\n", median(buildMs));
    std::printf("renderer CPU:      %.3f ms median (plan, glyphs, uploads, command recording)\n",
                median(recordMs));
    std::printf("GPU:               %.3f ms median\n", median(gpuMs));
    std::printf("submit to idle:    %.3f ms median\n", median(wallMs));
    return 0;
}
