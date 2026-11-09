// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// The app's behavior on a temporary directory tree: navigation, the breadcrumb, sorting
// (including ICU's natural numeric order), hidden files, the filter, selection with a range
// drag, the watcher, settings and tabs that survive a restart, the back stack, opening files and
// archives, the storage home, thumbnails and accessibility.

#include <archive.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "app/fake_media_backend.hpp"
#include "app_harness.hpp"
#include "filepan/app/favorites_sheet.hpp"
#include "filepan/app/format.hpp"
#include "filepan/app/text_screen.hpp"
#include "filepan/app/places.hpp"
#include "filepan/app/settings.hpp"
#include "filepan/app/settings_screen.hpp"
#include "filepan/app/video_thumbnails.hpp"
#include "filepan/app/viewer_screen.hpp"
#include "filepan/ui/components/common.hpp"
#include "filepan/create_archive.hpp"

using namespace filepan;
using namespace filepan::test;
using app::Route;
namespace fs = std::filesystem;

namespace {

// A home with Download/ (numbered files, a hidden one, Papers/ and an archive) and Music/.
struct Tree {
    TempDir dir;
    fs::path home;
    fs::path files;

    Tree() {
        home = dir.path() / "storage";
        files = dir.path() / "files";
        fs::create_directories(files);
        makeFile(home / "Download" / "Papers" / "a.pdf", 2'400'000, 2026, 10, 12);
        makeFile(home / "Download" / "file1.txt", 10, 2026, 9, 1);
        makeFile(home / "Download" / "file2.txt", 300, 2026, 9, 3);
        makeFile(home / "Download" / "file10.txt", 20, 2026, 9, 2);
        makeFile(home / "Download" / "File3.txt", 4000, 2026, 9, 5);
        makeFile(home / "Download" / "apple.md", 18'000, 2026, 10, 8);
        makeFile(home / "Download" / "Zebra.jpg", 4'100'000, 2026, 10, 12);
        makeFile(home / "Download" / ".hidden", 1, 2026, 1, 1);
        makeFolder(home / "Download", 2026, 10, 3);
        makeFolder(home / "Music", 2026, 8, 1);
        makeFolder(home, 2026, 1, 1);
        // An archive with one file in a folder.
        makeFile(dir.path() / "src" / "inside" / "readme.txt", 5, 2026, 1, 1);
        struct Quiet final : CreateArchiveCallbacks {
            bool onProgress(std::uint64_t, std::uint64_t, std::string_view) override { return true; }
            bool onSourceError(std::string_view, int) override { return false; }
        } quiet;
        const auto created = createArchive((home / "Download" / "bundle.zip").string(),
                                           {.format = ARCHIVE_FORMAT_ZIP, .filter = ARCHIVE_FILTER_NONE},
                                           {{(dir.path() / "src" / "inside").string(), "inside"}}, quiet);
        EXPECT(created.error == 0);
        setTime(home / "Download" / "bundle.zip", 2026, 9, 20);
        makeFolder(home / "Download" / "Papers", 2026, 10, 2);
        makeFolder(home / "Download", 2026, 10, 3);
        makeFolder(home, 2026, 1, 1);
    }

    [[nodiscard]] std::string path(const std::string &relative = {}) const {
        return relative.empty() ? home.string() : (home / relative).string();
    }
};

vfs::Location local(const std::string &path) { return vfs::Location::local(path); }

bool contains(const std::vector<std::string> &names, std::string_view name) {
    return std::ranges::find(names, name) != names.end();
}

// The first node under root whose semantics label (or text) is label.
ui::Node *findLabel(ui::Node &root, std::string_view label) {
    if (root.style().display) {
        if (root.semantics().label == label || root.semanticsText() == label) {
            return &root;
        }
        for (const auto &child : root.children()) {
            if (ui::Node *found = findLabel(*child, label)) {
                return found;
            }
        }
    }
    return nullptr;
}

const vfs::Location &shown(AppHarness &h) { return *h.state().peekActive()->current().location; }

void open(AppHarness &h, const std::string &name) {
    ui::Node *row = h.rowFor(name);
    EXPECT(row != nullptr);
    h.tap(*row);
    h.settle(100);
}

void testStartsAtHome() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    const Route &route = h.state().peekActive()->current();
    EXPECT(route.kind == Route::Kind::browse && route.location == local(tree.path()));
    EXPECT(h.state().peekActive()->stack.peek().size() == 2);
    // Folders first, natural order.
    EXPECT(h.names() == (std::vector<std::string>{"Download", "Music"}));
    EXPECT(h.browse().items().front().type == app::BrowseScreen::Item::header &&
           h.browse().items().front().label == "Folders");
    // The tab and breadcrumb name the volume.
    EXPECT(h.app->breadcrumb().segmentNodes().size() == 1);
    EXPECT(h.app->breadcrumb().segmentNodes()[0]->semantics().label == "Internal");
}

void testNavigateAndBreadcrumb() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    open(h, "Download");
    EXPECT(shown(h) == local(tree.path("Download")));
    EXPECT(contains(h.names(), "Papers"));
    open(h, "Papers");
    EXPECT(shown(h) == local(tree.path("Download/Papers")));
    EXPECT(h.names() == std::vector<std::string>{"a.pdf"});
    // Internal / Download / Papers: tapping Download goes there.
    const auto &segments = h.app->breadcrumb().segmentNodes();
    EXPECT(segments.size() == 3 && segments[1]->semantics().label == "Download");
    h.tap(*segments[1]);
    h.settle(100);
    EXPECT(shown(h) == local(tree.path("Download")));
    // The root segment goes to the storage home.
    h.tap(*h.app->breadcrumb().segmentNodes()[0]);
    h.pump(100);
    EXPECT(h.state().peekActive()->current().kind == Route::Kind::home);
    EXPECT(h.state().peekActive()->stack.peek().size() == 1);
}

void testHomeButton() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    open(h, "Download");
    // By default it goes to the storage home, leaving nothing to go back to.
    h.tap(h.app->homeButton());
    h.pump(100);
    EXPECT(h.state().peekActive()->current().kind == Route::Kind::home);
    EXPECT(h.state().peekActive()->stack.peek().size() == 1);

    // A favorite chosen in the settings takes its place.
    const vfs::Location papers = local(tree.path("Download/Papers"));
    h.state().updateSettings([&](app::Settings &s) {
        s.favorites = {{"Papers", papers, app::Favorite::Kind::folder}};
        s.homeTarget = papers;
    });
    h.tap(h.app->homeButton());
    h.settle(100);
    EXPECT(shown(h) == papers);

    // Once it's no longer a favorite, the storage home again.
    h.state().removeFavorite(papers);
    h.tap(h.app->homeButton());
    h.pump(100);
    EXPECT(h.state().peekActive()->current().kind == Route::Kind::home);
}

void testSortOrders() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    h.state().navigate(local(tree.path("Download")));
    h.settle(100);
    // ICU collation, numeric: file2 before File3 before file10, case aside.
    EXPECT(h.names() == (std::vector<std::string>{"Papers", "apple.md", "bundle.zip", "file1.txt", "file2.txt",
                                                  "File3.txt", "file10.txt", "Zebra.jpg"}));
    const auto sortBy = [&](vfs::SortKey key, bool descending, bool foldersFirst) {
        h.state().updateSettings([&](app::Settings &s) { s.sort = {key, descending, foldersFirst}; });
        h.pump(30);
    };
    sortBy(vfs::SortKey::kName, true, true);
    EXPECT(h.names() == (std::vector<std::string>{"Papers", "Zebra.jpg", "file10.txt", "File3.txt", "file2.txt",
                                                  "file1.txt", "bundle.zip", "apple.md"}));
    sortBy(vfs::SortKey::kSize, false, true);
    std::vector<std::string> names = h.names();
    EXPECT(names[0] == "Papers" && names[1] == "file1.txt" && names[2] == "file10.txt" && names.back() == "Zebra.jpg");
    sortBy(vfs::SortKey::kModified, true, false);
    names = h.names();
    EXPECT(names[0] == "Zebra.jpg" && names.back() == "file1.txt");
    // Without folders first it is one plain list: no section headers.
    EXPECT(std::ranges::count_if(h.browse().items(), [](const auto &i) {
               return i.type == app::BrowseScreen::Item::header;
           }) == 0);
    sortBy(vfs::SortKey::kType, false, true);
    names = h.names();
    EXPECT(names[0] == "Papers");
    // Images, then archives, then documents and text by kind.
    EXPECT(std::ranges::find(names, "Zebra.jpg") < std::ranges::find(names, "bundle.zip"));
    // The overflow menu leads with the sort options; they write the settings and leave it open.
    h.app->showOverflow();
    h.pump(300);
    EXPECT(h.app->context().overlays.openCount.peek() == 1);
    ui::Node *size = findLabel(*h.app->root(), "Sort by Size");
    EXPECT(size != nullptr);
    h.tap(*size);
    h.pump(30);
    // A new key starts in its natural order: biggest first.
    EXPECT(h.state().settings.peek().sort.key == vfs::SortKey::kSize && h.state().settings.peek().sort.descending);
    EXPECT(h.app->context().overlays.openCount.peek() == 1);
    EXPECT(findLabel(*h.app->root(), "Order: Descending, big first") != nullptr);
    // The same key again flips the order.
    h.tap(*size);
    h.pump(30);
    EXPECT(!h.state().settings.peek().sort.descending);
    EXPECT(h.app->back());
    EXPECT(h.waitUntil([&] { return h.app->context().overlays.openCount.peek() == 0; }));
    EXPECT(h.app->context().overlays.openCount.peek() == 0);
}

