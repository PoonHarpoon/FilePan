// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Golden images of the component kit, rendered the way the app renders: the real Vulkan renderer
// (lavapipe here) and the real text system with the bundled fonts, on a 1080x2400 phone at
// density 2.625 (411 dp wide).
//
//   FILEPAN_UPDATE_GOLDENS=1 build/host/ui_components/gallery_test   rewrites golden/*.png
//
// Every render is also written to golden-out/ in the build directory, for looking at.

#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "filepan/gfx/vulkan/renderer.hpp"
#include "filepan/text/text_system.hpp"
#include "filepan/ui/ui.hpp"
#include "gallery.hpp"
#include "gfx_test_util.hpp"

using namespace filepan;
using filepan::test::RgbaImage;

namespace {

constexpr int kWidth = 1080;
constexpr int kHeight = 2400;
constexpr float kDensity = 2.625f;

struct Harness {
    std::unique_ptr<gfx::TextSystem> text;
    std::unique_ptr<gfx::vulkan::Renderer> renderer;
    std::unique_ptr<gfx::vulkan::RenderTarget> target;
    int failures = 0;

    Harness() {
        text = filepan::text::createTextSystem(filepan::text::createDirectoryFontProvider(FILEPAN_FONTS_DIR));
        auto created = gfx::vulkan::Renderer::create(*text);
        if (!created) {
            test::skipOrFail(created.error().message);
        }
        renderer = std::move(*created);
        auto offscreen = renderer->createOffscreenTarget(kWidth, kHeight);
        EXPECT(offscreen.has_value());
        target = std::move(*offscreen);
    }

    // Builds a screen under preset (Clean in its dark or light tokens) and renders one frame of it.
    RgbaImage render(ui::ThemePreset preset, bool night, const std::function<test::Page()> &build) {
        ui::UiLoop loop;
        std::int64_t now = 1'000'000'000;
        loop.setClock([&] { return now; });
        ui::UiRoot root(loop, text.get());
        root.env().density.set(kDensity);
        root.env().windowSize.set({static_cast<float>(kWidth), static_cast<float>(kHeight)});
        root.env().safeArea.set({0, 63, 0, 63});  // A 24 dp status bar and a gesture bar.
        root.env().night.set(night);
        root.env().theme.set(ui::makeTheme(preset, std::nullopt, night));
        test::Page page = build();
        root.setContent(page.root);
        auto frame = root.frame(now);
        EXPECT(frame.has_value());
        std::shared_ptr<const gfx::DisplayList> list = frame->displayList;
        if (page.attached) {
            page.attached();
            // Settle anything that was started (animations snap: nothing here animates).
            for (int i = 0; i < 60; ++i) {
                now += 16'000'000;
                loop.runPending();
                if (auto next = root.frame(now)) {
                    list = next->displayList;
                }
            }
        }
        auto rendered = renderer->render(*target, *list, root.env().theme.peek().colors.ground);
        if (!rendered) {
            std::fprintf(stderr, "render failed: %s\n", rendered.error().message.c_str());
            std::exit(1);
        }
        auto pixels = renderer->readPixels(*target);
        EXPECT(pixels.has_value());
        return {kWidth, kHeight, std::move(*pixels)};
    }

    void check(const std::string &name, ui::ThemePreset preset, bool night,
               const std::function<test::Page()> &build) {
        const RgbaImage image = render(preset, night, build);
        const std::filesystem::path out(FILEPAN_TEST_OUTPUT_DIR);
        std::filesystem::create_directories(out);
        test::writePng(out / (name + ".png"), image);
        if (!test::matchesGolden(name, image)) {
            ++failures;
        }
    }
};

struct Look {
    ui::ThemePreset preset;
    bool night;
    const char *name;
};

constexpr Look kLooks[] = {{ui::ThemePreset::acid, true, "acid"},
                           {ui::ThemePreset::ultraviolet, true, "ultraviolet"},
                           {ui::ThemePreset::clean, true, "clean_dark"},
                           {ui::ThemePreset::clean, false, "clean_light"}};

}  // namespace

int main() {
    Harness h;
    const std::string only = std::getenv("FILEPAN_GALLERY_ONLY") ? std::getenv("FILEPAN_GALLERY_ONLY") : "";
    const auto want = [&](const std::string &name) { return only.empty() || name.find(only) != std::string::npos; };
    // Every page in the default preset; the pages that show the most tokens in the others (the
    // goldens are large, so not every page in every preset).
    struct Entry {
        const char *name;
        test::Page (*build)();
        bool allPresets;
    };
    const Entry pages[] = {
            {"gallery_controls", test::galleryControls, true}, {"gallery_marks", test::galleryMarks, true},
            {"gallery_overlays", test::galleryOverlays, false}, {"gallery_menus", test::galleryMenus, false},
            {"gallery_dialog", test::galleryDialog, false},     {"browsing", test::browsingMock, true},
    };
    for (const Look &look : kLooks) {
        const std::string suffix = std::string("_") + look.name;
        for (const Entry &page : pages) {
            const std::string name = page.name + suffix;
            if ((page.allPresets || look.preset == ui::ThemePreset::acid) && want(name)) {
                h.check(name, look.preset, look.night, page.build);
            }
        }
    }
    if (h.failures > 0) {
        std::fprintf(stderr, "%d golden(s) differ\n", h.failures);
        return 1;
    }
    return 0;
}
