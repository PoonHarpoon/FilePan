// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Turning the phone mid-session: the window goes between 1080x2400 and 2400x1080 (with the
// system bars moving to the sides) while the list is scrolled, a sheet, a menu or a dialog is
// open, or the viewer is showing. Every frame is also rendered on the real renderer (lavapipe
// here) into a target resized to match, as the shell's surface would be.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "app_harness.hpp"
#include "filepan/app/file_operations.hpp"
#include "filepan/app/sftp_screens.hpp"
#include "filepan/app/viewer_screen.hpp"
#include "filepan/gfx/vulkan/renderer.hpp"
#include "gfx_test_util.hpp"

using namespace filepan;
using namespace filepan::test;
namespace fs = std::filesystem;

namespace {

constexpr float kDensity = AppHarness::kDensity;

int failures = 0;

// Like EXPECT, but carries on, so one run shows everything a turn breaks.
#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "%s:%d: expectation failed: %s\n", __FILE__, __LINE__, #condition); \
            ++failures; \
        } \
    } while (false)

struct Tree {
    TempDir dir;
    fs::path home;
    fs::path files;

    Tree() {
        home = dir.path() / "storage";
        files = dir.path() / "files";
        fs::create_directories(files);
        for (int i = 0; i < 300; ++i) {
            makeFile(home / "many" / ("file-" + std::to_string(1000 + i) + ".txt"), 10, 2026, 1, 1);
        }
        for (const char *name : {"a.jpg", "b.png", "c.mp4"}) {
            makeFile(home / "Pictures" / name, 1000, 2026, 10, 1);
        }
        makeFolder(home / "many", 2026, 1, 1);
        makeFolder(home / "Pictures", 2026, 1, 1);
    }

    [[nodiscard]] std::string path(const std::string &relative) const { return (home / relative).string(); }
};

// A 4000x3000 still that decodes at once.
class FakeImage final : public media::ImageSource {
public:
    [[nodiscard]] const media::ImageInfo &info() const override { return info_; }
    media::MediaResult<std::optional<media::ImageFrame>> next() override {
        if (done_) {
            return std::nullopt;
        }
        done_ = true;
        return media::ImageFrame{4, 3, std::vector<std::uint8_t>(48, 200), 0};
    }
    media::MediaResult<void> rewind() override {
        done_ = false;
        return {};
    }

private:
    media::ImageInfo info_{4000, 3000, 4000, 3000, false, -1};
    bool done_ = false;
};

app::AppOptions fakeMedia() {
    app::AppOptions options;
    options.viewer.imageOpener = [](const media::MediaSource &, const media::DecodeLimits &)
            -> media::MediaResult<std::unique_ptr<media::ImageSource>> { return std::make_unique<FakeImage>(); };
    return options;
}

app::FileOperations &ops(AppHarness &h) { return static_cast<app::FileOperations &>(h.app->context().actions); }

// The renderer, shared by every test; null without a Vulkan device.
struct Gpu {
    std::unique_ptr<gfx::TextSystem> text =
            text::createTextSystem(text::createDirectoryFontProvider(FILEPAN_FONTS_DIR), {.locale = "en"});
    std::unique_ptr<gfx::vulkan::Renderer> renderer;
    std::unique_ptr<gfx::vulkan::RenderTarget> target;
    int renders = 0;

    Gpu() {
        if (auto created = gfx::vulkan::Renderer::create(*text)) {
            renderer = std::move(*created);
            if (auto made = renderer->createOffscreenTarget(AppHarness::kWidth, AppHarness::kHeight)) {
                target = std::move(*made);
            } else {
                renderer.reset();
            }
        }
    }
};

Gpu *gpu = nullptr;

struct Window {
    int width;
    int height;
    ui::Insets safe;
};

// Portrait with the status and navigation bars top and bottom; landscape (turned left) with the
// cutout and the navigation bar on the sides.
constexpr Window kPortrait{1080, 2400, {0, 63, 0, 63}};
constexpr Window kLandscape{2400, 1080, {126, 63, 63, 0}};