void testHiddenAndFilter() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    h.state().navigate(local(tree.path("Download")));
    h.settle(100);
    const auto has = [&](const char *name) { return contains(h.names(), name); };
    EXPECT(!has(".hidden"));
    h.state().updateSettings([](app::Settings &s) { s.showHidden = true; });
    h.pump(30);
    EXPECT(has(".hidden"));
    h.state().updateSettings([](app::Settings &s) { s.showHidden = false; });
    // The filter: case-insensitive substring, from the dock's search button. The field takes the
    // path's place in the top bar.
    ui::Node *search = findLabel(h.browse().dock().idleRow(), "Filter this folder");
    EXPECT(search != nullptr);
    h.tap(*search);
    EXPECT(h.state().peekActive()->searching.peek());
    h.pump(30);
    EXPECT(!h.app->breadcrumb().style().display && h.app->closeSearchButton().style().display);
    h.state().peekActive()->filter.set("FILE");
    h.pump(30);
    EXPECT(h.names() == (std::vector<std::string>{"file1.txt", "file2.txt", "File3.txt", "file10.txt"}));
    h.state().peekActive()->filter.set("nothing-matches");
    h.pump(30);
    EXPECT(h.names().empty() && h.browse().emptyState() != nullptr);
    EXPECT(findLabel(*h.browse().emptyState(), "No match") != nullptr);
    // Back closes the filter.
    EXPECT(h.app->back());
    h.pump(30);
    EXPECT(!h.state().peekActive()->searching.peek() && h.names().size() == 8);
}

void testSelectionAndRangeDrag() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    h.state().navigate(local(tree.path("Download")));
    h.settle(100);
    auto tab = h.state().peekActive();
    // Long press on file1.txt selects it, and dragging to File3.txt selects the range between.
    const gfx::Vec2 from = h.center(*h.rowFor("file1.txt"));
    const gfx::Vec2 to = h.center(*h.rowFor("File3.txt"));
    h.longPress(from, false);
    EXPECT(tab->selection.peek() == std::vector<std::string>{"file1.txt"});
    for (int i = 1; i <= 8; ++i) {
        h.pointer(ui::PointerAction::move, {from.x, from.y + (to.y - from.y) * static_cast<float>(i) / 8});
        h.pump(16);
    }
    h.pointer(ui::PointerAction::up, to);
    h.pump(30);
    EXPECT(tab->selection.peek() == (std::vector<std::string>{"File3.txt", "file1.txt", "file2.txt"}));
    // The dock switches to selection mode.
    h.pump(400);
    EXPECT(h.browse().dock().modeProgress() > 0.99f);
    // In selection mode a tap toggles.
    h.tap(*h.rowFor("apple.md"));
    EXPECT(tab->selection.peek().size() == 4);
    h.tap(*h.rowFor("file2.txt"));
    EXPECT(tab->selection.peek() == (std::vector<std::string>{"File3.txt", "apple.md", "file1.txt"}));
    // Rows carry the selected state.
    auto &row = static_cast<ui::components::FileRow &>(*h.rowFor("apple.md"));
    EXPECT(row.data().selected && row.data().selecting);
    EXPECT(row.semantics().flags & ui::semantics_flags::kSelected);
    // Back clears the selection, not the folder.
    EXPECT(h.app->back());
    h.pump(30);
    EXPECT(tab->selection.peek().empty() && shown(h) == local(tree.path("Download")));
    // Copy puts the selection on the clipboard (file_operations_test has the rest).
    h.browse().selectAll();
    h.pump(30);
    EXPECT(tab->selection.peek().size() == 8);
    h.app->context().actions.copy(h.browse().selectionTargets());
    h.pump(30);
    EXPECT(h.app->context().actions.canPaste() && tab->selection.peek().empty());
    const auto &message = h.app->context().overlays.snackbars().current();
    // Just a note: paste goes through the dock or the New menu in the folder it should land in.
    EXPECT(message && message->text == "8 items to copy · open a folder and paste" && message->action.empty());
}

void testWatcher() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    h.state().navigate(local(tree.path("Download")));
    h.settle(100);
    EXPECT(h.waitUntil([&] { return h.browse().model().watching(); }));
    makeFile(tree.home / "Download" / "file4.txt", 5, 2026, 9, 9);
    EXPECT(h.waitUntil([&] { return contains(h.names(), "file4.txt"); }));
    // In natural order, between File3 and file10.
    const auto names = h.names();
    EXPECT(std::ranges::find(names, "file4.txt") - std::ranges::find(names, "File3.txt") == 1);
    fs::remove(tree.home / "Download" / "file4.txt");
    EXPECT(h.waitUntil([&] { return !contains(h.names(), "file4.txt"); }));
    // Deleting the folder shows the error state with a retry.
    fs::remove_all(tree.home / "Download");
    EXPECT(h.waitUntil([&] { return h.browse().model().status.peek() == app::DirectoryModel::Status::error; }));
    h.pump(30);
    EXPECT(h.browse().emptyState() != nullptr && findLabel(*h.browse().emptyState(), "Retry") != nullptr);
}

void testSettingsRoundTrip() {
    app::Settings s;
    s.preset = ui::ThemePreset::ultraviolet;
    s.accent = gfx::Color::fromRgb(0x2DD4BF);
    s.sort = {vfs::SortKey::kModified, true, false};
    s.view = app::ViewMode::grid;
    s.showHidden = true;
    s.thumbnails = false;
    s.thumbnailCacheSize = 256;
    s.thumbnailDecayDays = 0;
    s.remoteThumbnails = false;
    s.remoteThumbnailMaxSize = 25;
    s.viewerPrefetch = 5;
    s.viewerPrefetchVideoMb = 0;
    s.remoteConnections = 6;
    s.dataSaver = true;
    s.cardPinLetters = true;
    s.documentTrees = true;
    s.diskUsage = false;
    s.trees = {{"Shared 2", "content://x/tree/a%20b"}, {"Music", "content://y/tree/primary%3AMusic"}};
    s.tabs ={{}, {{local("/storage/emulated/0"), local("/storage/emulated/0/a b/ü%")}},
              {{vfs::Location::archive(local("/x/y z.zip"), "/in side"), vfs::Location::sftp("nas:1", "/home/me")}}};
    s.activeTab = 2;
    using Kind = app::Favorite::Kind;
    s.favorites = {{"Papers", local("/storage/emulated/0/Download/Papers"), Kind::folder},
                   {"-", local("/"), Kind::folder},
                   {"", local("/tmp/a b.txt"), Kind::file},
                   {"Media", vfs::Location::sftp("nas", "/srv/media"), Kind::folder}};
    s.hiddenItems = {local("/storage/emulated/0/.thumbs x"), vfs::Location::sftp("nas", "/srv/old")};
    s.homeTarget = vfs::Location::sftp("nas", "/srv/media");
    vfs::SftpServerConfig server;
    server.id = "nas";
    server.name = "Home NAS";
    server.server = {.host = "nas.local", .port = 2222, .username = "me",
                     .authentication = {.type = vfs::SftpAuthentication::Type::kPrivateKey, .password = "",
                                        .privateKey = "-----BEGIN OPENSSH PRIVATE KEY-----\nb3Blbn+/=\n"
                                                      "-----END OPENSSH PRIVATE KEY-----\n",
                                        .passphrase = "p w\nx"}};
    server.jumpHosts = {{.host = "jump", .port = 22, .username = "j", .authentication = {.password = "secret"}}};
    server.knownHostsPath = "/files/known_hosts";
    server.timeoutSeconds = 30;
    s.servers = {server};
    // Secrets go through the platform's sealing (the harness's test transform here).
    FakePlatform platform;
    app::PlatformSecretCodec codec(platform);
    const std::string text = app::serializeSettings(s, &codec);
    auto parsed = app::parseSettings(text, &codec);
    EXPECT(parsed.has_value() && *parsed == s);
    // The private key is sealed like the other secrets, never written as given.
    EXPECT(text.find("secret nas 0 key ") != std::string::npos && text.find("b3Blbn") == std::string::npos);
    EXPECT(text.find("OPENSSH") == std::string::npos);
    // Without a codec it isn't kept at all.
    auto unsealed = app::parseSettings(app::serializeSettings(s), &codec);
    EXPECT(unsealed && unsealed->servers[0].server.authentication.privateKey.empty());
    // An older file's key path is read as no key.
    auto legacy = app::parseSettings("filepan-settings 1\nserver old h 22 u key - /sdcard/id_ed25519 - - 15\n");
    EXPECT(legacy && legacy->servers.size() == 1);
    EXPECT(legacy->servers[0].server.authentication.type == vfs::SftpAuthentication::Type::kPrivateKey);
    EXPECT(legacy->servers[0].server.authentication.privateKey.empty());
    // Unknown lines are skipped; a wrong header fails.
    auto tolerant = app::parseSettings("filepan-settings 7\nfuture-key 1 2 3\nhidden 1\n");
    EXPECT(tolerant && tolerant->showHidden && tolerant->preset == ui::ThemePreset::acid);  // Acid by default.
    // So are the dropped glass settings: the surface choice and reduce glass.
    auto surface = app::parseSettings("filepan-settings 7\ntheme ultraviolet\nsurface glass\nreduce-glass 1\nhidden 1\n");
    EXPECT(surface && surface->preset == ui::ThemePreset::ultraviolet && surface->showHidden);
    // Clean, written and read back.
    app::Settings plain;
    plain.preset = ui::ThemePreset::clean;
    EXPECT(app::serializeSettings(plain).find("theme clean\n") != std::string::npos);
    auto clean = app::parseSettings(app::serializeSettings(plain));
    EXPECT(clean && clean->preset == ui::ThemePreset::clean);
    // Its first name still reads.
    auto renamed = app::parseSettings("filepan-settings 7\ntheme basic\n");
    EXPECT(renamed && renamed->preset == ui::ThemePreset::clean);
    // The thumbnail cache: defaults of 1024 MB and 3 days, and out-of-range values ignored.
    EXPECT(app::thumbnailCacheBytes({}) == 1024ull << 20 && app::thumbnailDecaySeconds({}) == 3 * 86400);
    auto cache = app::parseSettings("filepan-settings 1\nthumbnail-cache 0\nthumbnail-decay -1\n");
    EXPECT(cache && cache->thumbnailCacheSize == 1024 && cache->thumbnailDecayDays == 3);
    cache = app::parseSettings("filepan-settings 1\nthumbnail-cache 2048\nthumbnail-decay 30\n");
    EXPECT(cache && app::thumbnailCacheBytes(*cache) == 2048ull << 20 && cache->thumbnailDecayDays == 30);
    // Remote thumbnails: on, up to 10 MB, by default.
    EXPECT(app::Settings{}.remoteThumbnails && app::remoteThumbnailMaxBytes({}) == 10ull << 20);
    auto remote = app::parseSettings("filepan-settings 1\nremote-thumbnails 0\nremote-thumbnail-max 0\n");
    EXPECT(remote && !remote->remoteThumbnails && remote->remoteThumbnailMaxSize == 10);
    // Mobile data mode: off by default.
    EXPECT(!app::Settings{}.dataSaver);
    // The intro: unseen on a fresh install, seen in a file from before it, and kept.
    EXPECT(app::Settings{}.onboarded == 0);
    auto before = app::parseSettings("filepan-settings 1\ntheme acid\n");
    EXPECT(before && before->onboarded == 1);
    auto unseen = app::parseSettings("filepan-settings 1\nonboarded 0\n");
    EXPECT(unseen && unseen->onboarded == 0);
    // The viewer's prefetching: 2 files each side, 8 MB of video, 3 connections by default, and
    // out-of-range values ignored.
    EXPECT(app::Settings{}.viewerPrefetch == 2 && app::Settings{}.viewerPrefetchVideoMb == 8 &&
           app::Settings{}.remoteConnections == 3);
    auto prefetch = app::parseSettings(
            "filepan-settings 1\nviewer-prefetch 0\nviewer-prefetch-video 16\nremote-connections 1\n");
    EXPECT(prefetch && prefetch->viewerPrefetch == 0 && prefetch->viewerPrefetchVideoMb == 16 &&
           prefetch->remoteConnections == 1);
    prefetch = app::parseSettings(
            "filepan-settings 1\nviewer-prefetch -1\nviewer-prefetch-video x\nremote-connections 7\n");
    EXPECT(prefetch && prefetch->viewerPrefetch == 2 && prefetch->viewerPrefetchVideoMb == 8 &&
           prefetch->remoteConnections == 3);
    EXPECT(!app::parseSettings("not settings\n"));
    // The dropped Crossfader preset reads as Ultraviolet, its nearest.
    auto dropped = app::parseSettings("filepan-settings 7\ntheme crossfader\n");
    EXPECT(dropped && dropped->preset == ui::ThemePreset::ultraviolet);

    // The store writes atomically and never lets an older save win.
    TempDir dir;
    app::SettingsStore store((dir.path() / "settings.txt").string());
    EXPECT(store.load() == app::Settings{});
    const std::uint64_t older = store.nextGeneration();
    const std::uint64_t newer = store.nextGeneration();
    EXPECT(store.save(text, newer));
    EXPECT(store.save(app::serializeSettings({}), older));
    EXPECT(store.load(&codec) == s);
    EXPECT(!fs::exists(dir.path() / "settings.txt.tmp"));

    // Locations survive their string form.
    for (const auto &tab : s.tabs) {
        for (const auto &location : tab.history) {
            EXPECT(app::decodeLocation(app::encodeLocation(location)) == location);
        }
    }
    EXPECT(!app::decodeLocation("L|relative") && !app::decodeLocation("Q|/x") && !app::decodeLocation("L|%G0"));
    const vfs::Location tree = vfs::Location::document("Music | 2", "/a b/ü");
    EXPECT(app::encodeLocation(tree).starts_with("D|") && app::decodeLocation(app::encodeLocation(tree)) == tree);
}

