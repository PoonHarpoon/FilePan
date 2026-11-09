// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// The media viewer in the app, with fake decoders: opening from the browsing list, swiping
// through the folder's media in its order, zoom, video controls, errors with "Open with", back,
// files in archives still going out to other apps, and files on a server streaming in.

#include <archive.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/fake_media_backend.hpp"
#include "app_harness.hpp"
#include "filepan/app/format.hpp"
#include "filepan/app/viewer_screen.hpp"
#include "filepan/create_archive.hpp"

using namespace filepan;
using namespace filepan::test;
namespace fs = std::filesystem;

namespace {

// Pictures/: four media files and a text file, by name: a.jpg b.png c.gif d.mp4 notes.txt.
struct Tree {
    TempDir dir;
    fs::path home;
    fs::path files;

    Tree() {
        home = dir.path() / "storage";
        files = dir.path() / "files";
        fs::create_directories(files);
        for (const char *name : {"a.jpg", "b.png", "c.gif", "d.mp4", "notes.txt"}) {
            makeFile(home / "Pictures" / name, 1000, 2026, 10, 1);
        }
        makeFile(dir.path() / "src" / "pic.jpg", 5, 2026, 1, 1);
        struct Quiet final : CreateArchiveCallbacks {
            bool onProgress(std::uint64_t, std::uint64_t, std::string_view) override { return true; }
            bool onSourceError(std::string_view, int) override { return false; }
        } quiet;
        const auto created = createArchive((home / "Pictures" / "pack.zip").string(),
                                           {.format = ARCHIVE_FORMAT_ZIP, .filter = ARCHIVE_FILTER_NONE},
                                           {{(dir.path() / "src" / "pic.jpg").string(), "pic.jpg"}}, quiet);
        EXPECT(created.error == 0);
    }