gfx::Size windowDp(const Window &w) { return {w.width / kDensity, w.height / kDensity}; }

// Renders the harness's last frame into the shared target at the window's size.
void render(AppHarness &h, const Window &w, const char *what) {
    if (gpu == nullptr || !gpu->renderer || !h.lastFrame) {
        return;
    }
    gpu->target->resize(w.width, w.height);
    const gfx::Size size = gpu->target->size();
    CHECK(size.width == static_cast<float>(w.width) && size.height == static_cast<float>(w.height));
    auto rendered = gpu->renderer->render(*gpu->target, *h.lastFrame, h.root->env().theme.peek().colors.ground);
    if (!rendered) {
        std::fprintf(stderr, "%s: render failed: %s\n", what, rendered.error().message.c_str());
    }
    CHECK(rendered.has_value());
    ++gpu->renders;
    if (const char *dump = std::getenv("FILEPAN_DUMP_RENDERS"); dump && *dump == '1') {
        if (auto pixels = gpu->renderer->readPixels(*gpu->target)) {
            writePng(std::string("rotation_") + what + "_" + std::to_string(w.width) + "x" +
                             std::to_string(w.height) + ".png",
                     {w.width, w.height, std::move(*pixels)});
        }
    }
}

// What the shell sends on a rotation: the surface's new size, then the moved insets.
void turn(AppHarness &h, const Window &w, const char *what) {
    h.root->env().windowSize.set({static_cast<float>(w.width), static_cast<float>(w.height)});
    h.root->env().safeArea.set(w.safe);
    // The surface's first frame at the new size, before anything has animated.
    h.loop.runPending();
    h.frame();
    render(h, w, what);
    h.pump(500);
    render(h, w, what);
}

bool near(float a, float b, float tolerance = 0.5f) { return std::abs(a - b) <= tolerance; }

// The first shown node with the debug name, depth first.
ui::Node *findNamed(ui::Node &root, std::string_view name) {
    if (!root.style().display) {
        return nullptr;
    }
    if (root.debugName() == name) {
        return &root;
    }
    for (const auto &child : root.children()) {
        if (ui::Node *found = findNamed(*child, name)) {
            return found;
        }
    }
    return nullptr;
}

// The node's rectangle in window dp.
gfx::Rect inWindow(const ui::Node &node) {
    const gfx::Transform t = ui::components::nodeToWindow(node, kDensity);
    return {t.tx / kDensity, t.ty / kDensity, node.frame().width, node.frame().height};
}

void checkScreen(AppHarness &h, const Window &w, const char *what) {
    const gfx::Size size = windowDp(w);
    const gfx::Rect root = h.app->root()->frame();
    if (!near(root.width, size.width) || !near(root.height, size.height)) {
        std::fprintf(stderr, "%s: root %gx%g, window %gx%g\n", what, root.width, root.height, size.width,
                     size.height);
    }
    CHECK(near(root.width, size.width) && near(root.height, size.height));
}

void checkList(AppHarness &h, const Window &w, const char *what) {
    auto &list = h.browse().list();
    const gfx::Rect frame = inWindow(list);
    const gfx::Size size = windowDp(w);
    CHECK(near(frame.width, size.width));
    // The bound rows fill the list's width and cover what's on screen.
    const float offset = list.offset();
    float top = 1e9f;
    float bottom = -1e9f;
    int rows = 0;
    for (const auto &child : list.children()) {
        if (!child->style().display || child->frame().height <= 0) {
            continue;
        }
        ++rows;
        top = std::min(top, child->frame().y - offset);
        bottom = std::max(bottom, child->frame().bottom() - offset);
    }
    // Up to the end of the content: the last row may stop short of the list's bottom padding.
    const float end = std::min(list.frame().height, list.contentExtent() - offset - list.style().padding.bottom);
    if (rows == 0 || (top > 1 && offset >= 1) || bottom < end - 1 || offset > list.maxOffset() + 0.5f) {
        std::fprintf(stderr, "%s: %d rows cover %g..%g of %g dp at offset %g (max %g)\n", what, rows, top, bottom,
                     end, offset, list.maxOffset());
    }
    CHECK(rows > 0 && (top <= 1 || offset < 1) && bottom >= end - 1 && offset <= list.maxOffset() + 0.5f);
}