void testTabsPersist() {
    Tree tree;
    std::uint64_t secondId = 0;
    {
        AppHarness h(tree.path(), tree.files.string());
        h.state().navigate(local(tree.path("Download")));
        h.settle(50);
        secondId = h.state().addTab(local(tree.path("Music")));
        h.settle(50);
        h.state().navigate(local(tree.path("Download/Papers")));
        h.state().addFavorite({"Download", local(tree.path("Download"))});
        h.state().updateSettings([](app::Settings &s) { s.preset = ui::ThemePreset::ultraviolet; });
        h.settle(50);
        // The theme follows the settings at once.
        EXPECT(h.root->env().theme.peek().preset == ui::ThemePreset::ultraviolet);
        // Closing the app saves.
    }
    EXPECT(secondId != 0);
    EXPECT(fs::exists(tree.files / "settings.txt"));
    AppHarness h(tree.path(), tree.files.string());
    const auto &tabs = h.state().tabs.peek();
    EXPECT(tabs.size() == 2);
    const auto &first = tabs[0]->stack.peek();
    EXPECT(first.size() == 3 && first[2].location == local(tree.path("Download")));
    const auto &second = tabs[1]->stack.peek();
    EXPECT(second.size() == 3 && second.back().location == local(tree.path("Download/Papers")));
    EXPECT(h.state().peekActive() == tabs[1]);
    EXPECT(shown(h) == local(tree.path("Download/Papers")) && h.names() == std::vector<std::string>{"a.pdf"});
    EXPECT(h.state().settings.peek().favorites.size() == 1);
    EXPECT(h.root->env().theme.peek().preset == ui::ThemePreset::ultraviolet);
    // Switching tabs shows the other tab's folder; closing the active one moves to a neighbour.
    h.state().selectTab(tabs[0]->id);
    h.settle(50);
    EXPECT(shown(h) == local(tree.path("Download")));
    h.state().closeTab(tabs[0]->id);
    h.settle(50);
    EXPECT(h.state().tabs.peek().size() == 1 && shown(h) == local(tree.path("Download/Papers")));
    // The overflow menu counts the tabs and opens the tabs sheet, each tab labelled by its folder.
    h.app->showOverflow();
    h.pump(30);
    ui::Node *tabsItem = findLabel(*h.app->root(), "Tabs, 1");
    EXPECT(tabsItem != nullptr);
    h.tap(*tabsItem);
    h.pump(300);
    ui::components::BottomSheet *sheet = h.app->context().overlays.topSheet();
    EXPECT(sheet != nullptr);
    ui::Node *row = findLabel(sheet->panel(), "Papers, Internal / Download / Papers");
    EXPECT(row != nullptr && (row->semantics().flags & ui::semantics_flags::kSelected));
    // A new tab from the sheet opens on the same folder.
    ui::Node *add = findLabel(sheet->panel(), "New tab");
    EXPECT(add != nullptr);
    h.tap(*add);
    h.pump(300);
    EXPECT(h.state().tabs.peek().size() == 2 && h.state().peekActive()->current().location ==
                                                         local(tree.path("Download/Papers")));
}

void testBackStack() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    open(h, "Download");
    // Scroll positions are per folder: deep in Download, then into Papers and back.
    open(h, "Papers");
    EXPECT(h.app->back());
    h.settle(100);
    EXPECT(shown(h) == local(tree.path("Download")));
    EXPECT(h.app->back());
    h.settle(100);
    EXPECT(shown(h) == local(tree.path()));
    EXPECT(h.app->back());
    h.pump(50);
    EXPECT(h.state().peekActive()->current().kind == Route::Kind::home);
    // At the home there's nothing left: the system takes back.
    EXPECT(!h.app->back());
    EXPECT(!h.root->dispatchBack());
    // A sheet takes back first.
    h.state().navigate(local(tree.path("Music")));
    h.settle(50);
    h.app->showTabs();
    h.pump(300);
    EXPECT(h.root->dispatchBack());
    EXPECT(h.waitUntil([&] { return h.app->context().overlays.openCount.peek() == 0; }));
    EXPECT(shown(h) == local(tree.path("Music")) && h.app->context().overlays.openCount.peek() == 0);
    // A registered screen is a route too.
    h.state().openScreen(app::kSettingsScreen);
    h.pump(50);
    EXPECT(h.app->screenNode() != nullptr && h.app->breadcrumb().segmentNodes().size() == 2);
    EXPECT(h.root->dispatchBack());
    h.pump(50);
    EXPECT(h.app->screenNode() == nullptr && shown(h) == local(tree.path("Music")));
}

// A registered screen can use back for its own state before it's left.
void testScreenBack() {
    Tree tree;
    int uses = 2;
    app::AppOptions options;
    options.screens.push_back({.id = "counter",
                               .title = "Counter",
                               .build = [](app::AppContext &) -> ui::NodePtr { return ui::Box::create({}); },
                               .back = [&uses](ui::Node &) { return uses-- > 0; },
                               .inMenu = false});
    AppHarness h(tree.path(), tree.files.string(), std::move(options));
    h.state().navigate(local(tree.path("Music")));
    h.settle(50);
    h.state().openScreen("counter");
    h.pump(50);
    EXPECT(h.root->dispatchBack() && h.app->screenNode() != nullptr);
    EXPECT(h.root->dispatchBack() && h.app->screenNode() != nullptr);
    EXPECT(h.root->dispatchBack());
    h.pump(50);
    EXPECT(h.app->screenNode() == nullptr && shown(h) == local(tree.path("Music")));
}

void testScrollRestore() {
    TempDir dir;
    const fs::path home = dir.path() / "home";
    for (int i = 0; i < 200; ++i) {
        makeFile(home / "many" / ("f" + std::to_string(i) + ".txt"), 1, 2026, 1, 1);
    }
    makeFolder(home / "many" / "sub", 2026, 1, 1);
    AppHarness h(home.string(), (dir.path()).string());
    h.state().navigate(local((home / "many").string()));
    h.settle(100);
    h.browse().list().scrollToItem(120);
    h.pump(50);
    const float offset = h.browse().list().offset();
    EXPECT(offset > 1000);
    h.state().navigate(local((home / "many" / "sub").string()));
    h.settle(50);
    EXPECT(h.browse().list().offset() == 0);
    h.app->back();
    h.settle(100);
    EXPECT(std::abs(h.browse().list().offset() - offset) < 80);
}