    [[nodiscard]] std::string path(const std::string &relative = {}) const {
        return relative.empty() ? home.string() : (home / relative).string();
    }
};

// A 4000x3000 still (or a two-frame animation for .gif); fails for paths containing "broken".
class FakeImage final : public media::ImageSource {
public:
    explicit FakeImage(bool animated) {
        info_ = {4000, 3000, 4000, 3000, animated, -1};
    }
    [[nodiscard]] const media::ImageInfo &info() const override { return info_; }
    media::MediaResult<std::optional<media::ImageFrame>> next() override {
        if (frame_ >= (info_.animated ? 2 : 1)) {
            return std::nullopt;
        }
        ++frame_;
        return media::ImageFrame{4, 3, std::vector<std::uint8_t>(48, 200), info_.animated ? 100'000'000 : 0};
    }
    media::MediaResult<void> rewind() override {
        frame_ = 0;
        return {};
    }

private:
    media::ImageInfo info_;
    int frame_ = 0;
};

// What the fake decoders were given to stream from.
struct Streamed {
    std::mutex mutex;
    // The first bytes of each streamed image, as read.
    std::vector<std::vector<std::uint8_t>> images;
    std::shared_ptr<media::ByteSource> video;
};
Streamed streamed;

media::MediaResult<std::unique_ptr<media::ImageSource>> openFakeImage(const media::MediaSource &source,
                                                                      const media::DecodeLimits &) {
    if (source.bytes) {
        auto size = source.bytes->size();
        if (!size) {
            return std::unexpected(size.error());
        }
        if (*size == 1234) {
            return std::unexpected(media::MediaError{"Unsupported format"});  // broken.jpg
        }
        std::vector<std::uint8_t> head(std::min<std::uint64_t>(*size, 64));
        auto read = source.bytes->readAt(0, head);
        if (!read) {
            return std::unexpected(read.error());
        }
        std::lock_guard lock(streamed.mutex);
        streamed.images.push_back(std::move(head));
        return std::make_unique<FakeImage>(false);
    }
    const std::string &path = source.path;
    if (path.find("broken") != std::string::npos) {
        return std::unexpected(media::MediaError{"Unsupported format"});
    }
    return std::make_unique<FakeImage>(path.ends_with(".gif"));
}

// Video frames every 40 ms, as pixel frames.
class FakeVideo final : public media::VideoBackend {
public:
    [[nodiscard]] bool needsHardwareBuffers() const override { return false; }
    media::MediaResult<media::VideoInfo> open(const media::MediaSource &source) override {
        if (source.bytes) {
            std::lock_guard lock(streamed.mutex);
            streamed.video = source.bytes;
        }
        return media::VideoInfo{1920, 1080, 0, 10'000'000, false};
    }
    media::MediaResult<std::int64_t> seek(std::int64_t us) override {
        std::lock_guard lock(mutex_);
        queue_.clear();
        next_ = us;
        fill();
        return us;
    }
    void close() override {}
    void setPlaying(bool) override {}
    void setClockHint(std::int64_t) override {}
    void setOnFrame(std::function<void()>) override {}
    std::vector<std::int64_t> queuedFrames() override {
        std::lock_guard lock(mutex_);
        return {queue_.begin(), queue_.end()};
    }
    std::optional<gfx::vulkan::ExternalFrame> takeFrame(std::size_t drop) override {
        std::lock_guard lock(mutex_);
        for (std::size_t i = 0; i < drop && !queue_.empty(); ++i) {
            queue_.pop_front();
        }
        if (queue_.empty()) {
            return std::nullopt;
        }
        queue_.pop_front();
        fill();
        return gfx::vulkan::ExternalFrame::fromPixels(1, 1, {0, 0, 0, 255});
    }
    bool videoEnded() override {
        std::lock_guard lock(mutex_);
        return queue_.empty() && next_ >= 10'000'000;
    }
    std::optional<std::pair<std::int64_t, std::int64_t>> audioPosition() override { return std::nullopt; }
    bool audioEnded() override { return true; }

private:
    void fill() {
        while (queue_.size() < 3 && next_ < 10'000'000) {
            queue_.push_back(next_);
            next_ += 40'000;
        }
    }
    std::mutex mutex_;
    std::deque<std::int64_t> queue_;
    std::int64_t next_ = 0;
};

app::AppOptions fakeMedia() {
    app::AppOptions options;
    options.viewer.imageOpener = openFakeImage;
    options.viewer.videoBackend = [] { return std::make_shared<FakeVideo>(); };
    return options;
}

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

void openFolder(AppHarness &h, const Tree &tree) {
    h.state().navigate(vfs::Location::local(tree.path("Pictures")));
    h.settle(100);
}

app::ViewerScreen &openViewer(AppHarness &h, const std::string &name) {
    ui::Node *row = h.rowFor(name);
    EXPECT(row != nullptr);
    h.tap(*row);
    h.pump(100);
    EXPECT(h.app->viewer() != nullptr);
    return *h.app->viewer();
}

// A one-finger horizontal drag by dx dp from the screen's centre, a step every stepMs.
void swipe(AppHarness &h, float dx, int stepMs = 8) {
    const gfx::Vec2 start{AppHarness::kWidth / AppHarness::kDensity / 2, 400};
    h.pointer(ui::PointerAction::down, start);
    for (int step = 1; step <= 6; ++step) {
        h.pump(stepMs);
        h.pointer(ui::PointerAction::move, {start.x + dx * static_cast<float>(step) / 6, start.y});
    }
    h.pointer(ui::PointerAction::up, {start.x + dx, start.y});
    h.pump(400);
}

// Which files the viewer opens: what the platform decoders read, by kind, but not vector images.
void testViewerKind() {
    const auto kindOf = [](const char *name) {
        vfs::Entry entry;
        entry.name = name;
        entry.type = entry.targetType = vfs::EntryType::kFile;
        return app::viewerKind(entry);
    };
    EXPECT(kindOf("a.jpg") == app::MediaItem::Kind::image);
    EXPECT(kindOf("IMG.HEIC") == app::MediaItem::Kind::image);
    EXPECT(kindOf("a.mkv") == app::MediaItem::Kind::video);
    for (const auto name : {"a.svg", "a.svgz", "a.svg.gz", "a.djvu", "a.mp3", "a.txt"}) {
        EXPECT(!kindOf(name));
    }
}

void testOpenAndSwipe() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string(), fakeMedia());
    openFolder(h, tree);
    app::ViewerScreen &viewer = openViewer(h, "b.png");
    EXPECT(h.platform.opened.empty());
    // The folder's media, in its order; the text file and the archive aren't.
    EXPECT(viewer.items().size() == 4);
    EXPECT(viewer.index() == 1 && viewer.current().name == "b.png");
    EXPECT(viewer.items()[3].kind == app::MediaItem::Kind::video);
    EXPECT(viewer.page(0) && viewer.page(1) && viewer.page(2) && !viewer.page(3));
    EXPECT(static_cast<ui::Text &>(viewer.title()).text() == "b.png");
    EXPECT(h.waitUntil([&] { return viewer.page(1)->image()->image() != 0; }));
    // What's behind leaves semantics; the viewer says where it is.
    EXPECT(h.app->root()->children()[2]->semantics().hidden);
    EXPECT(viewer.stage().semantics().value == "2 of 4");
    EXPECT(static_cast<ui::Text &>(viewer.subtitle()).text() == "2 of 4 · " + app::formatSize(1000));
    EXPECT(viewer.previousButton().style().display && viewer.nextButton().style().display);
    EXPECT(!viewer.gifTag().style().display && !viewer.controls().style().display);
    // The more menu: Open with, Rename, Delete, Properties.
    h.tap(viewer.moreButton());
    EXPECT(h.app->context().overlays.openCount.peek() == 1);
    EXPECT(h.app->back());  // Closes the menu, not the viewer.
    h.pump(300);
    EXPECT(h.app->viewer() != nullptr);
    // The side buttons step too.
    h.tap(viewer.previousButton());
    h.pump(400);
    EXPECT(viewer.index() == 0 && !viewer.previousButton().style().display);
    h.tap(viewer.nextButton());
    h.pump(400);
    EXPECT(viewer.index() == 1);

    swipe(h, -200);
    EXPECT(viewer.index() == 2 && viewer.current().name == "c.gif");
    EXPECT(static_cast<ui::Text &>(viewer.title()).text() == "c.gif");
    EXPECT(!viewer.page(0) && viewer.page(3));
    // The animation plays while it's current.
    EXPECT(h.waitUntil([&] { return viewer.page(2)->image()->scheduler().shown() >= 2; }));
    EXPECT(viewer.gifTag().style().display);
    // A short, slow drag springs back.
    swipe(h, 40, 60);
    EXPECT(viewer.index() == 2);
    swipe(h, 200);
    EXPECT(viewer.index() == 1);
    // Past the first item, nothing.
    swipe(h, 200);
    swipe(h, 200);
    EXPECT(viewer.index() == 0);

    // Screen readers page with scroll actions.
    EXPECT(viewer.stage().semantics().onAction(ui::Action::scrollForward, {}));
    EXPECT(viewer.index() == 1);

    // Back closes it.
    EXPECT(h.app->back());
    h.pump(50);
    EXPECT(h.app->viewer() == nullptr);
}