// An open sheet fits the window and rests on its bottom edge.
void checkSheet(ui::components::BottomSheet &sheet, const Window &w, const char *what) {
    const gfx::Size size = windowDp(w);
    const gfx::Rect panel = inWindow(sheet.panel());
    const float top = panel.y + sheet.offset();
    if (!near(panel.width, size.width) || top < -0.5f || !near(panel.bottom() + sheet.offset(), size.height) ||
        !near(sheet.offset(), 0)) {
        std::fprintf(stderr, "%s: sheet panel %g,%g %gx%g offset %g in a %gx%g window\n", what, panel.x, panel.y,
                     panel.width, panel.height, sheet.offset(), size.width, size.height);
    }
    CHECK(near(panel.width, size.width));
    CHECK(near(sheet.offset(), 0));
    CHECK(top >= -0.5f);
    CHECK(near(panel.bottom(), size.height));
    // Never taller than 90% of the window, whichever way up it was opened.
    if (sheet.panel().style().maxHeight != size.height * 0.9f) {
        std::fprintf(stderr, "%s: sheet max height %g in a %g dp window\n", what, sheet.panel().style().maxHeight,
                     size.height);
    }
    CHECK(near(sheet.panel().style().maxHeight, size.height * 0.9f));
    CHECK(panel.height <= size.height * 0.9f + 0.5f);
}

void testScrolledList() {
    Tree tree;
    AppHarness h(tree.path("many"), tree.files.string(), {}, true, gpu ? gpu->text.get() : nullptr);
    h.state().navigate(vfs::Location::local(tree.path("many")));
    h.settle(100);
    render(h, kPortrait, "list");
    h.browse().list().scrollToItem(150);
    h.pump(100);
    const auto anchor = [&] {
        auto &list = h.browse().list();
        return h.browse().model().visible()[0].name.empty() ? 0.0f : list.offset();
    };
    CHECK(anchor() > 1000);
    for (int i = 0; i < 3; ++i) {
        turn(h, kLandscape, "list");
        checkScreen(h, kLandscape, "list landscape");
        checkList(h, kLandscape, "list landscape");
        turn(h, kPortrait, "list");
        checkScreen(h, kPortrait, "list portrait");
        checkList(h, kPortrait, "list portrait");
    }
    // Scrolled to the very end, then turned: the end stays in view, not past it.
    h.browse().list().scrollToItem(h.browse().model().visible().size() - 1);
    h.pump(100);
    turn(h, kLandscape, "list end");
    checkList(h, kLandscape, "list end landscape");
    turn(h, kPortrait, "list end");
    checkList(h, kPortrait, "list end portrait");

    // The grid's columns follow the width.
    h.state().peekActive()->view.set(app::ViewMode::grid);
    h.settle(200);
    turn(h, kLandscape, "grid");
    checkScreen(h, kLandscape, "grid landscape");
    checkList(h, kLandscape, "grid landscape");
    turn(h, kPortrait, "grid");
    checkList(h, kPortrait, "grid portrait");
}

