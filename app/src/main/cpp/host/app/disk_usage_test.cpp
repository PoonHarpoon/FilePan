// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// The disk usage screen (the adirstat module): scanning, the treemap and list staying in step,
// zooming, the file type view, and deleting a selection across folders.

#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "app_harness.hpp"
#include "filepan/app/adirstat/disk_usage_screen.hpp"

using namespace filepan;
using namespace filepan::test;
using app::DiskUsageScreen;
using filepan::adirstat::kNoNode;
using filepan::adirstat::NodeId;
namespace fs = std::filesystem;

namespace {

struct Files {
    TempDir dir;
    fs::path home;
    fs::path files;

    Files() {
        home = dir.path() / "storage";
        files = dir.path() / "files";
        fs::create_directories(files);
        makeFile(home / "Download" / "Papers" / "a.pdf", 2'400'000, 2026, 10, 12);
        makeFile(home / "Download" / "Papers" / "deep" / "b.txt", 1'000, 2026, 10, 12);
        makeFile(home / "Download" / "apple.md", 18'000, 2026, 10, 8);
        makeFile(home / "Music" / "song.mp3", 900'000, 2026, 8, 1);
        makeFile(home / "Movies" / "clip.mp4", 6'000'000, 2026, 7, 1);
    }
};

ui::Node *findNode(ui::Node &root, const std::function<bool(ui::Node &)> &pred) {
    if (!root.style().display) {
        return nullptr;
    }
    if (pred(root)) {
        return &root;
    }
    for (const auto &child : root.children()) {
        if (ui::Node *found = findNode(*child, pred)) {
            return found;
        }
    }
    return nullptr;
}

ui::Node *byLabel(ui::Node &root, std::string_view label) {
    return findNode(root, [&](ui::Node &n) { return n.semantics().label == label; });
}

NodeId find(const adirstat::Tree &tree, NodeId parent, std::string_view name) {
    for (NodeId id = tree[parent].firstChild; id != kNoNode; id = tree[id].nextSibling) {
        if (tree.name(id) == name) {
            return id;
        }
    }
    return kNoNode;
}

DiskUsageScreen *open(AppHarness &h, const fs::path &folder) {
    h.state().navigate(vfs::Location::local(folder.string()));
    h.settle(100);
    h.state().openScreen(app::kDiskUsageScreen);
    h.pump(50);
    ui::Node *node = h.app->screenNode();
    EXPECT(node != nullptr && node->debugName() == "DiskUsageScreen");
    auto *screen = static_cast<DiskUsageScreen *>(node);
    EXPECT(h.waitUntil([&] { return screen->phase() == DiskUsageScreen::Phase::done && !screen->tiles().empty(); }));
    h.pump(50);
    return screen;
}

// Window dp of a tile's center.
gfx::Vec2 tileCenter(DiskUsageScreen &screen, NodeId id) {
    for (const adirstat::Tile &tile : screen.tiles()) {
        if (tile.node == id) {
            const gfx::Transform t = ui::components::nodeToWindow(screen.treemap(), AppHarness::kDensity);
            return {(t.tx + tile.rect.x + tile.rect.width / 2) / AppHarness::kDensity,
                    (t.ty + tile.rect.y + tile.rect.height / 2) / AppHarness::kDensity};
        }
    }
    EXPECT(false);
    return {};
}

std::optional<std::size_t> itemOf(DiskUsageScreen &screen, NodeId id) {
    for (std::size_t i = 0; i < screen.items().size(); ++i) {
        if (screen.items()[i].node == id) {
            return i;
        }
    }
    return std::nullopt;
}

void testScanAndTreemap() {
    Files f;
    app::AppOptions options;
    options.screens = app::diskUsageScreenSpecs();
    AppHarness h(f.home.string(), f.files.string(), std::move(options));
    DiskUsageScreen *screen = open(h, f.home);
    EXPECT(screen->rootPath() == f.home.string());
    const adirstat::Tree &tree = *screen->tree();
    EXPECT(tree[tree.root()].files == 5);
    EXPECT(tree[tree.root()].size >= 9'319'000);
    // Largest first: Movies, Download, Music.
    EXPECT(screen->items().size() == 3);
    const NodeId movies = find(tree, tree.root(), "Movies");
    EXPECT(screen->items()[0].node == movies);
    const NodeId clip = find(tree, movies, "clip.mp4");
    EXPECT(clip != kNoNode);

    // A tap on the clip's tile focuses it and opens Movies in the list.
    h.tap(tileCenter(*screen, clip));
    h.pump(350);
    EXPECT(screen->focus() == clip);
    EXPECT(itemOf(*screen, clip).has_value());
    // A second tap moves up to its folder.
    h.tap(tileCenter(*screen, clip));
    h.pump(350);
    EXPECT(screen->focus() == movies);

    // Zooming in lays out just that folder.
    const NodeId download = find(tree, tree.root(), "Download");
    screen->zoomTo(download);
    EXPECT(screen->zoomRoot() == download);
    h.pump(10);
    EXPECT(screen->tiles().front().node == download);
    EXPECT(screen->items()[0].node == find(tree, download, "Papers"));
    screen->zoomOut();
    EXPECT(screen->zoomRoot() == tree.root());

    // File types: videos lead.
    screen->setView(DiskUsageScreen::View::types);
    h.pump(10);
    // The switch moves with it, not just the list.
    ui::Node *typesSegment = byLabel(*screen, "File types");
    EXPECT(typesSegment != nullptr && (typesSegment->semantics().flags & ui::semantics_flags::kChecked));
    EXPECT(!(byLabel(*screen, "Folders")->semantics().flags & ui::semantics_flags::kChecked));
    EXPECT(screen->items().front().kind == DiskUsageScreen::Item::Kind::category);
    EXPECT(screen->items().front().category == adirstat::Category::kVideo);
    EXPECT(screen->stats().all.files == 5);
    screen->tapItem(0);
    // Opened: its extensions follow, and the treemap dims the rest.
    EXPECT(screen->items().size() >= 2 && screen->items()[1].kind == DiskUsageScreen::Item::Kind::extension);
    h.pump(10);
}

// Back closes the screen's own state one step at a time before leaving it, and a button brings the
// treemap back once it's scrolled away.
void testBackAndBackToTop() {
    Files f;
    for (int i = 0; i < 60; ++i) {
        makeFile(f.home / "Many" / ("file-" + std::to_string(i) + ".bin"),
                 100'000 + static_cast<std::size_t>(i) * 1'000, 2026, 10, 1);
    }
    app::AppOptions options;
    options.screens = app::diskUsageScreenSpecs();
    AppHarness h(f.home.string(), f.files.string(), std::move(options));
    DiskUsageScreen *screen = open(h, f.home);
    const adirstat::Tree &tree = *screen->tree();
    const NodeId many = find(tree, tree.root(), "Many");
    ui::Node *top = findNode(*screen, [](ui::Node &n) { return n.debugName() == "DiskUsageTop"; });
    EXPECT(top == nullptr);

    // Zoomed into Many, with a long list scrolled well past the treemap.
    screen->zoomTo(many);
    h.pump(50);
    screen->list().scrollTo(4000);
    h.pump(50);
    EXPECT(screen->scrolledDown());
    top = findNode(*screen, [](ui::Node &n) { return n.debugName() == "DiskUsageTop"; });
    EXPECT(top != nullptr);
    h.tap(*byLabel(*top, "Back to top"));
    h.pump(800);
    EXPECT(!screen->scrolledDown());
    EXPECT(findNode(*screen, [](ui::Node &n) { return n.debugName() == "DiskUsageTop"; }) == nullptr);

    // Back: the selection, then the scroll, then the zoom, then the screen.
    screen->list().scrollTo(4000);
    h.pump(50);
    screen->toggleSelected(find(tree, many, "file-59.bin"));
    EXPECT(h.root->dispatchBack());
    h.pump(50);
    EXPECT(screen->selection().empty() && screen->scrolledDown());
    EXPECT(h.root->dispatchBack());
    h.pump(800);
    EXPECT(!screen->scrolledDown() && screen->zoomRoot() == many);
    EXPECT(h.root->dispatchBack());
    h.pump(50);
    EXPECT(screen->zoomRoot() == tree.root());
    EXPECT(h.root->dispatchBack());
    h.pump(50);
    EXPECT(h.app->screenNode() == nullptr);
}

void testDeleteSelectionAcrossFolders() {
    Files f;
    app::AppOptions options;
    options.screens = app::diskUsageScreenSpecs();
    AppHarness h(f.home.string(), f.files.string(), std::move(options));
    DiskUsageScreen *screen = open(h, f.home);
    const adirstat::Tree &tree = *screen->tree();
    const NodeId download = find(tree, tree.root(), "Download");
    const NodeId papers = find(tree, download, "Papers");
    const NodeId song = find(tree, find(tree, tree.root(), "Music"), "song.mp3");
    const std::uint64_t before = tree[tree.root()].size;
    const std::uint64_t gone = tree[papers].size + tree[song].size;

    // Long press starts selecting; then taps add.
    const auto row = itemOf(*screen, download);
    EXPECT(row.has_value());
    screen->tapItem(*row);
    const auto papersRow = itemOf(*screen, papers);
    EXPECT(papersRow.has_value());
    screen->longPressItem(*papersRow);
    // Something inside a selected folder doesn't count twice.
    screen->toggleSelected(find(tree, papers, "a.pdf"));
    screen->toggleSelected(song);
    EXPECT(screen->selection().size() == 3);
    h.pump(50);
    ui::Node *bar = findNode(*screen, [](ui::Node &n) { return n.debugName() == "DiskUsageSelection"; });
    EXPECT(bar != nullptr);
    ui::Node *remove = byLabel(*bar, "Delete");
    EXPECT(remove != nullptr);
    h.tap(*remove);

    ui::components::BottomSheet *sheet = nullptr;
    EXPECT(h.waitUntil([&] { return (sheet = h.app->context().overlays.topSheet()) != nullptr; }));
    h.pump(450);
    ui::Node *confirm = byLabel(sheet->panel(), "Delete 2 items");
    EXPECT(confirm != nullptr);
    h.tap(*confirm);
    EXPECT(h.waitUntil([&] { return !fs::exists(f.home / "Download" / "Papers") && !fs::exists(f.home / "Music" / "song.mp3"); }));
    EXPECT(h.waitUntil([&] { return tree[tree.root()].size == before - gone; }));
    EXPECT(screen->selection().empty());
    EXPECT(!itemOf(*screen, papers).has_value());
    EXPECT(fs::exists(f.home / "Download" / "apple.md"));
    h.pump(10);
    for (const adirstat::Tile &tile : screen->tiles()) {
        EXPECT(tile.node != papers && tile.node != song);
    }
}

void testUnreadableRoot() {
    Files f;
    app::AppOptions options;
    options.screens = app::diskUsageScreenSpecs();
    AppHarness h(f.home.string(), f.files.string(), std::move(options));
    h.state().navigate(vfs::Location::local((f.home / "Music").string()));
    h.settle(100);
    fs::remove_all(f.home / "Music");
    h.state().openScreen(app::kDiskUsageScreen);
    h.pump(50);
    auto *screen = static_cast<DiskUsageScreen *>(h.app->screenNode());
    EXPECT(h.waitUntil([&] { return screen->phase() == DiskUsageScreen::Phase::failed; }));
    EXPECT(screen->tree() == nullptr);
}

// Turned off in the experimental settings, the screen doesn't open.
void testTurnedOff() {
    Files f;
    app::AppOptions options;
    options.screens = app::diskUsageScreenSpecs();
    AppHarness h(f.home.string(), f.files.string(), std::move(options));
    h.state().updateSettings([](app::Settings &s) { s.diskUsage = false; });
    h.state().navigate(vfs::Location::local(f.home.string()));
    h.settle(100);
    h.state().openScreen(app::kDiskUsageScreen);
    h.pump(50);
    EXPECT(h.app->screenNode() == nullptr);
    h.state().updateSettings([](app::Settings &s) { s.diskUsage = true; });
    h.state().back();
    h.pump(10);
    h.state().openScreen(app::kDiskUsageScreen);
    h.pump(50);
    EXPECT(h.app->screenNode() != nullptr && h.app->screenNode()->debugName() == "DiskUsageScreen");
}

void run(const char *name, void (*test)()) {
    if (const char *only = std::getenv("FILEPAN_TEST_ONLY"); only != nullptr && std::string_view(name).find(only) == std::string_view::npos) {
        return;
    }
    std::printf("disk_usage_test: %s\n", name);
    test();
}

}  // namespace

int main() {
    run("scan and treemap", testScanAndTreemap);
    run("back and back to top", testBackAndBackToTop);
    run("delete a selection across folders", testDeleteSelectionAcrossFolders);
    run("unreadable root", testUnreadableRoot);
    run("turned off", testTurnedOff);
    std::puts("disk_usage_test: OK");
    return 0;
}