void testDataSaving() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    const auto saving = [&] {
        h.pump(20);
        return h.app->context().dataSaving.peek();
    };
    // Mobile data mode saves only on a metered network, and follows it both ways.
    EXPECT(!saving());
    h.state().updateSettings([](app::Settings &s) { s.dataSaver = true; });
    EXPECT(!saving());
    h.services->metered.set(true);
    EXPECT(saving());
    h.services->metered.set(false);
    EXPECT(!saving());
    h.services->metered.set(true);
    h.state().updateSettings([](app::Settings &s) { s.dataSaver = false; });
    EXPECT(!saving());
}

// Mobile data mode on a metered network: a server's folder lists, but its subfolders aren't
// listed for their counts and its images aren't fetched for thumbnails, until the network is
// unmetered again.
void testDataSaverOnServer() {
    Tree tree;
    auto server = std::make_shared<FakeMediaBackend>();
    server->add("/Pics/Album/a.jpg", 3000, 1);
    server->add("/Pics/Trip/b.jpg", 3000, 2);
    server->add("/Pics/c.jpg", 3000, 3);
    const auto listed = [&](const std::string &path) {
        return server->with([&](FakeMediaBackend::Shared &s) { return s.lists[path]; });
    };
    AppHarness h(tree.path(), tree.files.string(), {}, false);
    h.backends->set(vfs::BackendKind::kSftp, server);
    h.platform.metered = true;
    h.start();
    h.state().updateSettings([](app::Settings &s) { s.dataSaver = true; });
    h.pump(20);
    EXPECT(!h.app->context().thumbnails.remoteEnabled());
    h.state().navigate(vfs::Location::sftp("nas", "/Pics"));
    h.settle(100);
    EXPECT(h.waitUntil([&] { return h.rowFor("Album") != nullptr && h.rowFor("c.jpg") != nullptr; }));
    h.pump(100);
    EXPECT(listed("/Pics") == 1 && listed("/Pics/Album") == 0 && listed("/Pics/Trip") == 0);
    EXPECT(h.browse().model().pendingCounts() == 0);
    // On Wi-Fi the rows on screen ask again, without listing the folder again.
    h.services->metered.set(false);
    EXPECT(h.waitUntil([&] { return listed("/Pics/Album") == 1 && listed("/Pics/Trip") == 1; }));
    EXPECT(h.app->context().thumbnails.remoteEnabled());
    EXPECT(listed("/Pics") == 1);
}

// A restored tab on a server doesn't connect until it's shown.
void testRestoredServerTabIsLazy() {
    Tree tree;
    auto server = std::make_shared<FakeMediaBackend>();
    server->add("/Pics/c.jpg", 3000, 3);
    app::Settings saved;
    saved.tabs = {{{local(tree.path("Download"))}}, {{vfs::Location::sftp("nas", "/Pics")}}};
    saved.activeTab = 0;
    {
        std::ofstream out(tree.files / "settings.txt");
        out << app::serializeSettings(saved);
    }
    AppHarness h(tree.path(), tree.files.string(), {}, false);
    h.backends->set(vfs::BackendKind::kSftp, server);
    h.start();
    h.settle(100);
    EXPECT(h.state().tabs.peek().size() == 2);
    EXPECT(server->with([](FakeMediaBackend::Shared &s) { return s.lists["/Pics"]; }) == 0);
}

void testHaptics() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    // The app forwards the kit's haptics to the platform; plain taps stay silent.
    ui::components::haptic(ui::components::Haptic::tap);
    EXPECT(h.platform.haptics.empty());
    ui::components::haptic(ui::components::Haptic::toggle);
    ui::components::haptic(ui::components::Haptic::longPress);
    EXPECT((h.platform.haptics == std::vector{runtime::HapticKind::tick, runtime::HapticKind::longPress}));
}

std::size_t ticks(const AppHarness &h) {
    return static_cast<std::size_t>(std::ranges::count(h.platform.haptics, runtime::HapticKind::tick));
}

std::optional<std::string> toast(AppHarness &h) {
    const auto &message = h.app->context().overlays.snackbars().current();
    return message ? std::optional(message->text) : std::nullopt;
}

void testPullToRefresh() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    h.state().navigate(local(tree.path("Download")));
    h.settle(100);
    const std::uint64_t version = h.browse().model().version.peek();
    const gfx::Vec2 start = h.center(*h.rowFor("file1.txt"));
    const auto moveTo = [&](float dy) {
        h.pointer(ui::PointerAction::move, {start.x, start.y + dy});
        h.pump(10);
    };
    h.pointer(ui::PointerAction::down, start);
    for (int i = 1; i <= 20; ++i) {
        moveTo(12.0f * static_cast<float>(i));
    }
    EXPECT(h.browse().pull() >= app::BrowseScreen::kPullThreshold);
    // Crossing the threshold arms the indicator, with one tick.
    EXPECT(h.browse().armed() && ticks(h) == 1);
    // Wavering around the threshold doesn't tick again.
    moveTo(220);
    moveTo(240);
    EXPECT(h.browse().armed() && ticks(h) == 1);
    // Pulling stretches nothing: the list stays at the top.
    EXPECT(h.browse().list().offset() == 0);
    h.pointer(ui::PointerAction::up, {start.x, start.y + 240});
    // The local listing is instant, but the indicator stays up and spins for a moment.
    EXPECT(h.browse().refreshing() && h.browse().spinning());
    h.pump(150);
    EXPECT(h.browse().refreshing() && h.browse().pull() > 1);
    // Then the folder is listed again, the indicator goes away and a toast says so.
    EXPECT(h.waitUntil([&] {
        return h.browse().model().version.peek() > version && h.browse().pull() < 0.5f &&
               h.browse().model().status.peek() == app::DirectoryModel::Status::loaded;
    }));
    EXPECT(!h.browse().refreshing() && !h.browse().spinning() && !h.browse().armed());
    EXPECT(h.names().size() == 8);
    EXPECT(toast(h) == "Refreshed · 8 items");
}

void testPullDisarms() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    h.state().navigate(local(tree.path("Download")));
    h.settle(100);
    const gfx::Vec2 start = h.center(*h.rowFor("file1.txt"));
    h.pointer(ui::PointerAction::down, start);
    for (int i = 1; i <= 20; ++i) {
        h.pointer(ui::PointerAction::move, {start.x, start.y + 12.0f * static_cast<float>(i)});
        h.pump(10);
    }
    EXPECT(h.browse().armed());
    // Back under the disarm line, releasing doesn't refresh.
    for (int i = 19; i >= 1; --i) {
        h.pointer(ui::PointerAction::move, {start.x, start.y + 12.0f * static_cast<float>(i)});
        h.pump(10);
    }
    EXPECT(!h.browse().armed());
    h.pointer(ui::PointerAction::up, {start.x, start.y + 12});
    h.pump(50);
    EXPECT(!h.browse().refreshing() && !toast(h));
    EXPECT(h.waitUntil([&] { return h.browse().pull() < 0.5f; }));
}

void testMenuRefresh() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    h.state().navigate(local(tree.path("Download")));
    h.settle(100);
    // The overflow menu's Refresh drops the indicator too.
    h.browse().refresh(app::RefreshFeedback::shown);
    h.pump(150);
    EXPECT(h.browse().refreshing() && h.browse().pull() > 10 && h.browse().armed());
    EXPECT(h.waitUntil([&] { return !h.browse().refreshing() && h.browse().pull() < 0.5f; }));
    EXPECT(toast(h) == "Refreshed · 8 items");
    EXPECT(ticks(h) == 0);
}

void testSilentRefresh() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    h.state().navigate(local(tree.path("Download")));
    h.settle(100);
    const std::uint64_t version = h.browse().model().version.peek();
    // A refresh nobody asked for (a setting changed what rows show) shows nothing.
    h.browse().refresh();
    EXPECT(!h.browse().spinning());
    EXPECT(h.waitUntil([&] { return h.browse().model().version.peek() > version && !h.browse().refreshing(); }));
    h.pump(50);
    EXPECT(h.browse().pull() == 0 && !toast(h));
}

void testRefreshReducedMotion() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    h.root->env().reducedMotion.set(true);
    h.state().navigate(local(tree.path("Download")));
    h.settle(100);
    // No spin under reduced motion; the indicator still shows, tinted, and the toast comes.
    h.browse().refresh(app::RefreshFeedback::shown);
    EXPECT(h.browse().refreshing() && !h.browse().spinning() && h.browse().armed());
    EXPECT(h.waitUntil([&] { return !h.browse().refreshing(); }));
    EXPECT(toast(h) == "Refreshed · 8 items");
}

void testOpenFilesAndArchives() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    h.state().navigate(local(tree.path("Download")));
    h.settle(100);
    open(h, "apple.md");
    // Text opens in the editor; anything else the app can't show goes to the system.
    EXPECT(h.app->textScreen() != nullptr && h.platform.opened.empty());
    EXPECT(h.app->back());
    h.pump(30);
    open(h, "Papers");
    open(h, "a.pdf");
    EXPECT(h.platform.opened.size() == 1 && h.platform.opened[0].first == tree.path("Download/Papers/a.pdf") &&
           h.platform.opened[0].second == "application/pdf");
    h.state().navigate(local(tree.path("Download")));
    h.settle(100);
    // An archive opens as a folder.
    open(h, "bundle.zip");
    EXPECT(shown(h).kind() == vfs::BackendKind::kArchive);
    EXPECT(h.waitUntil([&] { return h.names() == std::vector<std::string>{"inside"}; }));
    open(h, "inside");
    EXPECT(h.waitUntil([&] { return h.names() == std::vector<std::string>{"readme.txt"}; }));
    // Its breadcrumb runs through the archive's own name.
    const auto crumbs = app::crumbsFor(shown(h), h.platform.volumes);
    EXPECT(crumbs.size() == 4 && crumbs[2].label == "bundle.zip" && crumbs[3].label == "inside");
}

