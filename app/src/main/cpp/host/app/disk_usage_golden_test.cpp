// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Golden images of the disk usage screen (the adirstat module), rendered with the real Vulkan
// renderer like app_golden_test, treemap included. Same switches: FILEPAN_UPDATE_GOLDENS
// and FILEPAN_GOLDEN_ONLY.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "app_harness.hpp"
#include "filepan/app/adirstat/disk_usage_screen.hpp"
#include "filepan/app/format.hpp"
#include "filepan/gfx/vulkan/renderer.hpp"
#include "gfx_test_util.hpp"

using namespace filepan;
using namespace filepan::test;
using app::DiskUsageScreen;
using filepan::adirstat::kNoNode;
using filepan::adirstat::NodeId;
namespace fs = std::filesystem;

namespace {

// A phone's shared storage, small enough to scan in a moment and varied enough to fill a treemap.
struct Storage {
    TempDir dir;
    fs::path home;
    fs::path files;

    Storage() {
        home = dir.path() / "storage";
        files = dir.path() / "files";
        fs::create_directories(files);
        for (int i = 0; i < 24; ++i) {
            makeFile(home / "DCIM" / "Camera" / ("IMG_" + std::to_string(20260100 + i) + ".jpg"),
                     1'800'000 + static_cast<std::size_t>(i) * 210'000, 2026, 9, 1 + i % 28);
        }
        for (int i = 0; i < 4; ++i) {
            makeFile(home / "DCIM" / "Camera" / ("VID_" + std::to_string(20260200 + i) + ".mp4"),
                     38'000'000 + static_cast<std::size_t>(i) * 9'000'000, 2026, 10, 2 + i);
        }
        makeFile(home / "Movies" / "Trip to the coast.mkv", 92'000'000, 2026, 6, 14);
        makeFile(home / "Movies" / "Recital.mp4", 41'000'000, 2026, 5, 3);
        for (int i = 0; i < 18; ++i) {
            makeFile(home / "Music" / "Album" / ("Track " + std::to_string(i + 1) + ".flac"),
                     24'000'000 + static_cast<std::size_t>(i % 5) * 3'100'000, 2026, 3, 9);
        }
        makeFile(home / "Download" / "firmware-update.zip", 61'000'000, 2026, 10, 20);
        makeFile(home / "Download" / "maps-offline.tar.zst", 33'000'000, 2026, 10, 1);
        makeFile(home / "Download" / "app-release.apk", 27'000'000, 2026, 9, 28);
        for (int i = 0; i < 9; ++i) {
            makeFile(home / "Documents" / ("Report " + std::to_string(i + 1) + ".pdf"),
                     900'000 + static_cast<std::size_t>(i) * 400'000, 2026, 8, 1 + i);
        }
        makeFile(home / "Documents" / "Notes.md", 42'000, 2026, 10, 30);
        for (int i = 0; i < 14; ++i) {
            makeFile(home / "Android" / "media" / "chat" / ("voice-" + std::to_string(i) + ".opus"),
                     400'000 + static_cast<std::size_t>(i) * 90'000, 2026, 10, 1 + i);
            makeFile(home / "Android" / "media" / "chat" / ("photo-" + std::to_string(i) + ".webp"),
                     700'000 + static_cast<std::size_t>(i) * 60'000, 2026, 10, 1 + i);
        }
        makeFile(home / "Android" / "obb" / "game" / "main.obb", 48'000'000, 2026, 4, 2);
    }
};

NodeId find(const adirstat::Tree &tree, NodeId parent, std::string_view name) {
    for (NodeId id = tree[parent].firstChild; id != kNoNode; id = tree[id].nextSibling) {
        if (tree.name(id) == name) {
            return id;
        }
    }
    return kNoNode;
}

struct Page {
    const char *name;
    void (*show)(DiskUsageScreen &screen);
    // After the show step is laid out, if set.
    void (*then)(DiskUsageScreen &screen) = nullptr;
};

const Page kPages[] = {
        {"folders",
         [](DiskUsageScreen &screen) {
             const adirstat::Tree &tree = *screen.tree();
             const NodeId dcim = find(tree, tree.root(), "DCIM");
             screen.setFocus(find(tree, find(tree, dcim, "Camera"), "VID_20260203.mp4"));
         }},
        {"types",
         [](DiskUsageScreen &screen) {
             screen.setView(DiskUsageScreen::View::types);
             screen.tapItem(0);
         }},
        {"scrolled",
         [](DiskUsageScreen &screen) {
             // Far down a long folder: the back-to-top button shows.
             const adirstat::Tree &tree = *screen.tree();
             screen.toggleExpanded(find(tree, tree.root(), "DCIM"));
             screen.toggleExpanded(find(tree, find(tree, tree.root(), "DCIM"), "Camera"));
         },
         [](DiskUsageScreen &screen) { screen.list().scrollTo(4000); }},
        {"selection",
         [](DiskUsageScreen &screen) {
             const adirstat::Tree &tree = *screen.tree();
             const NodeId download = find(tree, tree.root(), "Download");
             screen.toggleSelected(find(tree, download, "firmware-update.zip"));
             screen.toggleSelected(find(tree, find(tree, tree.root(), "Movies"), "Recital.mp4"));
         }},
};

struct Look {
    ui::ThemePreset preset;
    bool night;
    const char *name;
};

constexpr Look kLooks[] = {{ui::ThemePreset::acid, true, "acid"}, {ui::ThemePreset::clean, false, "clean_light"}};

}  // namespace

int main() {
    app::setCurrentYearForTesting(2026);
    std::unique_ptr<gfx::TextSystem> text =
            text::createTextSystem(text::createDirectoryFontProvider(FILEPAN_FONTS_DIR), {.locale = "en"});
    auto created = gfx::vulkan::Renderer::create(*text);
    if (!created) {
        skipOrFail(created.error().message);
    }
    auto renderer = std::move(*created);
    auto target = renderer->createOffscreenTarget(AppHarness::kWidth, AppHarness::kHeight);
    EXPECT(target.has_value());
    const char *onlyEnv = std::getenv("FILEPAN_GOLDEN_ONLY");
    const std::string only = onlyEnv ? onlyEnv : "";
    int failures = 0;
    for (const Look &look : kLooks) {
        for (const Page &page : kPages) {
            const std::string name = std::string("disk_usage_") + page.name + "_" + look.name;
            if (!only.empty() && name.find(only) == std::string::npos) {
                continue;
            }
            Storage storage;
            app::AppOptions options;
            // Sparse files, and file systems that report blocks late, would make the on-disk total vary.
            options.screens = app::diskUsageScreenSpecs({.showAllocated = false});
            AppHarness h(storage.home.string(), storage.files.string(), std::move(options), false, text.get());
            h.start();
            h.root->env().night.set(look.night);
            h.state().updateSettings([&](app::Settings &s) { s.preset = look.preset; });
            h.pump(30);
            h.state().navigate(vfs::Location::local(storage.home.string()));
            h.settle(100);
            h.state().openScreen(app::kDiskUsageScreen);
            h.pump(50);
            auto *screen = static_cast<DiskUsageScreen *>(h.app->screenNode());
            EXPECT(screen != nullptr && screen->debugName() == "DiskUsageScreen");
            EXPECT(h.waitUntil(
                    [&] { return screen->phase() == DiskUsageScreen::Phase::done && !screen->tiles().empty(); }));
            page.show(*screen);
            if (page.then != nullptr) {
                h.pump(50);
                page.then(*screen);
            }
            h.pump(10);
            h.pump(600);
            EXPECT(h.lastFrame != nullptr);
            auto rendered = renderer->render(**target, *h.lastFrame, h.root->env().theme.peek().colors.ground);
            if (!rendered) {
                std::fprintf(stderr, "render failed: %s\n", rendered.error().message.c_str());
                return 1;
            }
            auto pixels = renderer->readPixels(**target);
            EXPECT(pixels.has_value());
            const RgbaImage image{AppHarness::kWidth, AppHarness::kHeight, std::move(*pixels)};
            const fs::path out(FILEPAN_TEST_OUTPUT_DIR);
            fs::create_directories(out);
            writePng(out / (name + ".png"), image);
            if (!matchesGolden(name, image)) {
                ++failures;
            }
        }
    }
    if (failures > 0) {
        std::fprintf(stderr, "%d golden(s) differ\n", failures);
        return 1;
    }
    std::puts("disk_usage_golden_test: OK");
    return 0;
}