void testZoom() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string(), fakeMedia());
    openFolder(h, tree);
    app::ViewerScreen &viewer = openViewer(h, "a.jpg");
    app::MediaPage *page = viewer.page(0);
    EXPECT(h.waitUntil([&] { return page->image()->image() != 0; }));
    // 4000 px wide on a 1080 px screen: up to 1 image px per screen px.
    EXPECT(std::abs(page->maxScale() - 4000.0f / 1080.0f) < 0.01f);

    const gfx::Vec2 at{200, 400};
    h.tap(at);
    h.tap(at);
    EXPECT(std::abs(page->scale() - 2.5f) < 0.01f);
    EXPECT(viewer.chromeVisible());  // A double tap doesn't toggle the chrome.
    // Zoomed, a drag pans instead of turning the page.
    swipe(h, -150);
    EXPECT(viewer.index() == 0 && page->zoomed());
    h.tap(at);
    h.tap(at);
    EXPECT(!page->zoomed());

    // Pinch: twice the distance, twice the scale; never past the decoded resolution.
    const auto finger = [&](std::int32_t id, ui::PointerAction action, gfx::Vec2 dp) {
        h.root->dispatchPointer({.pointer = id, .action = action,
                                 .position = {dp.x * AppHarness::kDensity, dp.y * AppHarness::kDensity},
                                 .timeNs = h.loop.now()});
    };
    finger(0, ui::PointerAction::down, {150, 400});
    finger(1, ui::PointerAction::down, {250, 400});
    h.pump(16);
    finger(1, ui::PointerAction::move, {350, 400});
    h.pump(16);
    EXPECT(std::abs(page->scale() - 2.0f) < 0.05f);
    finger(1, ui::PointerAction::move, {2000, 400});
    EXPECT(std::abs(page->scale() - page->maxScale()) < 0.01f);
    finger(1, ui::PointerAction::up, {2000, 400});
    finger(0, ui::PointerAction::up, {150, 400});
    h.pump(16);
    EXPECT(viewer.index() == 0);

    // A single tap toggles the chrome, after the double-tap window.
    page->resetZoom();
    h.tap({200, 600});
    h.pump(400);
    EXPECT(!viewer.chromeVisible());
    h.tap({200, 600});
    h.pump(400);
    EXPECT(viewer.chromeVisible());
}