void testHome() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string(), {}, false);
    h.platform.access = false;
    h.start();
    h.state().navigateHome();
    h.pump(50);
    auto &home = h.app->home();
    EXPECT(home.permissionCard() != nullptr);
    ui::Node *grant = findLabel(*home.permissionCard(), "Grant all files access");
    EXPECT(grant != nullptr);
    h.tap(*grant);
    EXPECT(h.platform.accessRequests == 1);
    // Granted while away: resuming refreshes the signal and the card goes.
    h.platform.access = true;
    h.services->allFilesAccess.set(true);
    h.pump(50);
    EXPECT(home.permissionCard() == nullptr);
    EXPECT(home.volumeCards().size() == 1);
    EXPECT(home.volumeCards()[0]->semantics().label.starts_with("Internal, 69% used, 74 GiB free of 238 GiB"));
    // A favorite from a folder's overflow menu.
    h.tap(*home.volumeCards()[0]);
    h.settle(50);
    EXPECT(shown(h) == local(tree.path()));
    open(h, "Music");
    h.app->showOverflow();
    h.pump(300);
    // The same menu leads with the sort options.
    EXPECT(findLabel(*h.app->root(), "Sort by Date") != nullptr);
    ui::Node *add = findLabel(*h.app->root(), "Add to favorites");
    EXPECT(add != nullptr);
    h.tap(*add);
    h.pump(300);
    EXPECT(h.state().settings.peek().favorites.size() == 1 && h.state().settings.peek().favorites[0].name == "Music");
    h.state().navigateHome();
    h.pump(50);
    EXPECT(home.favoriteRows().size() == 1);
    // Long press removes it.
    h.longPress(h.center(*home.favoriteRows()[0]));
    h.pump(50);
    EXPECT(h.state().settings.peek().favorites.empty() && home.favoriteRows().empty());
    // The SFTP entry isn't there yet.
    EXPECT(home.serversRow() != nullptr);
}