void testSheets() {
    Tree tree;
    AppHarness h(tree.path("many"), tree.files.string(), {}, true, gpu ? gpu->text.get() : nullptr);
    h.state().navigate(vfs::Location::local(tree.path("many")));
    h.settle(100);

    // A sheet opened in portrait, turned to landscape and back.
    h.app->showTabs();
    h.pump(600);
    auto *sheet = h.app->context().overlays.topSheet();
    CHECK(sheet != nullptr);
    checkSheet(*sheet, kPortrait, "tabs portrait");
    turn(h, kLandscape, "tabs");
    checkSheet(*sheet, kLandscape, "tabs landscape");
    turn(h, kPortrait, "tabs");
    checkSheet(*sheet, kPortrait, "tabs portrait again");
    CHECK(h.app->back());
    h.pump(600);
    CHECK(h.app->context().overlays.topSheet() == nullptr);

    // One opened in landscape: as tall as it needs, but never taller than the window.
    turn(h, kLandscape, "properties");
    h.state().setSelection({"file-1000.txt"});
    ops(h).properties(h.browse().selectionTargets());
    h.pump(600);
    sheet = h.app->context().overlays.topSheet();
    CHECK(sheet != nullptr);
    checkSheet(*sheet, kLandscape, "properties landscape");
    turn(h, kPortrait, "properties");
    checkSheet(*sheet, kPortrait, "properties portrait");
    turn(h, kLandscape, "properties");
    checkSheet(*sheet, kLandscape, "properties landscape again");
    CHECK(h.app->back());
    h.pump(600);

    // The overflow menu, turned while open: its anchor moved, so it closes, as the platform's
    // popups do, and back goes on to the screen.
    turn(h, kPortrait, "menu");
    h.app->showOverflow();
    h.pump(300);
    CHECK(h.app->context().overlays.openCount.peek() == 1);
    turn(h, kLandscape, "menu");
    CHECK(h.app->context().overlays.openCount.peek() == 0);
    CHECK(findNamed(*h.app->root(), "Menu") == nullptr);

    // A dialog stays centred in the window.
    h.state().setSelection({"file-1000.txt"});
    ops(h).rename(h.browse().selectionTargets());
    h.pump(500);
    ui::Node *dialog = findNamed(*h.app->root(), "Dialog");
    CHECK(dialog != nullptr);
    if (dialog != nullptr) {
        for (const Window &w : {kPortrait, kLandscape}) {
            turn(h, w, "dialog");
            const gfx::Size size = windowDp(w);
            const gfx::Rect card = inWindow(static_cast<ui::components::Dialog *>(dialog)->card());
            if (!near(card.x + card.width / 2, size.width / 2, 1) || card.y < 0 || card.bottom() > size.height) {
                std::fprintf(stderr, "dialog card %g,%g %gx%g in %gx%g\n", card.x, card.y, card.width, card.height,
                             size.width, size.height);
            }
            CHECK(near(card.x + card.width / 2, size.width / 2, 1) && card.y >= 0 && card.bottom() <= size.height);
        }
    }
    CHECK(h.app->back());
    h.pump(400);
}

