// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Golden images of the app itself, rendered the way the app renders: the real Vulkan renderer
// (lavapipe here) and the real text system with the bundled fonts, on a 1080x2400 phone at
// density 2.625, over a temporary directory tree made to look like a phone's storage, in Acid
// (the default), some of them in Ultraviolet too.
//
//   FILEPAN_UPDATE_GOLDENS=1 build/host/app/app_golden_test   rewrites golden/*.png
//   FILEPAN_GOLDEN_ONLY=<substring>                             renders only matching pages
//
// Every render is also written to golden-out/ in the build directory, for looking at.

#include <archive.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app_harness.hpp"
#include "filepan/app/favorites_sheet.hpp"
#include "filepan/app/file_operations.hpp"
#include "filepan/app/format.hpp"
#include "filepan/ui/components/text_field.hpp"
#include "filepan/create_archive.hpp"
#include "filepan/gfx/vulkan/renderer.hpp"
#include "filepan/runtime/shared_items.hpp"
#include "gfx_test_util.hpp"

using namespace filepan;
using namespace filepan::test;
namespace fs = std::filesystem;

namespace {

// Internal / Download / Papers and friends, with the mock's names, sizes and dates.
struct Storage {
    TempDir dir;
    fs::path home;

    Storage() {
        home = dir.path() / "emulated";
        const fs::path download = home / "Download";
        const fs::path papers = download / "Papers";
        const auto files = [](const fs::path &folder, int count, int day) {
            for (int i = 0; i < count; ++i) {
                makeFile(folder / ("item-" + std::to_string(i)), 10, 2026, 9, 1);
            }
            makeFolder(folder, 2026, day < 20 ? 10 : 9, day);
        };
        files(papers / "Conferences", 8, 3);
        files(papers / "Drafts", 14, 2);
        files(papers / "Receipts", 112, 28);
        makeFile(papers / "quarterly-report-2026-q3-final-revised.pdf", 2'400'000, 2026, 10, 12);
        makeFile(papers / "IMG_20261012_184455.jpg", 4'100'000, 2026, 10, 12);
        makeFile(papers / "keynote-recording.mp4", 812'000'000, 2026, 10, 11);
        makeFile(papers / "dataset-v4.tar.zst", 1'200'000'000, 2026, 10, 9);
        makeFile(papers / "interview-notes.md", 18'000, 2026, 10, 8);
        makeFile(papers / "filepan-0.4.0-arm64.apk", 24'600'000, 2026, 10, 6);
        makeFile(papers / "podcast-ep12.flac", 96'000'000, 2026, 10, 2);
        makeFile(papers / "render.cpp", 42'000, 2026, 10, 1);
        makeFile(papers / "slides-final.pptx", 6'100'000, 2026, 9, 30);
        makeFile(papers / "budget-2027.xlsx", 88'000, 2026, 9, 29);
        makeFolder(papers, 2026, 10, 12);
        makeFolder(download, 2026, 10, 12);
        makeFolder(home / "DCIM" / "Camera" / "2026" / "october-coast-trip-raw-exports", 2026, 10, 12);
        makeFolder(home / "DCIM" / "Camera" / "2026", 2026, 10, 12);
        makeFolder(home / "DCIM" / "Camera", 2026, 10, 12);
        makeFolder(home / "Music", 2026, 8, 1);
        makeFolder(home / "Empty", 2026, 10, 1);
        makeFolder(home, 2026, 1, 1);
    }

    [[nodiscard]] std::string path(const std::string &relative = {}) const {
        return relative.empty() ? home.string() : (home / relative).string();
    }
};

struct Page {
    const char *name;
    // Sets up the screen once the app has started.
    std::function<void(AppHarness &, const Storage &)> show;
    // Decode thumbnails with fakeThumbnail; elsewhere there are none, as on a host.
    bool thumbnails = false;
};

// Stands in for an app icon: a green disc with soft edges on transparency.
app::DecodedImage fakeIcon() {
    constexpr int kSize = 48;
    app::DecodedImage image{kSize, kSize, std::vector<std::uint8_t>(kSize * kSize * 4, 0)};
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - kSize / 2.0f;
            const float dy = static_cast<float>(y) + 0.5f - kSize / 2.0f;
            const float coverage = std::clamp(kSize * 0.42f - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
            std::uint8_t *p = image.rgba.data() + (y * kSize + x) * 4;
            p[0] = 61;
            p[1] = 220;
            p[2] = 132;
            p[3] = static_cast<std::uint8_t>(coverage * 255 + 0.5f);
        }
    }
    return image;
}

// Stands in for a photo and a video frame: a sky-to-ground gradient, warm for images and cool
// for videos; and for an APK's icon.
std::optional<app::DecodedImage> fakeThumbnail(const std::string &path, int) {
    if (path.ends_with(".apk")) {
        return fakeIcon();
    }
    const bool video = path.ends_with(".mp4");
    const std::array<float, 3> top = video ? std::array{0.16f, 0.45f, 0.62f} : std::array{0.95f, 0.62f, 0.38f};
    const std::array<float, 3> bottom = video ? std::array{0.05f, 0.10f, 0.22f} : std::array{0.36f, 0.18f, 0.42f};
    constexpr int kSize = 32;
    app::DecodedImage image{kSize, kSize, std::vector<std::uint8_t>(kSize * kSize * 4)};
    for (int y = 0; y < kSize; ++y) {
        const float t = static_cast<float>(y) / (kSize - 1);
        for (int x = 0; x < kSize; ++x) {
            std::uint8_t *p = image.rgba.data() + (y * kSize + x) * 4;
            for (int c = 0; c < 3; ++c) {
                p[c] = static_cast<std::uint8_t>((top[c] + (bottom[c] - top[c]) * t) * 255 + 0.5f);
            }
            p[3] = 255;
        }
    }
    return image;
}

// Waits for the thumbnails and the rows they rebind.
void settleThumbnails(AppHarness &h) {
    h.waitUntil([&] { return h.app->context().thumbnails.pending() == 0; });
    h.pump(100);
}

vfs::Location local(const std::string &path) { return vfs::Location::local(path); }

// Three tabs like the mock, the first on Papers.
void tabs(AppHarness &h, const Storage &s) {
    auto &state = h.state();
    state.navigate(local(s.path("Download")));
    state.navigate(local(s.path("Download/Papers")));
    const std::uint64_t first = state.activeId.peek();
    state.addTab(local(s.path("DCIM")));
    state.addTab(local(s.path("Music")));
    state.selectTab(first);
}

app::FileOperations &ops(AppHarness &h) { return static_cast<app::FileOperations &>(h.app->context().actions); }

app::FileTargets select(AppHarness &h, std::vector<std::string> names) {
    h.state().setSelection(std::move(names));
    return h.browse().selectionTargets();
}

// A job's progress as the queue would report it, for a job the jobs model is told about.
void publish(AppHarness &h, jobs::JobProgress progress, std::string title) {
    ops(h).jobs().track(progress.id, {.title = std::move(title)});
    h.services->jobEvents.publish(progress);
}

jobs::JobProgress copying() {
    return {.id = 901,
            .kind = jobs::JobKind::kCopy,
            .state = jobs::JobState::kRunning,
            .bytesDone = 456'000'000,
            .bytesTotal = 1'200'000'000,
            .itemsDone = 12,
            .itemsTotal = 40,
            .currentItem = "Download/Papers/keynote-recording.mp4",
            .bytesPerSecond = 38'000'000,
            .eta = std::chrono::seconds(20)};
}

ui::Node *find(ui::Node &root, const std::function<bool(ui::Node &)> &pred) {
    if (!root.style().display) {
        return nullptr;
    }
    if (pred(root)) {
        return &root;
    }
    for (const auto &child : root.children()) {
        if (ui::Node *found = find(*child, pred)) {
            return found;
        }
    }
    return nullptr;
}

// A folder, a long file name, a folder whose path runs long, and one on a server.
void addFavorites(AppHarness &h, const Storage &s) {
    auto &state = h.state();
    state.addFavorite({"Camera", local(s.path("DCIM/Camera"))});
    state.addFavorite({"quarterly-report-2026-q3-final-revised.pdf",
                       local(s.path("Download/Papers/quarterly-report-2026-q3-final-revised.pdf")),
                       app::Favorite::Kind::file});
    state.addFavorite({"Coast trip", local(s.path("DCIM/Camera/2026/october-coast-trip-raw-exports"))});
    state.addFavorite({"media", vfs::Location::sftp("nas", "/srv/media")});
}

void settleAll(AppHarness &h) {
    h.settle(200);
    h.waitUntil([&] { return h.browse().model().pendingCounts() == 0; });
    h.pump(600);
}

const Page kPages[] = {
        {"home",
         [](AppHarness &h, const Storage &s) {
             h.state().addFavorite({"Papers", local(s.path("Download/Papers"))});
             h.state().addFavorite({"Camera", local(s.path("DCIM/Camera"))});
             h.state().addFavorite({"quarterly-report-2026-q3-final-revised.pdf",
                                    local(s.path("Download/Papers/quarterly-report-2026-q3-final-revised.pdf")),
                                    app::Favorite::Kind::file});
             h.state().navigateHome();
         }},
        {"browse_list",
         [](AppHarness &h, const Storage &s) {
             tabs(h, s);
             settleAll(h);
             h.state().setSelection({"Receipts", "quarterly-report-2026-q3-final-revised.pdf", "keynote-recording.mp4"});
         }},
        {"browse_grid",
         [](AppHarness &h, const Storage &s) {
             tabs(h, s);
             h.state().peekActive()->view.set(app::ViewMode::grid);
         }},
        {"browse_pull_armed",
         [](AppHarness &h, const Storage &s) {
             tabs(h, s);
             settleAll(h);
             // Pulled past the threshold and held: the indicator takes the accent.
             const gfx::Vec2 start = h.center(*h.rowFor("Receipts"));
             h.pointer(ui::PointerAction::down, start);
             for (int i = 1; i <= 20; ++i) {
                 h.pointer(ui::PointerAction::move, {start.x, start.y + 12.0f * static_cast<float>(i)});
                 h.pump(10);
             }
         }},
        {"overflow_menu",
         [](AppHarness &h, const Storage &s) {
             tabs(h, s);
             settleAll(h);
             h.app->showOverflow();
         }},
        {"browse_band",
         [](AppHarness &h, const Storage &s) {
             tabs(h, s);
             settleAll(h);
             // Three on the clipboard, and a copy running.
             ops(h).copy(select(h, {"Receipts", "quarterly-report-2026-q3-final-revised.pdf", "keynote-recording.mp4"}));
             h.app->context().overlays.snackbars().dismissCurrent();
             publish(h, copying(), "Copy 40 items to Camera");
         }},
        {"browse_share",
         [](AppHarness &h, const Storage &s) {
             tabs(h, s);
             settleAll(h);
             // Three files shared from another app, waiting for a folder.
             runtime::SharedItems share;
             for (const char *name : {"boarding-pass.pdf", "IMG_2041.jpg", "notes.txt"}) {
                 share.items.push_back({.name = name, .text = "x"});
             }
             share.fromOtherApp = true;
             h.services->shares.deliver(std::move(share));
             h.app->context().overlays.snackbars().dismissCurrent();
         }},
        {"conflict_sheet",
         [](AppHarness &h, const Storage &s) {
             tabs(h, s);
             settleAll(h);
             // Download has an older copy of the report.
             makeFile(s.home / "Download" / "quarterly-report-2026-q3-final-revised.pdf", 2'100'000, 2026, 9, 30);
             ops(h).copy(select(h, {"quarterly-report-2026-q3-final-revised.pdf"}));
             ops(h).paste(local(s.path("Download")));
             h.waitUntil([&] { return h.app->context().overlays.topSheet() != nullptr; });
         }},
        {"jobs_sheet",
         [](AppHarness &h, const Storage &s) {
             tabs(h, s);
             settleAll(h);
             publish(h, copying(), "Copy 40 items to Camera");
             publish(h,
                     {.id = 902,
                      .kind = jobs::JobKind::kMove,
                      .state = jobs::JobState::kWaitingForDecision,
                      .bytesDone = 18'000'000,
                      .bytesTotal = 96'000'000,
                      .itemsDone = 3,
                      .itemsTotal = 112,
                      .currentItem = "Download/Papers/Receipts/2026-09-12.pdf"},
                     "Move “Receipts” to Archive");
             publish(h, {.id = 903, .kind = jobs::JobKind::kExtractArchive, .state = jobs::JobState::kQueued},
                     "Extract “dataset-v4.tar.zst”");
             publish(h,
                     {.id = 904,
                      .kind = jobs::JobKind::kCreateArchive,
                      .state = jobs::JobState::kSucceeded,
                      .bytesDone = 3'200'000,
                      .bytesTotal = 3'200'000,
                      .itemsDone = 3,
                      .itemsTotal = 3},
                     "Compress 3 items into “papers.zip”");
             publish(h,
                     {.id = 905,
                      .kind = jobs::JobKind::kDelete,
                      .state = jobs::JobState::kFailed,
                      .error = Error{EROFS, {}}},
                     "Delete “old-builds”");
             ops(h).showJobs();
         }},
        {"properties_sheet",
         [](AppHarness &h, const Storage &s) {
             // A file inside an archive: no owner or creation time that would differ by machine.
             struct Quiet final : CreateArchiveCallbacks {
                 bool onProgress(std::uint64_t, std::uint64_t, std::string_view) override { return true; }
                 bool onSourceError(std::string_view, int) override { return false; }
             } quiet;
             const fs::path zip = s.home / "Download" / "conference-kit.zip";
             createArchive(zip.string(), {.format = ARCHIVE_FORMAT_ZIP, .filter = ARCHIVE_FILTER_NONE},
                           {{s.path("Download/Papers/slides-final.pptx"), "slides-final.pptx"}}, quiet);
             h.state().navigate(local(s.path("Download")));
             h.state().navigate(vfs::Location::archive(local(zip.string())));
             settleAll(h);
             ops(h).properties(select(h, {"slides-final.pptx"}));
             h.pump(500);
             auto *sheet = h.app->context().overlays.topSheet();
             auto *compute = find(sheet->panel(), [](ui::Node &n) { return n.semantics().label == "Compute checksums"; });
             h.tap(*compute);
             h.waitUntil([&] {
                 return find(sheet->panel(), [](ui::Node &n) { return n.semantics().label.starts_with("SHA-256, "); });
             });
         }},
        {"rename_dialog",
         [](AppHarness &h, const Storage &s) {
             tabs(h, s);
             settleAll(h);
             ops(h).rename(select(h, {"quarterly-report-2026-q3-final-revised.pdf"}));
         }},
        // Onto a taken name, with "Replace existing" on.
        {"rename_replace_dialog",
         [](AppHarness &h, const Storage &s) {
             tabs(h, s);
             settleAll(h);
             ops(h).rename(select(h, {"quarterly-report-2026-q3-final-revised.pdf"}));
             h.pump(400);
             ui::Node *card = find(*h.app->root(), [](ui::Node &n) { return n.debugName() == "Dialog"; });
             auto &editor = static_cast<ui::components::TextField &>(
                                    *find(*card, [](ui::Node &n) { return n.debugName() == "TextField"; }))
                                    .editor();
             const std::string taken = "keynote-recording.mp4";
             editor.updateEditingValue({.text = taken, .selection = {static_cast<std::int32_t>(taken.size()),
                                                                     static_cast<std::int32_t>(taken.size())}});
             const auto replace = [&] {
                 return find(*card, [](ui::Node &n) {
                     return n.debugName() == "Switch" && n.semantics().label == "Replace existing";
                 });
             };
             h.waitUntil([&] { return replace() != nullptr; });
             h.tap(*replace());
         }},
        {"compress_sheet",
         [](AppHarness &h, const Storage &s) {
             tabs(h, s);
             settleAll(h);
             ops(h).compress(select(h, {"Receipts", "quarterly-report-2026-q3-final-revised.pdf", "keynote-recording.mp4"}));
         }},
        {"tabs_sheet",
         [](AppHarness &h, const Storage &s) {
             tabs(h, s);
             settleAll(h);
             h.app->showTabs();
         }},
        {"favorites_sheet",
         [](AppHarness &h, const Storage &s) {
             tabs(h, s);
             addFavorites(h, s);
             settleAll(h);
             app::showFavorites(h.app->context());
         }},
        {"favorites_edit",
         [](AppHarness &h, const Storage &s) {
             tabs(h, s);
             addFavorites(h, s);
             settleAll(h);
             app::showFavorites(h.app->context());
             h.pump(400);
             auto *sheet = h.app->context().overlays.topSheet();
             h.tap(*find(sheet->panel(), [](ui::Node &n) { return n.semantics().label == "Edit favorites"; }));
         }},
        // Hidden files shown: a dot file and two the user hid, dimmed, the latter saying so.
        {"browse_hidden",
         [](AppHarness &h, const Storage &s) {
             makeFile(s.home / "Download" / "Papers" / ".nomedia", 0, 2026, 9, 1);
             h.state().setHidden({local(s.path("Download/Papers/Drafts")),
                                  local(s.path("Download/Papers/budget-2027.xlsx"))},
                                 true);
             h.state().updateSettings([](app::Settings &st) {
                 st.showHidden = true;
                 st.sort.key = vfs::SortKey::kName;
             });
             tabs(h, s);
         }},
        {"deep_path",
         [](AppHarness &h, const Storage &s) {
             // Too long for the bar: the start goes behind an ellipsis.
             h.state().navigate(local(s.path("DCIM/Camera/2026/october-coast-trip-raw-exports")));
         }},
        {"empty",
         [](AppHarness &h, const Storage &s) {
             h.state().navigate(local(s.path("Empty")));
         }},
        // Thumbnails of the photo and the video, which is marked with a play glyph, and the APK's
        // icon, drawn whole on its transparency.
        {"browse_thumbnails",
         [](AppHarness &h, const Storage &s) {
             tabs(h, s);
             settleAll(h);
             settleThumbnails(h);
         },
         true},
        {"browse_thumbnails_grid",
         [](AppHarness &h, const Storage &s) {
             tabs(h, s);
             h.state().peekActive()->view.set(app::ViewMode::grid);
             settleAll(h);
             settleThumbnails(h);
         },
         true},
};

// Every page in Acid, the default, and in Clean, whose layout is its own; only kOtherLookPages in
// Ultraviolet, which shares Acid's (the goldens are large).
constexpr std::string_view kOtherLookPages[] = {"home", "browse_list", "browse_grid", "browse_share", "overflow_menu",
                                                "properties_sheet"};

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
    // Dates in the renders leave out this year whatever the clock says.
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
            if (look.preset == ui::ThemePreset::ultraviolet &&
                std::ranges::find(kOtherLookPages, std::string_view(page.name)) == std::end(kOtherLookPages)) {
                continue;
            }
            const std::string name = std::string("app_") + page.name + "_" + look.name;
            if (!only.empty() && name.find(only) == std::string::npos) {
                continue;
            }
            Storage storage;
            TempDir files;
            app::AppOptions options;
            options.decoder = page.thumbnails ? app::ImageDecoder(fakeThumbnail) : app::ImageDecoder();
            AppHarness h(storage.path(), files.path().string(), options, false, text.get());
            // Images the app uploads (only fakeThumbnail's) go up with the frame.
            h.imageStore = &renderer->images();
            h.start();
            h.root->env().night.set(look.night);
            h.state().updateSettings([&](app::Settings &s) { s.preset = look.preset; });
            // Applied before the page opens, as it would be by the time anyone opens one.
            h.pump(30);
            page.show(h, storage);
            settleAll(h);
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
    std::puts("app_golden_test: OK");
    return 0;
}