void testThumbnails() {
    Tree tree;
    // Videos get one too; other files don't.
    makeFile(tree.home / "Download" / "clip.mp4", 9'000'000, 2026, 10, 11);
    std::vector<std::string> decoded;
    app::AppOptions options;
    options.decoder = [&decoded](const std::string &path, int size) -> std::optional<app::DecodedImage> {
        decoded.push_back(fs::path(path).filename().string());
        EXPECT(size > 100);
        return app::DecodedImage{4, 4, std::vector<std::uint8_t>(64, 200)};
    };
    AppHarness h(tree.path(), tree.files.string(), options);
    h.state().navigate(local(tree.path("Download")));
    h.settle(100);
    EXPECT(h.waitUntil([&] {
        auto *row = static_cast<ui::components::FileRow *>(h.rowFor("Zebra.jpg"));
        auto *video = static_cast<ui::components::FileRow *>(h.rowFor("clip.mp4"));
        return row != nullptr && row->data().thumbnail != 0 && video != nullptr && video->data().thumbnail != 0;
    }));
    std::ranges::sort(decoded);
    EXPECT((decoded == std::vector<std::string>{"Zebra.jpg", "clip.mp4"}));
    // The grid shows the same thumbnails from the cache.
    h.state().peekActive()->view.set(app::ViewMode::grid);
    h.pump(50);
    auto *tile = static_cast<ui::components::GridTile *>(h.rowFor("Zebra.jpg"));
    EXPECT(tile != nullptr && tile->data().thumbnail != 0 && decoded.size() == 2);
    tile = static_cast<ui::components::GridTile *>(h.rowFor("clip.mp4"));
    EXPECT(tile != nullptr && tile->data().thumbnail != 0 && decoded.size() == 2);

    // The app's decoder sends videos to the frame grab and the rest to the image decoder; on a
    // host both give nothing, and the cache doesn't ask again.
    EXPECT(!app::decodeFileThumbnail(tree.path("Download/clip.mp4"), 128));
    EXPECT(!app::decodeFileThumbnail(tree.path("Download/Zebra.jpg"), 128));
    EXPECT(!app::decodeVideoThumbnail(tree.path("Download/clip.mp4"), 128));

    // The cache: LRU eviction and cancellation.
    ui::UiLoop loop;
    ui::IoPool io(loop, 1);
    gfx::vulkan::ImageStore images;
    std::atomic<int> calls{0};
    app::ThumbnailCache cache(io, images, [&](const std::string &, int) -> std::optional<app::DecodedImage> {
        ++calls;
        return app::DecodedImage{1, 1, {1, 2, 3, 4}};
    }, 2);
    int done = 0;
    for (const char *key : {"a", "b", "c"}) {
        cache.request(key, key, 8, app::ThumbnailPersistence::memoryOnly, [&](gfx::ImageId) { ++done; });
    }
    cache.request("d", "d", 8, app::ThumbnailPersistence::memoryOnly, [&](gfx::ImageId) { ++done; });
    cache.cancelExcept({"a", "b", "c"});
    for (int i = 0; i < 500 && done < 3; ++i) {
        loop.runPending();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT(done == 3 && cache.cached() == 2 && cache.get("a") == 0 && cache.get("c") != 0);

    // A file that gives nothing (a video without a decodable frame, or one that timed out) isn't
    // decoded again for the same key right away. It's tried once more after kThumbnailRetryMs, as
    // the failure may have been a timeout; then never. A changed file (a new key) is.
    std::atomic<int> failing{0};
    std::int64_t nowMs = 1'000;
    app::ThumbnailCache failed(
            io, images,
            [&](const std::string &, int) -> std::optional<app::DecodedImage> {
                ++failing;
                return std::nullopt;
            },
            app::ThumbnailCacheOptions{.retryClock = [&] { return nowMs; }});
    auto settleFailed = [&] {
        for (int i = 0; i < 500 && failed.pending() > 0; ++i) {
            loop.runPending();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };
    failed.request("v|1|9", "v", 8, app::ThumbnailPersistence::memoryOnly, {});
    settleFailed();
    failed.request("v|1|9", "v", 8, app::ThumbnailPersistence::memoryOnly, {});
    EXPECT(failed.pending() == 0 && failing == 1);
    nowMs += app::kThumbnailRetryMs - 1;
    failed.request("v|1|9", "v", 8, app::ThumbnailPersistence::memoryOnly, {});
    EXPECT(failed.pending() == 0 && failing == 1);
    nowMs += 1;
    failed.request("v|1|9", "v", 8, app::ThumbnailPersistence::memoryOnly, {});
    settleFailed();
    EXPECT(failing == 2);
    nowMs += 10 * app::kThumbnailRetryMs;
    failed.request("v|1|9", "v", 8, app::ThumbnailPersistence::memoryOnly, {});
    EXPECT(failed.pending() == 0 && failing == 2);
    failed.request("v|2|9", "v", 8, app::ThumbnailPersistence::memoryOnly, {});
    settleFailed();
    EXPECT(failing == 3 && failed.cached() == 0);
}

// The app's own decoder (AppOptions::decoder unset): videos go to the frame grab and images to
// the image decoder, through the disk tier, so a restart shows them without decoding.
void testDefaultThumbnailDecoder() {
    Tree tree;
    makeFile(tree.home / "Download" / "clip.mp4", 9'000'000, 2026, 10, 11);
    makeFile(tree.home / "Download" / "trip.mkv", 9'000'000, 2026, 10, 11);
    std::mutex mutex;
    std::vector<std::string> videos;
    std::vector<std::string> images;
    auto record = [&mutex](std::vector<std::string> &into) {
        return [&mutex, &into](const std::string &path, int) -> std::optional<app::DecodedImage> {
            const std::lock_guard lock(mutex);
            into.push_back(fs::path(path).filename().string());
            return app::DecodedImage{4, 4, std::vector<std::uint8_t>(64, 200)};
        };
    };
    app::AppOptions options;
    options.thumbnailSources = {.image = record(images), .video = record(videos)};
    const std::string cache = (tree.files / "cache").string();
    fs::create_directories(cache);  // The system makes the real one.
    auto shown = [](AppHarness &h) {
        return h.waitUntil([&] {
            for (const char *name : {"Zebra.jpg", "clip.mp4", "trip.mkv"}) {
                auto *row = static_cast<ui::components::FileRow *>(h.rowFor(name));
                if (row == nullptr || row->data().thumbnail == 0) {
                    return false;
                }
            }
            return true;
        });
    };
    {
        AppHarness h(tree.path(), tree.files.string(), options, false);
        h.cacheDir = cache;
        h.start();
        h.state().navigate(local(tree.path("Download")));
        h.settle(100);
        EXPECT(shown(h));
        const std::lock_guard lock(mutex);
        std::ranges::sort(videos);
        EXPECT((videos == std::vector<std::string>{"clip.mp4", "trip.mkv"}));
        EXPECT((images == std::vector<std::string>{"Zebra.jpg"}));
    }
    std::size_t stored = 0;
    for (const auto &file : fs::directory_iterator(cache + "/thumbnails")) {
        stored += file.path().extension() == ".fpt" ? 1 : 0;
    }
    EXPECT(stored == 3);
    // Restarted with decoders that would fail: the disk tier has them.
    options.thumbnailSources = {.image = {}, .video = {}};
    AppHarness h(tree.path(), tree.files.string(), options, false);
    h.cacheDir = cache;
    h.start();
    h.state().navigate(local(tree.path("Download")));
    h.settle(100);
    EXPECT(shown(h));
}

// Local APKs show their app icon, from the platform, kept on disk and drawn as an icon; remote
// ones get none.
void testApkThumbnails() {
    // Which files ask for a thumbnail, and where from.
    auto entry = [](std::string name, vfs::EntryType type = vfs::EntryType::kFile) {
        vfs::Entry e;
        e.name = std::move(name);
        e.type = type;
        e.targetType = type;
        return e;
    };
    const vfs::Location here = local("/sdcard/Download");
    const vfs::Location server = vfs::Location::sftp("box", "/srv");
    EXPECT(app::thumbnailSource(here, entry("app.apk")) == app::ThumbnailSource::local);
    EXPECT(app::thumbnailSource(here, entry("Base.APK")) == app::ThumbnailSource::local);
    EXPECT(app::thumbnailSource(here, entry("clip.mp4")) == app::ThumbnailSource::local);
    EXPECT(app::thumbnailSource(here, entry("notes.md")) == app::ThumbnailSource::none);
    EXPECT(app::thumbnailSource(here, entry("pics.apk", vfs::EntryType::kDirectory)) == app::ThumbnailSource::none);
    EXPECT(app::thumbnailSource(server, entry("app.apk")) == app::ThumbnailSource::none);
    EXPECT(app::thumbnailSource(server, entry("clip.mp4")) == app::ThumbnailSource::none);
    EXPECT(app::thumbnailSource(server, entry("photo.jpg")) == app::ThumbnailSource::remote);
    const vfs::Location archive = vfs::Location::archive(local("/sdcard/a.zip"));
    EXPECT(app::thumbnailSource(archive, entry("app.apk")) == app::ThumbnailSource::none);
    EXPECT(app::isIconThumbnail(vfs::FileKind::kApk) && !app::isIconThumbnail(vfs::FileKind::kImage));

    // The decoder sends .apk files to the APK source only.
    std::vector<std::string> calls;
    auto named = [&calls](const char *source) {
        return [&calls, source](const std::string &, int) -> std::optional<app::DecodedImage> {
            calls.emplace_back(source);
            return std::nullopt;
        };
    };
    const app::ImageDecoder decoder =
            app::fileThumbnailDecoder({.image = named("image"), .video = named("video"), .apk = named("apk")});
    decoder("/sdcard/app.apk", 64);
    decoder("/sdcard/clip.webm", 64);
    decoder("/sdcard/photo.png", 64);
    EXPECT((calls == std::vector<std::string>{"apk", "video", "image"}));
    // Without an APK source, an APK gets nothing (not the image decoder).
    calls.clear();
    EXPECT(!app::fileThumbnailDecoder({.image = named("image"), .video = named("video")})("/sdcard/app.apk", 64));
    EXPECT(calls.empty());

    // The platform's premultiplied pixels come out unpremultiplied, the size asked for; a wrong
    // size or none is nothing.
    test::FakePlatform platform;
    EXPECT(!app::decodeApkThumbnail(platform, "/sdcard/app.apk", 4));
    platform.apkPixel = std::array<std::uint8_t, 4>{64, 32, 0, 128};
    auto icon = app::decodeApkThumbnail(platform, "/sdcard/app.apk", 4);
    EXPECT(icon && icon->width == 4 && icon->height == 4 && icon->rgba.size() == 64);
    EXPECT(icon->rgba[0] == 128 && icon->rgba[1] == 64 && icon->rgba[2] == 0 && icon->rgba[3] == 128);
    EXPECT(platform.apkIcons.size() == 2 && platform.apkIcons[1].second == 4);

    // Through the app: the row shows the icon as an icon, and it's stored on disk.
    Tree tree;
    makeFile(tree.home / "Download" / "app.apk", 3'000'000, 2026, 10, 11);
    const std::string cache = (tree.files / "cache").string();
    fs::create_directories(cache);
    app::AppOptions options;
    options.thumbnailSources = {.image = {}, .video = {}};
    AppHarness h(tree.path(), tree.files.string(), options, false);
    h.cacheDir = cache;
    h.platform.apkPixel = std::array<std::uint8_t, 4>{0, 0, 255, 255};
    h.start();
    h.state().navigate(local(tree.path("Download")));
    h.settle(100);
    EXPECT(h.waitUntil([&] {
        auto *row = static_cast<ui::components::FileRow *>(h.rowFor("app.apk"));
        return row != nullptr && row->data().thumbnail != 0;
    }));
    auto *row = static_cast<ui::components::FileRow *>(h.rowFor("app.apk"));
    EXPECT(row->data().iconThumbnail && !row->data().video);
    {
        const std::lock_guard lock(h.platform.apkMutex);
        EXPECT(h.platform.apkIcons.size() == 1 && h.platform.apkIcons[0].first == tree.path("Download/app.apk") &&
               h.platform.apkIcons[0].second > 100);
    }
    std::size_t stored = 0;
    for (const auto &file : fs::directory_iterator(cache + "/thumbnails")) {
        stored += file.path().extension() == ".fpt" ? 1 : 0;
    }
    EXPECT(stored == 1);
    // Other files there don't ask the platform.
    {
        const std::lock_guard lock(h.platform.apkMutex);
        EXPECT(h.platform.apkIcons.size() == 1);
    }
}

void testAccessibility() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    h.root->setAccessibilityEnabled(true);
    h.state().navigate(local(tree.path("Download")));
    h.settle(100);
    const auto *apple = h.root->semantics().find(h.rowFor("apple.md")->id());
    EXPECT(apple != nullptr);
    EXPECT(apple->label == "apple.md, Text, 18 KiB, modified 8 Oct 2026");
    const auto *papers = h.root->semantics().find(h.rowFor("Papers")->id());
    EXPECT(papers != nullptr && papers->label == "Papers, Folder, 1 item, modified 2 Oct 2026");
    // Every button in the tree has a label.
    std::function<void(const ui::Node &)> check = [&](const ui::Node &node) {
        if (!node.style().display || node.semantics().hidden) {
            return;
        }
        if (node.semantics().role == ui::Role::button) {
            if (node.semantics().label.empty() && node.semanticsText().empty()) {
                std::fprintf(stderr, "unlabelled button: %s\n", std::string(node.debugName()).c_str());
                EXPECT(false);
            }
        }
        for (const auto &child : node.children()) {
            check(*child);
        }
    };
    check(*h.app->root());
    // Behind a sheet, nothing else is in semantics.
    h.app->showTabs();
    h.pump(300);
    EXPECT(h.root->semantics().find(h.rowFor("apple.md")->id()) == nullptr);
    EXPECT(h.root->semantics().find(h.app->overflowButton().id()) == nullptr);
}

void testFormat() {
    EXPECT(app::formatSize(512) == "512 B" && app::formatSize(18'000) == "18 KiB" &&
           app::formatSize(2'400'000) == "2.3 MiB" && app::formatSize(812'000'000) == "774 MiB" &&
           app::formatSize(1'200'000'000) == "1.1 GiB");
    EXPECT(app::displayExtension("dataset-v4.tar.zst") == "ZST" && app::displayExtension(".profile").empty());
    // Meta lines: sentence case, no extension, and the year only when it isn't this one.
    app::setCurrentYearForTesting(2026);
    vfs::Entry file;
    file.name = "report.pdf";
    file.type = file.targetType = vfs::EntryType::kFile;
    file.hasStat = true;
    file.size = 2'400'000;
    file.modifiedTime.seconds = 1791806400;  // 12 Oct 2026, midday UTC.
    EXPECT(app::metaLine(file, std::nullopt) == "2.3 MiB · 12 Oct");
    file.modifiedTime.seconds = 1760270400;  // 12 Oct 2025.
    EXPECT(app::metaLine(file, std::nullopt) == "2.3 MiB · 12 Oct 2025");
    vfs::Entry folder;
    folder.name = "Papers";
    folder.type = folder.targetType = vfs::EntryType::kDirectory;
    folder.hasStat = true;
    folder.modifiedTime.seconds = 1791806400;
    EXPECT(app::metaLine(folder, 8) == "8 items · 12 Oct");
    app::setCurrentYearForTesting(std::nullopt);
}

// Every displayed node under root with this debug name, in tree order.
std::vector<ui::Node *> allNamed(ui::Node &root, std::string_view name) {
    std::vector<ui::Node *> out;
    std::function<void(ui::Node &)> walk = [&](ui::Node &node) {
        if (!node.style().display) {
            return;
        }
        if (node.debugName() == name) {
            out.push_back(&node);
        }
        for (const auto &child : node.children()) {
            walk(*child);
        }
    };
    walk(root);
    return out;
}

bool iconFilled(ui::Node &button) {
    return static_cast<ui::components::Icon &>(*button.children()[0]).textStyle().iconFill > 0.5f;
}

std::vector<std::string> favoriteNames(AppHarness &h) {
    std::vector<std::string> out;
    for (const auto &f : h.state().settings.peek().favorites) {
        out.push_back(f.name);
    }
    return out;
}

void testFavoritesData() {
    using Kind = app::Favorite::Kind;
    // Bookmark lines from older files read as folder favorites, and are written back as
    // favorite lines.
    const std::string papers = app::percentEncode(app::encodeLocation(local("/sd/Download/Papers")));
    auto old = app::parseSettings("filepan-settings 1\nbookmark Papers " + papers + "\nbookmark - " +
                                  app::percentEncode(app::encodeLocation(local("/"))) + "\nbookmark broken %G0\n");
    EXPECT(old && old->favorites.size() == 2);
    EXPECT(old->favorites[0] == (app::Favorite{"Papers", local("/sd/Download/Papers"), Kind::folder}));
    EXPECT(old->favorites[1].name.empty() && old->favorites[1].location == local("/"));
    const std::string rewritten = app::serializeSettings(*old);
    EXPECT(rewritten.find("bookmark ") == std::string::npos);
    EXPECT(rewritten.find("favorite Papers " + papers + " folder\n") != std::string::npos);
    EXPECT(app::parseSettings(rewritten)->favorites == old->favorites);
    // A line naming a place twice keeps the first; an unknown kind is skipped.
    auto twice = app::parseSettings("filepan-settings 1\nfavorite A " + papers + " folder\nfavorite B " + papers +
                                    " file\nfavorite C " + papers + "x thing\n");
    EXPECT(twice && twice->favorites.size() == 1 && twice->favorites[0].name == "A");

    // Following FilePan's own renames, moves and deletes.
    app::Settings s;
    s.favorites = {{"Papers", local("/sd/Download/Papers"), Kind::folder},
                   {"Report", local("/sd/Download/Papers/q3.pdf"), Kind::file},
                   {"Mine", local("/sd/Download"), Kind::folder},
                   {"Music", local("/sd/Music"), Kind::folder},
                   {"Remote", vfs::Location::sftp("nas", "/sd/Download"), Kind::folder}};
    s.homeTarget = local("/sd/Download/Papers");
    EXPECT(app::relocateUserItems(s, local("/sd/Download"), local("/sd/Inbox")));
    EXPECT(s.homeTarget == local("/sd/Inbox/Papers"));
    // Below it follows; a favorite named after it takes the new name, one the user named keeps theirs.
    EXPECT(s.favorites[0].location == local("/sd/Inbox/Papers") && s.favorites[0].name == "Papers");
    EXPECT(s.favorites[1].location == local("/sd/Inbox/Papers/q3.pdf"));
    EXPECT(s.favorites[2].location == local("/sd/Inbox") && s.favorites[2].name == "Mine");
    // Another backend's same path doesn't.
    EXPECT(s.favorites[4].location == vfs::Location::sftp("nas", "/sd/Download"));
    EXPECT(app::relocateUserItems(s, local("/sd/Inbox/Papers"), local("/sd/Inbox/Docs")));
    EXPECT(s.favorites[0].name == "Docs" && s.favorites[1].location == local("/sd/Inbox/Docs/q3.pdf"));
    EXPECT(!app::relocateUserItems(s, local("/elsewhere"), local("/other")));
    EXPECT(app::dropUserItems(s, {local("/sd/Inbox/Docs")}));
    EXPECT(!s.homeTarget);
    EXPECT(s.favorites.size() == 3 && s.favorites[0].name == "Mine");
    EXPECT(!app::dropUserItems(s, {local("/sd/Nothing")}));
    EXPECT(app::rebaseLocation(local("/a/b/c"), local("/a"), vfs::Location::sftp("x", "/")) ==
           vfs::Location::sftp("x", "/b/c"));
}

void testFavorites() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    h.root->setAccessibilityEnabled(true);
    h.state().navigate(local(tree.path("Download")));
    h.settle(100);
    // The dock's star: outlined until the folder is a favorite.
    ui::Node *star = findLabel(h.browse().dock().idleRow(), "Favorites");
    EXPECT(star != nullptr && !iconFilled(*star));
    EXPECT(findLabel(h.browse().dock().idleRow(), "Change view") == nullptr);

    // The sheet, empty: no edit toggle, and the way to add this folder.
    h.tap(*star);
    h.pump(400);
    auto *sheet = h.app->context().overlays.topSheet();
    EXPECT(sheet != nullptr);
    EXPECT(findLabel(sheet->panel(), "No favorites yet. Star a folder here, or a file from its menu.") != nullptr);
    EXPECT(findLabel(sheet->panel(), "Edit favorites") == nullptr);
    ui::Node *add = findLabel(sheet->panel(), "Add Download to favorites");
    EXPECT(add != nullptr);
    h.tap(*add);
    h.pump(50);
    EXPECT(favoriteNames(h) == std::vector<std::string>{"Download"} && iconFilled(*star));
    EXPECT(findLabel(sheet->panel(), "Remove Download from favorites") != nullptr);
    EXPECT(allNamed(sheet->panel(), "FavoriteRow").size() == 1 && findLabel(sheet->panel(), "Edit favorites"));
    h.app->back();
    EXPECT(h.waitUntil([&] { return h.app->context().overlays.openCount.peek() == 0; }));

    // From the selection's menu: several at once, files and folders.
    h.state().setSelection({"Papers", "apple.md"});
    h.pump(30);
    h.browse().showMoreMenu(h.browse().dock());
    h.pump(300);
    ui::Node *addSome = findLabel(*h.app->root(), "Add to favorites");
    EXPECT(addSome != nullptr);
    h.tap(*addSome);
    h.pump(300);
    EXPECT((favoriteNames(h) == std::vector<std::string>{"Download", "Papers", "apple.md"}));
    EXPECT(h.state().settings.peek().favorites[2].kind == app::Favorite::Kind::file);
    EXPECT(h.state().peekActive()->selection.peek().empty());
    // All of them already: the menu takes them off.
    h.state().setSelection({"Papers", "apple.md"});
    h.pump(30);
    h.browse().showMoreMenu(h.browse().dock());
    h.pump(300);
    EXPECT(findLabel(*h.app->root(), "Add to favorites") == nullptr);
    ui::Node *removeSome = findLabel(*h.app->root(), "Remove from favorites");
    EXPECT(removeSome != nullptr);
    h.app->context().overlays.dismissAll();
    EXPECT(h.waitUntil([&] { return h.app->context().overlays.openCount.peek() == 0; }));
    // One of them not: add again (adding skips the one that is).
    h.state().setSelection({"Papers", "file1.txt"});
    h.pump(30);
    h.browse().showMoreMenu(h.browse().dock());
    h.pump(300);
    h.tap(*findLabel(*h.app->root(), "Add to favorites"));
    h.pump(300);
    EXPECT((favoriteNames(h) == std::vector<std::string>{"Download", "Papers", "apple.md", "file1.txt"}));

    // The overflow menu's item says which, with the star filled.
    h.app->showOverflow();
    h.pump(300);
    EXPECT(findLabel(*h.app->root(), "Remove from favorites") != nullptr);
    h.app->context().overlays.dismissAll();
    EXPECT(h.waitUntil([&] { return h.app->context().overlays.openCount.peek() == 0; }));

    // Reordering and renaming.
    h.state().moveFavorite(3, 0);
    EXPECT((favoriteNames(h) == std::vector<std::string>{"file1.txt", "Download", "Papers", "apple.md"}));
    h.state().moveFavorite(0, 3);
    h.state().renameFavorite(1, "Docs");
    h.state().renameFavorite(1, "");
    EXPECT((favoriteNames(h) == std::vector<std::string>{"Download", "Docs", "apple.md", "file1.txt"}));

    // The sheet in edit mode: handles, rename and remove buttons.
    h.app->context().overlays.snackbars().dismissCurrent();
    app::showFavorites(h.app->context());
    h.pump(400);
    sheet = h.app->context().overlays.topSheet();
    auto rows = allNamed(sheet->panel(), "FavoriteRow");
    EXPECT(rows.size() == 4);
    EXPECT(rows[1]->semantics().label == "Docs, folder, Internal / Download / Papers");
    // Every row offers its actions to a screen reader.
    EXPECT(rows[0]->semantics().customActions.size() == 3 && rows[1]->semantics().customActions.size() == 4);
    h.tap(*findLabel(sheet->panel(), "Edit favorites"));
    h.pump(50);
    EXPECT(findLabel(sheet->panel(), "Done editing") != nullptr);
    EXPECT(findLabel(sheet->panel(), "Add Download to favorites") == nullptr &&
           findLabel(sheet->panel(), "Remove Download from favorites") == nullptr);
    EXPECT(allNamed(sheet->panel(), "DragHandle").size() == 4);
    // Dragging the first row's handle down two rows puts it third.
    auto handles = allNamed(sheet->panel(), "DragHandle");
    const gfx::Vec2 from = h.center(*handles[0]);
    h.pointer(ui::PointerAction::down, from);
    for (int i = 1; i <= 12; ++i) {
        h.pointer(ui::PointerAction::move, {from.x, from.y + 116.0f * static_cast<float>(i) / 12});
        h.pump(16);
    }
    h.pointer(ui::PointerAction::up, {from.x, from.y + 116});
    h.pump(50);
    EXPECT((favoriteNames(h) == std::vector<std::string>{"Docs", "apple.md", "Download", "file1.txt"}));
    // The sheet stayed up.
    EXPECT(h.app->context().overlays.topSheet() == sheet);
    // Move up as an accessibility action.
    rows = allNamed(sheet->panel(), "FavoriteRow");
    rows[2]->semantics().onAction(ui::Action::custom, {.customId = 3});
    h.pump(30);
    EXPECT((favoriteNames(h) == std::vector<std::string>{"Docs", "Download", "apple.md", "file1.txt"}));
    // Rename, in its own sheet.
    h.tap(*findLabel(sheet->panel(), "Rename Download"));
    h.pump(400);
    auto *rename = h.app->context().overlays.topSheet();
    EXPECT(rename != nullptr && rename != sheet);
    auto &field = static_cast<ui::components::TextField &>(*allNamed(rename->panel(), "TextField")[0]);
    EXPECT(field.editor().editingValue().text == "Download");
    field.editor().updateEditingValue({.text = "Inbox", .selection = {5, 5}});
    h.pump(30);
    field.editor().performAction(ui::ImeAction::done);
    h.pump(400);
    EXPECT(favoriteNames(h)[1] == "Inbox");
    EXPECT(h.waitUntil([&] { return h.app->context().overlays.topSheet() == sheet; }));
    // Remove, with an undo once the sheet is gone.
    h.tap(*findLabel(sheet->panel(), "Remove apple.md"));
    h.pump(50);
    EXPECT((favoriteNames(h) == std::vector<std::string>{"Docs", "Inbox", "file1.txt"}));
    h.app->back();
    EXPECT(h.waitUntil([&] { return h.app->context().overlays.openCount.peek() == 0; }));
    auto &snackbars = h.app->context().overlays.snackbars();
    EXPECT(h.waitUntil([&] {
        if (snackbars.current() && snackbars.current()->text != "Removed “apple.md” from favorites") {
            snackbars.dismissCurrent();
        }
        return snackbars.current().has_value();
    }));
    snackbars.current()->onAction();
    EXPECT((favoriteNames(h) == std::vector<std::string>{"Docs", "Inbox", "apple.md", "file1.txt"}));

    // Opening: a folder in this tab; a file in its folder, scrolled to and tinted for a moment.
    h.state().navigate(local(tree.path("Music")));
    h.settle(50);
    app::showFavorites(h.app->context());
    h.pump(400);
    sheet = h.app->context().overlays.topSheet();
    h.tap(*allNamed(sheet->panel(), "FavoriteRow")[0]);
    h.settle(100);
    EXPECT(shown(h) == local(tree.path("Download/Papers")));
    EXPECT(h.app->context().overlays.topSheet() == nullptr || !h.app->context().overlays.topSheet()->isOpen());
    app::openFavorite(h.app->context(), h.state().settings.peek().favorites[3]);
    h.settle(100);
    EXPECT(shown(h) == local(tree.path("Download")));
    EXPECT(h.browse().flashing() == std::optional<std::string>("file1.txt"));
    EXPECT(h.waitUntil([&] {
        auto *row = static_cast<ui::components::FileRow *>(h.rowFor("file1.txt"));
        return row != nullptr && row->data().flash;
    }));
    EXPECT(h.waitUntil([&] { return !h.browse().flashing(); }, 4000));
    EXPECT(h.platform.opened.empty());

    // One that's gone (outside FilePan) shows as unavailable, and opening it says so.
    fs::remove(tree.home / "Download" / "file1.txt");
    h.services->resumed.set(false);
    h.services->resumed.set(true);
    EXPECT(h.waitUntil([&] { return h.app->context().missingFavorites.peek().size() == 1; }));
    app::showFavorites(h.app->context());
    h.pump(400);
    sheet = h.app->context().overlays.topSheet();
    EXPECT(allNamed(sheet->panel(), "FavoriteRow")[3]->semantics().label.ends_with(", unavailable"));
    h.app->back();
    EXPECT(h.waitUntil([&] { return h.app->context().overlays.openCount.peek() == 0; }));
    app::openFavorite(h.app->context(), h.state().settings.peek().favorites[3]);
    EXPECT(h.waitUntil([&] {
        if (snackbars.current() && snackbars.current()->text != "“file1.txt” isn't there any more") {
            snackbars.dismissCurrent();
        }
        return snackbars.current().has_value();
    }));
    // The home lists them as Favorites.
    h.state().navigateHome();
    h.pump(50);
    EXPECT(h.app->home().favoriteRows().size() == 4);
    EXPECT(h.app->home().favoriteRows()[3]->semantics().label.ends_with(", unavailable"));
}

void testHiddenItems() {
    Tree tree;
    makeFile(tree.home / "Download" / "beach.jpg", 3'000, 2026, 10, 1);
    AppHarness h(tree.path(), tree.files.string());
    h.state().navigate(local(tree.path("Download")));
    h.settle(100);
    auto &snackbars = h.app->context().overlays.snackbars();
    const auto has = [&](const char *name) { return contains(h.names(), name); };
    const auto row = [&](const char *name) { return static_cast<ui::components::FileRow *>(h.rowFor(name)); };
    // Hide two from the selection's menu: they go, as dot files do.
    h.state().setSelection({"Papers", "apple.md"});
    h.pump(30);
    h.browse().showMoreMenu(h.browse().dock());
    h.pump(300);
    ui::Node *hide = findLabel(*h.app->root(), "Hide");
    EXPECT(hide != nullptr && findLabel(*h.app->root(), "Unhide") == nullptr);
    h.tap(*hide);
    h.pump(300);
    EXPECT(h.state().settings.peek().hiddenItems ==
           (std::vector<vfs::Location>{local(tree.path("Download/Papers")), local(tree.path("Download/apple.md"))}));
    EXPECT(!has("Papers") && !has("apple.md") && !has(".hidden") && has("file1.txt"));
    EXPECT(h.state().peekActive()->selection.peek().empty());
    // Undo puts them back.
    EXPECT(snackbars.current() && snackbars.current()->text == "Hid 2 items");
    snackbars.current()->onAction();
    h.pump(30);
    EXPECT(has("Papers") && has("apple.md") && h.state().settings.peek().hiddenItems.empty());
    // Hidden again, from a row's own menu, one at a time.
    const auto hideOne = [&](const char *name) {
        const auto &model = h.browse().model();
        h.browse().setHidden({local(tree.path("Download")), {model.visible()[*model.indexOf(name)]}}, true);
    };
    hideOne("apple.md");
    hideOne("Papers");
    hideOne("beach.jpg");
    h.pump(30);
    EXPECT(!has("apple.md") && !has("Papers") && !has("beach.jpg"));
    // The filter (search) leaves them out too.
    h.state().peekActive()->filter.set("apple");
    h.pump(30);
    EXPECT(h.names().empty());
    h.state().peekActive()->filter.set("");
    h.pump(30);
    // The viewer swipes only through what's shown.
    open(h, "Zebra.jpg");
    EXPECT(h.app->viewer() != nullptr && h.app->viewer()->items().size() == 1);
    h.app->closeViewer();
    h.pump(50);
    // Item counts still count everything in a folder, as they do dot files.
    h.state().navigate(local(tree.path()));
    h.settle(200);
    EXPECT(h.waitUntil([&] { return h.browse().model().itemCount("Download") == std::optional<std::uint64_t>(10); }));
    h.state().navigate(local(tree.path("Download")));
    h.settle(100);

    // With hidden files shown, both kinds show, dimmed; the user's say so in their meta line.
    h.state().updateSettings([](app::Settings &s) { s.showHidden = true; });
    h.pump(50);
    EXPECT(has("apple.md") && has("Papers") && has(".hidden"));
    EXPECT(row("apple.md")->data().dimmed && row("apple.md")->data().meta.starts_with("Hidden · "));
    EXPECT(row("apple.md")->data().spokenLabel.ends_with(", hidden"));
    EXPECT(row(".hidden")->data().dimmed && !row(".hidden")->data().meta.starts_with("Hidden"));
    EXPECT(!row("file1.txt")->data().dimmed);
    // Every one selected is hidden: the menu unhides.
    h.state().setSelection({"apple.md", "Papers"});
    h.pump(30);
    h.browse().showMoreMenu(h.browse().dock());
    h.pump(300);
    ui::Node *unhide = findLabel(*h.app->root(), "Unhide");
    EXPECT(unhide != nullptr);
    h.tap(*unhide);
    h.pump(300);
    EXPECT(h.state().settings.peek().hiddenItems == std::vector<vfs::Location>{local(tree.path("Download/beach.jpg"))});
    EXPECT(!row("apple.md")->data().dimmed);
    // Mixed: hide.
    h.state().setSelection({"apple.md", "beach.jpg"});
    h.pump(30);
    h.browse().showMoreMenu(h.browse().dock());
    h.pump(300);
    EXPECT(findLabel(*h.app->root(), "Hide") != nullptr);
    h.app->context().overlays.dismissAll();
    EXPECT(h.waitUntil([&] { return h.app->context().overlays.openCount.peek() == 0; }));
    h.state().clearSelection();

    // A hidden folder still opens by its path (or a favorite).
    h.state().updateSettings([](app::Settings &s) { s.showHidden = false; });
    h.state().setHidden({local(tree.path("Download/Papers"))}, true);
    h.pump(30);
    EXPECT(!has("Papers"));
    h.state().navigate(local(tree.path("Download/Papers")));
    h.settle(100);
    EXPECT(h.names() == std::vector<std::string>{"a.pdf"});
    // Even when it's the folder being shown that gets hidden.
    h.state().setHidden({local(tree.path("Download/Papers/a.pdf")), local(tree.path("Download/Papers"))}, true);
    h.pump(30);
    EXPECT(h.names().empty() && shown(h) == local(tree.path("Download/Papers")));
    EXPECT(h.browse().emptyState() != nullptr && findLabel(*h.browse().emptyState(), "Only hidden files here."));
}

void testOverflowView() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    h.state().navigate(local(tree.path("Download")));
    h.settle(100);
    h.app->showOverflow();
    h.pump(300);
    ui::Node *list = findLabel(*h.app->root(), "List view");
    ui::Node *grid = findLabel(*h.app->root(), "Grid view");
    EXPECT(list != nullptr && grid != nullptr);
    EXPECT((list->semantics().flags & ui::semantics_flags::kChecked) &&
           !(grid->semantics().flags & ui::semantics_flags::kChecked));
    h.tap(*grid);
    h.pump(50);
    // This tab and the default; the menu stays open.
    EXPECT(h.state().peekActive()->view.peek() == app::ViewMode::grid &&
           h.state().settings.peek().view == app::ViewMode::grid);
    EXPECT(h.app->context().overlays.openCount.peek() == 1);
    EXPECT(grid->semantics().flags & ui::semantics_flags::kChecked);
    EXPECT(h.browse().items().back().type == app::BrowseScreen::Item::tiles);
    h.tap(*list);
    h.pump(50);
    EXPECT(h.state().peekActive()->view.peek() == app::ViewMode::list);
}

}  // namespace

int main() {
    testFormat();
    testSettingsRoundTrip();
    testStartsAtHome();
    testNavigateAndBreadcrumb();
    testHomeButton();
    testSortOrders();
    testHiddenAndFilter();
    testSelectionAndRangeDrag();
    testWatcher();
    testTabsPersist();
    testBackStack();
    testScreenBack();
    testScrollRestore();
    testHaptics();
    testDataSaving();
    testDataSaverOnServer();
    testRestoredServerTabIsLazy();
    testPullToRefresh();
    testPullDisarms();
    testMenuRefresh();
    testSilentRefresh();
    testRefreshReducedMotion();
    testOpenFilesAndArchives();
    testHome();
    testThumbnails();
    testDefaultThumbnailDecoder();
    testApkThumbnails();
    testAccessibility();
    testFavoritesData();
    testFavorites();
    testOverflowView();
    testHiddenItems();
    std::puts("app_test: OK");
    return 0;
}