// The overflow menu is taller than a landscape window: opened there it's capped to the room
// below its button inside the safe area, and its content scrolls to the last item, by touch,
// by keyboard and by a screen reader's scroll action.
void testTallMenu() {
    Tree tree;
    AppHarness h(tree.path("many"), tree.files.string(), {}, true, gpu ? gpu->text.get() : nullptr);
    h.state().navigate(vfs::Location::local(tree.path("many")));
    h.settle(100);
    turn(h, kLandscape, "tall menu");
    const gfx::Size size = windowDp(kLandscape);
    const float areaTop = kLandscape.safe.top / kDensity;
    const float areaBottom = size.height - kLandscape.safe.bottom / kDensity;
    const float areaLeft = kLandscape.safe.left / kDensity;
    const float areaRight = size.width - kLandscape.safe.right / kDensity;

    const auto open = [&] {
        h.app->showOverflow();
        h.pump(300);
        auto *menu = static_cast<ui::components::Menu *>(findNamed(*h.app->root(), "Menu"));
        CHECK(menu != nullptr);
        return menu;
    };
    auto *menu = open();
    if (menu == nullptr) {
        return;
    }
    render(h, kLandscape, "tall menu");
    const gfx::Rect card = inWindow(menu->card());
    if (card.x < areaLeft || card.y < areaTop || card.right() > areaRight || card.bottom() > areaBottom) {
        std::fprintf(stderr, "menu card %g,%g %gx%g in a safe area %g,%g..%g,%g\n", card.x, card.y, card.width,
                     card.height, areaLeft, areaTop, areaRight, areaBottom);
    }
    CHECK(card.x >= areaLeft && card.y >= areaTop && card.right() <= areaRight && card.bottom() <= areaBottom);
    // Capped, so it scrolls, from its first item.
    ui::ScrollNode &scroll = menu->scroll();
    CHECK(scroll.maxOffset() > 0 && scroll.offset() == 0);
    CHECK(scroll.semantics().role == ui::Role::scroll);
    CHECK((scroll.semantics().actions & static_cast<std::uint32_t>(ui::Action::scrollForward)) != 0);
    ui::Node &last = *scroll.children().back();
    const std::string lastLabel = last.semantics().label;
    CHECK(!lastLabel.empty());
    // Out of view at first: below the card's bottom.
    CHECK(inWindow(last).y >= card.bottom());

    // A drag up scrolls to the end, and the last item is then inside the card, and taps.
    const gfx::Vec2 start{card.x + card.width / 2, card.bottom() - 30};
    h.pointer(ui::PointerAction::down, start);
    for (int step = 1; step <= 10; ++step) {
        h.pump(16);
        h.pointer(ui::PointerAction::move, {start.x, start.y - 30.0f * static_cast<float>(step)});
    }
    h.pump(16);
    h.pointer(ui::PointerAction::up, {start.x, start.y - 300});
    h.pump(600);
    CHECK(near(scroll.offset(), scroll.maxOffset(), 1));
    gfx::Rect row = inWindow(last);
    if (row.y < card.y || row.bottom() > card.bottom() + 0.5f) {
        std::fprintf(stderr, "last item %g..%g in a card %g..%g, offset %g of %g\n", row.y, row.bottom(), card.y,
                     card.bottom(), scroll.offset(), scroll.maxOffset());
    }
    CHECK(row.y >= card.y && row.bottom() <= card.bottom() + 0.5f);
    render(h, kLandscape, "tall menu scrolled");
    h.tap({row.x + row.width / 2, row.y + row.height / 2});
    h.pump(400);
    CHECK(findNamed(*h.app->root(), "Menu") == nullptr);
    {
        const app::Route route = h.state().peekActive()->current();
        CHECK(route.kind == app::Route::Kind::screen);
    }
    h.app->back();
    h.pump(400);

    // By keyboard: back from the first item wraps to the last, which scrolls into view.
    menu = open();
    if (menu == nullptr) {
        return;
    }
    ui::ScrollNode &again = menu->scroll();
    CHECK(again.offset() == 0);
    ui::Node *first = ui::components::firstFocusable(menu->card());
    CHECK(first != nullptr);
    first->requestFocus();
    h.root->dispatchKey({.keyCode = ui::keys::kTab, .action = ui::KeyAction::down, .modifiers = ui::modifiers::kShift});
    h.root->dispatchKey({.keyCode = ui::keys::kTab, .action = ui::KeyAction::up, .modifiers = ui::modifiers::kShift});
    h.pump(600);
    ui::Node &lastAgain = *again.children().back();
    CHECK(lastAgain.focused());
    CHECK(near(again.offset(), again.maxOffset(), 1));
    const gfx::Rect cardAgain = inWindow(menu->card());
    row = inWindow(lastAgain);
    CHECK(row.y >= cardAgain.y && row.bottom() <= cardAgain.bottom() + 0.5f);
    // And forward again wraps to the first (a sort chip in the header), scrolled back into view.
    h.root->dispatchKey({.keyCode = ui::keys::kTab, .action = ui::KeyAction::down});
    h.pump(600);
    CHECK(first->focused() && again.offset() < again.maxOffset());
    row = inWindow(*first);
    CHECK(row.y >= cardAgain.y && row.bottom() <= cardAgain.bottom() + 0.5f);

    // A screen reader pages it with the scroll action, from the top to the end.
    again.scrollTo(0);
    h.pump(50);
    for (int page = 0; page < 4 && again.offset() < again.maxOffset() - 0.5f; ++page) {
        CHECK(again.performDefaultAction(ui::Action::scrollForward, {}));
        h.pump(400);
    }
    CHECK(near(again.offset(), again.maxOffset(), 1));
    h.app->back();
    h.pump(300);
    CHECK(findNamed(*h.app->root(), "Menu") == nullptr);
}