void testVideo() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string(), fakeMedia());
    openFolder(h, tree);
    app::ViewerScreen &viewer = openViewer(h, "d.mp4");
    EXPECT(viewer.index() == 3);
    app::MediaPage *page = viewer.page(3);
    EXPECT(h.waitUntil([&] { return page->video() && page->video()->hasFrame(); }));
    // It plays as its page comes up, with the centred toggle saying so.
    EXPECT(page->video()->state() == media::VideoPlayer::State::playing);
    EXPECT(viewer.controls().style().display && viewer.playButton().style().display);
    EXPECT(h.waitUntil([&] { return viewer.playButton().semantics().label == "Pause"; }));
    EXPECT(viewer.seekBar().semantics().label == "Seek");
    EXPECT(viewer.muteButton().semantics().label == "Mute");
    EXPECT(h.waitUntil([&] { return page->video()->positionUs() > 100'000; }));
    // A tap on the picture hides the chrome, another brings it back.
    h.tap({200, 300});
    EXPECT(!viewer.chromeVisible());
    h.tap({200, 300});
    EXPECT(viewer.chromeVisible());
    // While playing it hides by itself after 2.5 s.
    EXPECT(h.waitUntil([&] { return !viewer.chromeVisible(); }, 4000));
    h.tap({200, 300});
    h.tap(viewer.muteButton());
    EXPECT(page->video()->muted() && viewer.muteButton().semantics().label == "Unmute");
    h.tap(viewer.playButton());
    EXPECT(page->video()->state() == media::VideoPlayer::State::paused);
    EXPECT(viewer.playButton().semantics().label == "Play");
    EXPECT(viewer.seekBar().semantics().onAction(ui::Action::scrollForward, {}));
    EXPECT(h.waitUntil([&] { return page->video()->positionUs() >= 100'000; }));

    // The background releases the decoders; coming back reopens where it was.
    h.services->resumed.set(false);
    h.pump(50);
    EXPECT(page->video()->suspended());
    h.services->resumed.set(true);
    h.pump(50);
    EXPECT(!page->video()->suspended());

    // Swiping away pauses it; images have no controls.
    swipe(h, 200);
    EXPECT(viewer.index() == 2 && !viewer.controls().style().display);
}

void testErrorsAndOpenWith() {
    Tree tree;
    makeFile(tree.home / "Pictures" / "broken.jpg", 10, 2026, 10, 1);
    AppHarness h(tree.path(), tree.files.string(), fakeMedia());
    openFolder(h, tree);
    app::ViewerScreen &viewer = openViewer(h, "broken.jpg");
    app::MediaPage *page = viewer.page(viewer.index());
    EXPECT(h.waitUntil([&] { return page->failed(); }));
    EXPECT(page->openWithButton() != nullptr);
    EXPECT(findLabel(*page->errorPanel(), "Can't show this image. Unsupported format") != nullptr);
    h.tap(*page->openWithButton());
    EXPECT(h.platform.opened.size() == 1 && h.platform.opened[0].first == tree.path("Pictures/broken.jpg") &&
           h.platform.opened[0].second == "*/*");
    h.tap(viewer.backButton());
    h.pump(50);
    EXPECT(h.app->viewer() == nullptr);

    // Without a video backend (a host, or a device that can't), video says so.
    app::AppOptions noVideo = fakeMedia();
    noVideo.viewer.videoBackend = nullptr;
    AppHarness h2(tree.path(), tree.files.string(), noVideo);
    openFolder(h2, tree);
    app::ViewerScreen &second = openViewer(h2, "d.mp4");
    EXPECT(second.page(second.index())->failed());

    // Media inside an archive isn't local: it's copied out and handed to other apps as before.
    h2.state().navigate(vfs::Location::archive(vfs::Location::local(tree.path("Pictures/pack.zip")), "/"));
    h2.settle(200);
    EXPECT(h2.waitUntil([&] { return h2.rowFor("pic.jpg") != nullptr; }));
    h2.app->closeViewer();
    h2.pump(50);
    h2.tap(*h2.rowFor("pic.jpg"));
    EXPECT(h2.waitUntil([&] { return !h2.platform.opened.empty(); }));
    EXPECT(h2.app->viewer() == nullptr);
    EXPECT(h2.platform.opened[0].second == "image/jpeg");
}

// Mobile data mode on a metered network: the viewer opens only the image shown, not its
// neighbours, until the network is unmetered.
void testRemoteDataSaver() {
    Tree tree;
    auto server = std::make_shared<FakeMediaBackend>();
    server->add("/Pictures/a.jpg", 3000, 1);
    server->add("/Pictures/b.png", 2000, 2);
    AppHarness h(tree.path(), tree.files.string(), fakeMedia(), false);
    h.backends->set(vfs::BackendKind::kSftp, server);
    h.platform.metered = true;
    h.start();
    h.state().updateSettings([](app::Settings &s) { s.dataSaver = true; });
    h.state().navigate(vfs::Location::sftp("nas", "/Pictures"));
    h.settle(100);
    EXPECT(h.waitUntil([&] { return h.rowFor("a.jpg") != nullptr; }));
    app::ViewerScreen &viewer = openViewer(h, "a.jpg");
    EXPECT(h.waitUntil([&] { return viewer.page(0)->image()->image() != 0; }));
    h.pump(50);
    EXPECT(viewer.page(1) == nullptr);
    EXPECT(viewer.remoteCache()->started() == (std::vector<std::size_t>{0}));
    // Swiping there opens it.
    viewer.goTo(1, false);
    EXPECT(h.waitUntil([&] { return viewer.page(1) != nullptr && viewer.page(1)->image()->image() != 0; }));
    // Unmetered, the neighbour is opened ahead again.
    viewer.goTo(0, false);
    h.pump(20);
    h.services->metered.set(false);
    EXPECT(h.waitUntil([&] { return viewer.page(1) != nullptr; }));
}

// Media on an SFTP server opens in the viewer too, read through a streamed source; video shows
// a spinner while it waits on the network.
void testRemote() {
    Tree tree;
    auto server = std::make_shared<FakeMediaBackend>();
    server->add("/Pictures/a.jpg", 3000, 1);
    server->add("/Pictures/b.png", 2000, 2);
    server->add("/Pictures/broken.jpg", 1234, 4);
    server->add("/Pictures/d.mp4", 20 << 20, 3);
    server->add("/Pictures/notes.txt", 10);
    AppHarness h(tree.path(), tree.files.string(), fakeMedia(), false);
    h.backends->set(vfs::BackendKind::kSftp, server);
    h.cacheDir = (tree.dir.path() / "cache").string();
    fs::create_directories(h.cacheDir);
    h.start();
    h.state().navigate(vfs::Location::sftp("nas", "/Pictures"));
    h.settle(100);
    EXPECT(h.waitUntil([&] { return h.rowFor("a.jpg") != nullptr; }));
    app::ViewerScreen &viewer = openViewer(h, "a.jpg");
    EXPECT(h.platform.opened.empty());
    EXPECT(viewer.items().size() == 4 && viewer.index() == 0);
    EXPECT(viewer.current().location == vfs::Location::sftp("nas", "/Pictures/a.jpg"));
    EXPECT(h.waitUntil([&] { return viewer.page(0)->image()->image() != 0; }));
    // The decoder read the file from the server: its first bytes, and the neighbour's.
    EXPECT(h.waitUntil([&] {
        std::lock_guard lock(streamed.mutex);
        return streamed.images.size() == 2;
    }));
    {
        std::lock_guard lock(streamed.mutex);
        const auto a = server->file("/Pictures/a.jpg");
        const auto b = server->file("/Pictures/b.png");
        EXPECT(std::ranges::count(streamed.images, std::vector<std::uint8_t>(a.begin(), a.begin() + 64)) == 1);
        EXPECT(std::ranges::count(streamed.images, std::vector<std::uint8_t>(b.begin(), b.begin() + 64)) == 1);
    }
    // Each within 2 of the current one has its own stream, which its page shares.
    EXPECT(h.waitUntil([&] {
        auto *cache = viewer.remoteCache();
        return cache != nullptr && cache->running() == 0 && cache->started().size() == 3;
    }));
    EXPECT(server->parallelOpens() == 3);

    // A server's connection limit is told of once.
    auto &snackbars = h.app->context().overlays.snackbars();
    h.app->poolLimited({.serverId = "nas", .working = 2, .reason = "refused"});
    h.app->poolLimited({.serverId = "nas", .working = 2, .reason = "refused"});
    h.app->poolLimited({.serverId = "elsewhere", .working = 1, .reason = "refused"});
    h.pump(50);
    EXPECT(snackbars.current() && snackbars.queued() == 0);
    EXPECT(snackbars.current()->text ==
           "nas refused another connection, probably a connection limit. Prefetching over 2 connections.");
    snackbars.dismissCurrent();

    // The video plays from the server; while a read waits on it, the play button spins.
    viewer.goTo(3, false);
    app::MediaPage *page = viewer.page(3);
    // It came into the window: one more stream, for the video's start and end.
    EXPECT(h.waitUntil([&] { return server->parallelOpens() == 4 && viewer.remoteCache()->prefetched(3); }));
    EXPECT(h.waitUntil([&] { return page->video() && page->video()->state() == media::VideoPlayer::State::playing; }));
    std::shared_ptr<media::ByteSource> bytes;
    {
        std::lock_guard lock(streamed.mutex);
        bytes = streamed.video;
    }
    EXPECT(bytes != nullptr && !viewer.buffering());
    server->hold(true);
    std::thread decoder([&] {
        std::vector<std::uint8_t> out(100);
        EXPECT(bytes->readAt(12 << 20, out).has_value());  // Past what was prefetched.
    });
    EXPECT(h.waitUntil([&] { return page->video()->waiting(); }));
    // Not at once: a short wait doesn't flash it.
    EXPECT(!viewer.buffering());
    EXPECT(h.waitUntil([&] { return viewer.buffering(); }, 2000));
    EXPECT(viewer.playButton().semantics().label == "Pause" && viewer.playButton().semantics().value == "Buffering");
    server->hold(false);
    decoder.join();
    EXPECT(h.waitUntil([&] { return !viewer.buffering(); }, 2000));
    EXPECT(viewer.playButton().semantics().value.empty());
    bytes.reset();
    {
        std::lock_guard lock(streamed.mutex);
        streamed.video.reset();
        streamed.images.clear();
    }

    // One that can't be shown goes to other apps through the file actions, copied out first.
    viewer.goTo(2, false);
    app::MediaPage *broken = viewer.page(2);
    EXPECT(h.waitUntil([&] { return broken->failed(); }));
    h.tap(*broken->openWithButton());
    EXPECT(h.waitUntil([&] { return !h.platform.opened.empty(); }));
    EXPECT(h.platform.opened[0].first.starts_with(tree.files.string() + "/cache/open/") &&
           h.platform.opened[0].first.ends_with("/broken.jpg") && h.platform.opened[0].second == "*/*");
    h.app->closeViewer();
    h.pump(50);
}

}  // namespace

int main() {
    testViewerKind();
    testOpenAndSwipe();
    testZoom();
    testVideo();
    testErrorsAndOpenWith();
    testRemote();
    testRemoteDataSaver();
    std::printf("viewer_test passed\n");
    return 0;
}