void testViewer() {
    Tree tree;
    AppHarness h(tree.path("Pictures"), tree.files.string(), fakeMedia(), true, gpu ? gpu->text.get() : nullptr);
    h.state().navigate(vfs::Location::local(tree.path("Pictures")));
    h.settle(100);
    ui::Node *row = h.rowFor("b.png");
    CHECK(row != nullptr);
    h.tap(*row);
    h.pump(300);
    CHECK(h.app->viewer() != nullptr);
    app::ViewerScreen &viewer = *h.app->viewer();
    CHECK(h.waitUntil([&] { return viewer.page(1) && viewer.page(1)->image()->image() != 0; }));
    for (const Window &w : {kLandscape, kPortrait, kLandscape}) {
        turn(h, w, "viewer");
        checkScreen(h, w, "viewer");
        const gfx::Size size = windowDp(w);
        const gfx::Rect stage = inWindow(viewer.stage());
        if (!near(stage.width, size.width) || !near(stage.height, size.height)) {
            std::fprintf(stderr, "viewer stage %g,%g %gx%g in %gx%g\n", stage.x, stage.y, stage.width, stage.height,
                         size.width, size.height);
        }
        CHECK(near(stage.x, 0) && near(stage.y, 0) && near(stage.width, size.width) &&
               near(stage.height, size.height));
        // The current page fills the stage.
        const gfx::Rect page = inWindow(*viewer.page(1));
        if (!near(page.x, 0) || !near(page.width, size.width) || !near(page.height, size.height)) {
            std::fprintf(stderr, "viewer page %g,%g %gx%g in %gx%g\n", page.x, page.y, page.width, page.height,
                         size.width, size.height);
        }
        CHECK(near(page.x, 0) && near(page.width, size.width) && near(page.height, size.height));
        CHECK(viewer.index() == 1);
    }
    // Swiping still lands on the next page after the turn.
    const gfx::Size size = windowDp(kLandscape);
    const gfx::Vec2 start{size.width / 2, size.height / 2};
    h.pointer(ui::PointerAction::down, start);
    for (int step = 1; step <= 6; ++step) {
        h.pump(8);
        h.pointer(ui::PointerAction::move, {start.x - 300.0f * static_cast<float>(step) / 6, start.y});
    }
    h.pointer(ui::PointerAction::up, {start.x - 300, start.y});
    h.pump(500);
    CHECK(viewer.index() == 2);
    const gfx::Rect page = inWindow(*viewer.page(2));
    if (!near(page.x, 0)) {
        std::fprintf(stderr, "viewer page after swipe at %g\n", page.x);
    }
    CHECK(near(page.x, 0));
    CHECK(h.app->back());
    h.pump(300);
    turn(h, kPortrait, "after viewer");
    checkList(h, kPortrait, "after viewer");
}

// The pixels of the harness's last frame at the window's size; empty without a Vulkan device.
std::optional<RgbaImage> snapshot(AppHarness &h, const Window &w) {
    if (gpu == nullptr || !gpu->renderer || !h.lastFrame) {
        return std::nullopt;
    }
    gpu->target->resize(w.width, w.height);
    if (!gpu->renderer->render(*gpu->target, *h.lastFrame, h.root->env().theme.peek().colors.ground)) {
        return std::nullopt;
    }
    ++gpu->renders;
    auto pixels = gpu->renderer->readPixels(*gpu->target);
    if (!pixels) {
        return std::nullopt;
    }
    return RgbaImage{w.width, w.height, std::move(*pixels)};
}

// Runs the loop for ms as the shell does: a frame only when one was asked for.
void pumpRequested(AppHarness &h, int ms) {
    h.root->setFrameSink([&h](const ui::UiRoot::Frame &frame) { h.lastFrame = frame.displayList; });
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    do {
        h.loop.runPending();
        h.loop.runFrame(h.loop.now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < end);
    h.root->setFrameSink(nullptr);
}

// The SFTP screens after the window changes under them (turned, or the keyboard coming up or
// going) look as they do opened at that size: nothing kept from the size they were opened at.
void testSftpScreensResized() {
    const auto servers = [](AppHarness &h) {
        vfs::SftpEndpoint server{.host = "10.0.4.17", .port = 2222, .username = "ci"};
        vfs::SftpEndpoint bastion{.host = "bastion.example.org", .username = "ops"};
        h.state().updateSettings([&](app::Settings &s) {
            s.servers = {{.id = "build-box", .name = "Build box", .server = server, .jumpHosts = {bastion}},
                         {.id = "nas", .name = "Home NAS", .server = {.host = "nas.local", .username = "me"}}};
        });
    };
    struct Case {
        const char *name;
        std::function<void(AppHarness &)> open;
    };
    const Case cases[] = {
            {"sftp list",
             [&](AppHarness &h) {
                 servers(h);
                 h.state().openScreen(app::kServersScreen);
             }},
            {"sftp editor",
             [&](AppHarness &h) {
                 servers(h);
                 h.state().openScreen(app::kServersScreen);
                 h.pump(50);
                 static_cast<app::ServersScreen *>(h.app->screenNode())->edit("build-box");
             }},
            {"sftp editor with keys",
             [&](AppHarness &h) {
                 h.state().openScreen(app::kServersScreen);
                 h.pump(50);
                 static_cast<app::ServersScreen *>(h.app->screenNode())->add();
                 h.pump(50);
                 auto *editor = static_cast<app::ServerEditor *>(h.app->screenNode());
                 editor->setPrivateKey(std::nullopt, true);
                 editor->addJump();
                 editor->setPrivateKey(0, true);
                 h.pump(50);
                 // A key pasted into the server's multi-line field, wrapped to its width.
                 const std::string key = kTestPrivateKey;
                 editor->field("Private key")->editor().updateEditingValue(
                         {key, {static_cast<std::int32_t>(key.size()), static_cast<std::int32_t>(key.size())}, {}});
             }},
    };
    // The keyboard up, in px, as the shell sends it: over the navigation bar.
    const ui::Insets portraitIme{0, 0, 0, 900};
    const ui::Insets landscapeIme{0, 0, 0, 560};
    struct Step {
        const char *name;
        Window window;
        ui::Insets ime;
    };
    const Step steps[] = {
            {"keyboard up", kPortrait, portraitIme},
            {"turned with the keyboard up", kLandscape, landscapeIme},
            {"keyboard down", kLandscape, {}},
            {"turned back", kPortrait, {}},
    };
    Tree tree;
    for (const Case &c : cases) {
        AppHarness h(tree.path("many"), tree.files.string(), {}, true, gpu ? gpu->text.get() : nullptr);
        c.open(h);
        h.settle(400);
        for (const Step &step : steps) {
            const std::string what = std::string(c.name) + ", " + step.name;
            h.root->env().windowSize.set({static_cast<float>(step.window.width), static_cast<float>(step.window.height)});
            h.root->env().safeArea.set(step.window.safe);
            h.root->env().imeInsets.set(step.ime);
            pumpRequested(h, 600);
            const auto changed = snapshot(h, step.window);

            AppHarness fresh(tree.path("many"), tree.files.string(), {}, false, gpu ? gpu->text.get() : nullptr);
            fresh.root->env().windowSize.set(
                    {static_cast<float>(step.window.width), static_cast<float>(step.window.height)});
            fresh.root->env().safeArea.set(step.window.safe);
            fresh.root->env().imeInsets.set(step.ime);
            fresh.start();
            c.open(fresh);
            fresh.settle(600);
            const auto expected = snapshot(fresh, step.window);

            // The page's column: its bottom room follows the keyboard.
            ui::Node *page = h.app->screenNode();
            ui::Node *freshPage = fresh.app->screenNode();
            CHECK(page != nullptr && freshPage != nullptr);
            if (page == nullptr || freshPage == nullptr) {
                continue;
            }
            {
                // Every node where it would be had the screen been opened at this size.
                int shown = 0;
                std::function<void(ui::Node &, ui::Node &, std::string)> diffTrees = [&](ui::Node &x, ui::Node &y,
                                                                                         std::string path) {
                    if (shown > 6) {
                        return;
                    }
                    if (!(x.frame() == y.frame())) {
                        std::fprintf(stderr, "%s: %s at %g,%g %gx%g, opened at this size %g,%g %gx%g\n", what.c_str(),
                                     path.c_str(), x.frame().x, x.frame().y, x.frame().width, x.frame().height,
                                     y.frame().x, y.frame().y, y.frame().width, y.frame().height);
                        ++shown;
                    }
                    CHECK(x.frame() == y.frame() && x.children().size() == y.children().size());
                    for (std::size_t i = 0; i < std::min(x.children().size(), y.children().size()); ++i) {
                        diffTrees(*x.children()[i], *y.children()[i],
                                  path + "/" + std::string(x.children()[i]->debugName()) + std::to_string(i));
                    }
                };
                diffTrees(*page, *freshPage, "");
            }
            const auto column = [](ui::Node &screen) -> ui::Node & {
                // screen > page box > scroll > column
                return *screen.children()[0]->children()[0]->children()[0];
            };
            const float bottom = column(*page).style().padding.bottom;
            const float freshBottom = column(*freshPage).style().padding.bottom;
            const gfx::Rect frame = inWindow(*page->children()[0]->children()[0]);
            const gfx::Rect freshFrame = inWindow(*freshPage->children()[0]->children()[0]);
            if (!near(bottom, freshBottom) || !near(frame.width, freshFrame.width) ||
                !near(frame.height, freshFrame.height)) {
                std::fprintf(stderr, "%s: scroll %gx%g, padding %g; opened at this size: %gx%g, padding %g\n",
                             what.c_str(), frame.width, frame.height, bottom, freshFrame.width, freshFrame.height,
                             freshBottom);
            }
            CHECK(near(bottom, freshBottom));
            CHECK(near(frame.width, freshFrame.width) && near(frame.height, freshFrame.height));
            if (changed && expected) {
                const ImageDiff diff = compareImages(*changed, *expected, 3);
                if (diff.pixelsOverTolerance > 0) {
                    std::fprintf(stderr, "%s: %d pixels differ (by up to %d) from the screen opened at this size\n",
                                 what.c_str(), diff.pixelsOverTolerance, diff.maxDifference);
                    writePng(std::string("resized_") + std::to_string(&c - cases) + "_" +
                                     std::to_string(&step - steps) + ".png",
                             *changed);
                    writePng(std::string("resized_") + std::to_string(&c - cases) + "_" +
                                     std::to_string(&step - steps) + ".expected.png",
                             *expected);
                }
                CHECK(diff.pixelsOverTolerance == 0);
            }
        }
    }
}

}  // namespace

int main() {
    Gpu shared;
    if (shared.renderer) {
        gpu = &shared;
    } else {
        std::fprintf(stderr, "No Vulkan device: layout only\n");
    }
    testScrolledList();
    testSheets();
    testViewer();
    testTallMenu();
    testSftpScreensResized();
    if (gpu != nullptr) {
        CHECK(gpu->renders > 0);
    }
    return failures == 0 ? 0 : 1;
}
