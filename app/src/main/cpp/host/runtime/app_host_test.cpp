// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// AppHost headless: its own UI and render threads, the real text system and renderer into an
// offscreen target, a fake Platform, and the app over a temporary directory.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "filepan/app/app.hpp"
#include "filepan/runtime/app_host.hpp"
#include "filepan/runtime/shared_items.hpp"
#include "filepan/text/font_provider.hpp"
#include "png.hpp"
#include "test_util.hpp"

using namespace filepan;
using namespace std::chrono_literals;

namespace {

constexpr int kWidth = 720;
constexpr int kHeight = 1440;

std::int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

[[noreturn]] void skipOrFail(const std::string &why) {
    const char *ci = std::getenv("CI");
    if (ci != nullptr && *ci != '\0' && std::string_view(ci) != "false") {
        std::fprintf(stderr, "FAIL: no Vulkan device on CI: %s\n", why.c_str());
        std::exit(1);
    }
    std::fprintf(stderr, "SKIP: %s. The dev shell provides lavapipe.\n", why.c_str());
    std::exit(77);
}

// The semantics wire format, read back the way Kotlin's A11yBridge does: a node map that full
// snapshots replace and deltas update.
class SemanticsMirror {
public:
    struct Node {
        std::uint32_t id = 0;
        std::uint32_t parent = 0;
        std::int32_t left = 0, top = 0, right = 0, bottom = 0;
        std::uint8_t role = 0;
        std::uint16_t flags = 0;
        std::uint32_t actions = 0;
        std::string label, value, hint;
        std::vector<std::uint32_t> children;
    };

    // Returns false if the bytes don't parse.
    bool apply(std::span<const std::uint8_t> bytes) {
        Reader in{bytes};
        if (in.u32() != 0x4D535046 || in.u16() != 1) {
            return false;
        }
        const std::uint16_t flags = in.u16();
        if (flags & 1) {
            nodes_.clear();
        }
        rootId_ = in.u32();
        const std::uint32_t removed = in.u32();
        for (std::uint32_t i = 0; i < removed && in.ok; ++i) {
            nodes_.erase(in.u32());
        }
        const std::uint32_t count = in.u32();
        for (std::uint32_t i = 0; i < count && in.ok; ++i) {
            Node node;
            node.id = in.u32();
            node.parent = in.u32();
            node.left = static_cast<std::int32_t>(in.u32());
            node.top = static_cast<std::int32_t>(in.u32());
            node.right = static_cast<std::int32_t>(in.u32());
            node.bottom = static_cast<std::int32_t>(in.u32());
            node.role = in.u8();
            in.u8();
            node.flags = in.u16();
            node.actions = in.u32();
            node.label = in.str();
            node.value = in.str();
            node.hint = in.str();
            const std::uint32_t children = in.u32();
            for (std::uint32_t c = 0; c < children && in.ok; ++c) {
                node.children.push_back(in.u32());
            }
            const std::uint32_t custom = in.u32();
            for (std::uint32_t c = 0; c < custom && in.ok; ++c) {
                in.u32();
                in.str();
            }
            nodes_[node.id] = std::move(node);
        }
        return in.ok && in.at == bytes.size();
    }

    [[nodiscard]] const Node *find(std::string_view label) const {
        for (const auto &[id, node] : nodes_) {
            // Rows read "name, type, size, date": a label matches its first part too.
            if (node.label == label || (node.label.starts_with(label) && node.label.size() > label.size() &&
                                         node.label[label.size()] == ',')) {
                return &node;
            }
        }
        return nullptr;
    }
    void dump() const {
        for (const auto &[id, node] : nodes_) {
            std::fprintf(stderr, "  %u role %u [%d %d %d %d] %s\n", id, node.role, node.left, node.top, node.right,
                         node.bottom, node.label.c_str());
        }
    }
    [[nodiscard]] std::uint32_t rootId() const { return rootId_; }
    [[nodiscard]] bool consistent() const {
        if (rootId_ != 0 && !nodes_.contains(rootId_)) {
            return false;
        }
        for (const auto &[id, node] : nodes_) {
            for (std::uint32_t child : node.children) {
                if (!nodes_.contains(child)) {
                    return false;
                }
            }
        }
        return true;
    }

private:
    struct Reader {
        std::span<const std::uint8_t> bytes;
        std::size_t at = 0;
        bool ok = true;

        bool need(std::size_t n) {
            ok = ok && at + n <= bytes.size();
            return ok;
        }
        std::uint8_t u8() { return need(1) ? bytes[at++] : 0; }
        std::uint16_t u16() {
            if (!need(2)) {
                return 0;
            }
            std::uint16_t v;
            std::memcpy(&v, bytes.data() + at, 2);
            at += 2;
            return v;
        }
        std::uint32_t u32() {
            if (!need(4)) {
                return 0;
            }
            std::uint32_t v;
            std::memcpy(&v, bytes.data() + at, 4);
            at += 4;
            return v;
        }
        std::string str() {
            const std::uint32_t n = u32();
            if (!need(n)) {
                return {};
            }
            std::string s(reinterpret_cast<const char *>(bytes.data() + at), n);
            at += n;
            return s;
        }
    };

    std::map<std::uint32_t, Node> nodes_;
    std::uint32_t rootId_ = 0;
};

class FakePlatform final : public runtime::Platform {
public:
    bool hasAllFilesAccess() override { return true; }
    void requestAllFilesAccess() override {}
    std::vector<runtime::StorageVolume> storageVolumes() override { return {}; }
    void openFile(const std::string &path, const std::string &mimeType) override {
        record([&] { opened.emplace_back(path, mimeType); });
    }
    void shareFiles(const std::vector<std::string> &, const std::vector<std::string> &) override {}
    void setClipboardText(const std::string &) override {}
    void showIme(gfx::Rect, const runtime::TextInputState &) override {}
    void hideIme() override {}
    void updateIme(const runtime::TextInputState &) override {}
    void updateSemantics(std::span<const std::uint8_t> bytes) override {
        record([&] {
            ++semanticsUpdates;
            semanticsParsed = semantics.apply(bytes) && semanticsParsed;
        });
    }
    void announce(const std::string &) override {}
    void jobsActive(bool) override {}
    void updateJobNotification(const runtime::JobSummary &) override {}
    void performHaptic(runtime::HapticKind) override {}
    void finishActivity() override {
        record([&] { ++finishes; });
    }
    bool pickDocument(std::uint64_t request, const runtime::DocumentPickOptions &options) override {
        record([&] { picks.emplace_back(request, options.maxBytes); });
        return true;
    }
    bool hasNotificationPermission() override { return notificationsGranted; }
    bool saveDocument(std::uint64_t request, const runtime::DocumentSaveOptions &options) override {
        record([&] { saves.emplace_back(request, options.bytes); });
        return true;
    }

    // Waits until pred (called under the lock) holds.
    bool waitFor(const std::function<bool()> &pred, std::chrono::milliseconds timeout = 10s) {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, timeout, pred);
    }

    template <typename F>
    auto locked(F &&fn) {
        std::lock_guard lock(mutex);
        return fn();
    }

    std::mutex mutex;
    std::condition_variable condition;
    std::vector<std::pair<std::string, std::string>> opened;
    SemanticsMirror semantics;
    int semanticsUpdates = 0;
    bool semanticsParsed = true;
    int finishes = 0;
    // pickDocument's requests and maxBytes.
    std::vector<std::pair<std::uint64_t, std::size_t>> picks;
    std::vector<std::pair<std::uint64_t, std::string>> saves;
    std::atomic<bool> notificationsGranted{true};

private:
    template <typename F>
    void record(F &&fn) {
        {
            std::lock_guard lock(mutex);
            fn();
        }
        condition.notify_all();
    }
};

runtime::SurfaceSource offscreen(int *released) {
    return {
            .create = [](gfx::vulkan::Renderer &renderer) {
                return renderer.createOffscreenTarget(kWidth, kHeight);
            },
            .release = [released] { ++*released; },
    };
}

// The center of the semantics node labelled label, once it exists.
std::optional<gfx::Vec2> centerOf(FakePlatform &platform, std::string_view label) {
    std::optional<gfx::Vec2> center;
    platform.waitFor([&] {
        if (const auto *node = platform.semantics.find(label)) {
            center = gfx::Vec2{(node->left + node->right) / 2.0f, (node->top + node->bottom) / 2.0f};
            return true;
        }
        return false;
    });
    if (!center) {
        platform.locked([&] { platform.semantics.dump(); });
    }
    return center;
}

void tap(runtime::AppHost &host, gfx::Vec2 at) {
    const std::int64_t t = nowNs();
    host.pointerEvents({{.pointer = 0, .action = ui::PointerAction::down, .position = at, .timeNs = t}});
    host.pointerEvents({{.pointer = 0, .action = ui::PointerAction::up, .position = at,
                         .timeNs = t + 50'000'000}});
}

// Waits until no frame has been produced or rendered for quiet.
runtime::AppHost::Stats settle(runtime::AppHost &host, std::chrono::milliseconds quiet = 300ms) {
    auto last = host.stats();
    auto since = std::chrono::steady_clock::now();
    const auto deadline = since + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(20ms);
        const auto current = host.stats();
        if (current.framesProduced != last.framesProduced ||
            current.framesRendered != last.framesRendered) {
            last = current;
            since = std::chrono::steady_clock::now();
        } else if (std::chrono::steady_clock::now() - since >= quiet) {
            return current;
        }
    }
    EXPECT(!"the UI never went idle");
    return last;
}

void testSummarizeJobs() {
    std::map<jobs::JobId, jobs::JobProgress> active;
    EXPECT(runtime::summarizeJobs(active).activeJobs == 0);
    active[1] = {.id = 1, .kind = jobs::JobKind::kCopy, .state = jobs::JobState::kRunning,
                 .bytesDone = 250, .bytesTotal = 1000, .itemsDone = 1, .itemsTotal = 3,
                 .currentItem = "/a/b.txt"};
    auto summary = runtime::summarizeJobs(active);
    EXPECT(summary.activeJobs == 1 && summary.title == "Copying 3 items");
    EXPECT(summary.text == "250 B / 1.0 KiB · /a/b.txt" && summary.progressPermille == 250);
    active[2] = {.id = 2, .kind = jobs::JobKind::kDelete, .state = jobs::JobState::kQueued,
                 .scanning = true};
    summary = runtime::summarizeJobs(active);
    EXPECT(summary.title == "2 jobs" && summary.text == "1 waiting" && summary.progressPermille == -1);
}

// DocumentPicker alone: one pick at a time, answered once.
void testDocumentPicker() {
    using Status = runtime::PickedDocument::Status;
    // The Platform defaults have no picker.
    struct NoPicker final : runtime::Platform {
        bool hasAllFilesAccess() override { return false; }
        void requestAllFilesAccess() override {}
        std::vector<runtime::StorageVolume> storageVolumes() override { return {}; }
        void openFile(const std::string &, const std::string &) override {}
        void shareFiles(const std::vector<std::string> &, const std::vector<std::string> &) override {}
        void setClipboardText(const std::string &) override {}
        void showIme(gfx::Rect, const runtime::TextInputState &) override {}
        void hideIme() override {}
        void updateIme(const runtime::TextInputState &) override {}
        void updateSemantics(std::span<const std::uint8_t>) override {}
        void announce(const std::string &) override {}
        void jobsActive(bool) override {}
        void updateJobNotification(const runtime::JobSummary &) override {}
        void performHaptic(runtime::HapticKind) override {}
        void finishActivity() override {}
    } none;
    runtime::DocumentPicker unsupported(none);
    int calls = 0;
    EXPECT(!unsupported.pick({}, [&](runtime::PickedDocument) { ++calls; }));
    EXPECT(!unsupported.waiting() && calls == 0);

    FakePlatform platform;
    runtime::DocumentPicker picker(platform);
    std::vector<runtime::PickedDocument> answers;
    const auto record = [&](runtime::PickedDocument document) { answers.push_back(std::move(document)); };
    EXPECT(picker.pick({.maxBytes = 4096}, record));
    EXPECT(picker.waiting() && platform.picks.size() == 1 && platform.picks[0].second == 4096);
    const std::uint64_t first = platform.picks[0].first;
    // A second pick cancels the first; the first's late answer is dropped.
    EXPECT(picker.pick({}, record));
    EXPECT(answers.size() == 1 && answers[0].status == Status::cancelled);
    const std::uint64_t second = platform.picks[1].first;
    EXPECT(second != first && picker.lastRequest() == second);
    picker.deliver(first, {.status = Status::picked, .name = "old", .bytes = "x"});
    EXPECT(answers.size() == 1 && picker.waiting());
    picker.deliver(second, {.status = Status::picked, .name = "id_ed25519", .bytes = "key"});
    EXPECT(answers.size() == 2 && answers[1].name == "id_ed25519" && answers[1].bytes == "key");
    // Answered once only.
    picker.deliver(second, {.status = Status::failed});
    EXPECT(answers.size() == 2 && !picker.waiting());
    // cancel() answers a waiting pick, and nothing else.
    EXPECT(picker.pick({}, record));
    picker.cancel();
    picker.cancel();
    EXPECT(answers.size() == 3 && answers[2].status == Status::cancelled);
}

void testDocumentSaver() {
    using Status = runtime::SavedDocument::Status;
    FakePlatform platform;
    runtime::DocumentSaver saver(platform);
    std::vector<runtime::SavedDocument> answers;
    const auto record = [&](runtime::SavedDocument document) { answers.push_back(std::move(document)); };
    EXPECT(saver.save({.name = "a.filepan", .bytes = "one"}, record));
    EXPECT(saver.waiting() && platform.saves.size() == 1 && platform.saves[0].second == "one");
    const std::uint64_t first = platform.saves[0].first;
    // A second save cancels the first; the first's late answer is dropped.
    EXPECT(saver.save({.name = "b.filepan", .bytes = "two"}, record));
    EXPECT(answers.size() == 1 && answers[0].status == Status::cancelled);
    const std::uint64_t second = platform.saves[1].first;
    EXPECT(second != first && saver.lastRequest() == second);
    saver.deliver(first, {.status = Status::saved, .name = "old"});
    EXPECT(answers.size() == 1 && saver.waiting());
    saver.deliver(second, {.status = Status::saved, .name = "b.filepan"});
    EXPECT(answers.size() == 2 && answers[1].status == Status::saved && answers[1].name == "b.filepan");
    // Answered once only.
    saver.deliver(second, {.status = Status::failed});
    EXPECT(answers.size() == 2 && !saver.waiting());
}

void testAppHost() {
    test::TempDir dir;
    for (const char *name : {"a.txt", "b.txt", "c.pdf"}) {
        std::ofstream(dir.path() / name) << "hello";
    }
    std::filesystem::create_directory(dir.path() / "photos");
    std::ofstream(dir.path() / "photos" / "inside.txt") << "x";

    FakePlatform platform;
    runtime::AppHostConfig config;
    config.fonts = text::createDirectoryFontProvider(FILEPAN_FONTS_DIR);
    config.homePath = dir.path().string();
    config.cacheDir = dir.path().string() + "/photos";
    config.filesDir = dir.path().string() + "/photos";
    // Someone who's been here: no intro over the screens this drives.
    std::filesystem::create_directories(dir.path() / "photos");
    std::ofstream(dir.path() / "photos" / "settings.txt") << "filepan-settings 1\n";
    runtime::DocumentPicker *documents = nullptr;
    runtime::AppServices *services = nullptr;
    config.factory = [&documents, &services](runtime::AppServices &created) {
        documents = created.documents;
        services = &created;
        return app::createApp(created);
    };
    auto created = runtime::AppHost::create(platform, std::move(config));
    if (!created) {
        skipOrFail(created.error());
    }
    auto &host = **created;

    host.setEnvironment({.density = 2, .safeArea = {0, 48, 0, 32}});
    host.setAccessibilityEnabled(true);
    host.setResumed(true);
    // Nothing renders without a surface.
    std::this_thread::sleep_for(100ms);
    EXPECT(host.stats().framesProduced == 0);

    int released = 0;
    host.surfaceCreated(offscreen(&released), kWidth, kHeight);

    // Semantics arrive and parse, and the listing shows up in them.
    const auto bRow = centerOf(platform, "b.txt");
    EXPECT(bRow.has_value());
    EXPECT(platform.locked([&] {
        return platform.semanticsParsed && platform.semantics.consistent() &&
               platform.semantics.find("Internal") != nullptr && platform.semantics.find("a.txt") != nullptr;
    }));

    // A frame renders: the glows and text make it far from a flat fill.
    auto stats = settle(host);
    EXPECT(stats.framesProduced > 0 && stats.framesRendered > 0 && stats.renderErrors == 0);
    std::vector<std::uint8_t> pixels;
    host.runOnRenderThread([&](gfx::vulkan::Renderer &renderer, gfx::vulkan::RenderTarget *target) {
        EXPECT(target != nullptr);
        auto read = renderer.readPixels(*target);
        EXPECT(read.has_value());
        pixels = std::move(*read);
    });
    EXPECT(pixels.size() == static_cast<std::size_t>(kWidth) * kHeight * 4);
    if (const char *dump = std::getenv("FILEPAN_DUMP_RENDERS"); dump && *dump == '1') {
        test::writePng("app_host_test.png", {kWidth, kHeight, pixels});
    }
    const auto pixel = [&](int x, int y) {
        const std::uint8_t *p = &pixels[(static_cast<std::size_t>(y) * kWidth + x) * 4];
        return std::uint32_t{p[0]} << 16 | std::uint32_t{p[1]} << 8 | p[2];
    };
    std::set<std::uint32_t> colors;
    for (int y = 0; y < kHeight; y += 7) {
        for (int x = 0; x < kWidth; x += 7) {
            colors.insert(pixel(x, y));
        }
    }
    EXPECT(colors.size() > 50);

    // Idle: no frames at all.
    std::this_thread::sleep_for(400ms);
    const auto idle = host.stats();
    EXPECT(idle.framesProduced == stats.framesProduced && idle.framesRendered == stats.framesRendered);

    // Turned to landscape and back, as the shell reports it (the surface's new size, then the
    // insets moved to the sides): the target follows the surface, the frame is relaid out for
    // the new window, and nothing fails to render.
    struct Turn {
        int width;
        int height;
        ui::Insets safe;
    };
    for (const Turn &turn : {Turn{kHeight, kWidth, {64, 48, 32, 0}}, Turn{kWidth, kHeight, {0, 48, 0, 32}}}) {
        const auto before = host.stats();
        host.surfaceChanged(turn.width, turn.height);
        host.setEnvironment({.density = 2, .safeArea = turn.safe});
        host.redraw();
        const auto turned = settle(host);
        EXPECT(turned.renderErrors == 0 && turned.framesRendered > before.framesRendered &&
               turned.framesProduced > before.framesProduced);
        host.runOnRenderThread([&](gfx::vulkan::Renderer &renderer, gfx::vulkan::RenderTarget *target) {
            EXPECT(target != nullptr);
            EXPECT(target->size().width == static_cast<float>(turn.width) &&
                   target->size().height == static_cast<float>(turn.height));
            auto read = renderer.readPixels(*target);
            EXPECT(read.has_value() && read->size() == static_cast<std::size_t>(turn.width) * turn.height * 4);
        });
        // The row spans the new width.
        EXPECT(platform.waitFor([&] {
            const auto *row = platform.semantics.find("b.txt");
            return row != nullptr && row->right > turn.width * 3 / 4 && row->right <= turn.width;
        }));
    }
    const auto bRowAgain = centerOf(platform, "b.txt");
    EXPECT(bRowAgain.has_value());

    // A tap on a row opens the file: one the app has no screen for goes to the platform.
    const auto pdfRow = centerOf(platform, "c.pdf");
    EXPECT(pdfRow.has_value());
    tap(host, *pdfRow);
    EXPECT(platform.waitFor([&] { return !platform.opened.empty(); }));
    EXPECT(platform.locked([&] {
        return platform.opened[0].first == dir.path().string() + "/c.pdf" &&
               platform.opened[0].second == "application/pdf";
    }));

    // Surface destroyed: the target and window are released before the call returns, and no
    // frames are produced while there's no surface, even with work to do.
    stats = settle(host);
    host.surfaceDestroyed();
    EXPECT(released == 1);
    const auto photos = centerOf(platform, "photos");
    EXPECT(photos.has_value());
    tap(host, *photos);
    std::this_thread::sleep_for(300ms);
    EXPECT(host.stats().framesProduced == stats.framesProduced);
    EXPECT(host.stats().framesRendered == stats.framesRendered);

    // Recreated: the last frame is drawn at once, then the pending one (the folder's listing).
    host.surfaceCreated(offscreen(&released), kWidth, kHeight);
    EXPECT(centerOf(platform, "inside.txt").has_value());
    stats = settle(host);
    EXPECT(stats.framesRendered >= idle.framesRendered + 2 && stats.renderErrors == 0);

    // redraw() renders again synchronously.
    host.redraw();
    EXPECT(host.stats().framesRendered == stats.framesRendered + 1);

    // Back returns to the home folder, then to the storage home, and there it finishes the
    // activity.
    host.back();
    EXPECT(centerOf(platform, "b.txt").has_value());
    host.back();
    EXPECT(centerOf(platform, "SFTP servers").has_value());
    host.back();
    EXPECT(platform.waitFor([&] { return platform.finishes == 1; }));

    // A document pick: asked of the platform on the UI thread, answered from another thread.
    EXPECT(documents != nullptr);
    std::optional<runtime::PickedDocument> picked;
    std::thread::id answeredOn;
    std::thread::id uiThread;
    host.runOnUiThread([&] {
        uiThread = std::this_thread::get_id();
        EXPECT(documents->pick({.maxBytes = 1000}, [&](runtime::PickedDocument document) {
            platform.locked([&] {
                picked = std::move(document);
                answeredOn = std::this_thread::get_id();
                return 0;
            });
        }));
    });
    EXPECT(platform.waitFor([&] { return platform.picks.size() == 1; }));
    const std::uint64_t request = platform.locked([&] { return platform.picks[0].first; });
    std::thread([&] {
        host.documentPicked(request, {.status = runtime::PickedDocument::Status::picked, .name = "key", .bytes = "abc"});
    }).join();
    host.runOnUiThread([] {});
    EXPECT(platform.locked([&] { return picked && picked->bytes == "abc" && picked->name == "key"; }));
    EXPECT(answeredOn == uiThread);
    // A document save goes the same way.
    std::optional<runtime::SavedDocument> saved;
    host.runOnUiThread([&] {
        EXPECT(services->saver->save({.name = "b.filepan", .bytes = "xyz"}, [&](runtime::SavedDocument document) {
            platform.locked([&] {
                saved = std::move(document);
                return 0;
            });
        }));
    });
    EXPECT(platform.waitFor([&] { return platform.saves.size() == 1; }));
    const std::uint64_t saveRequest = platform.locked([&] { return platform.saves[0].first; });
    std::thread([&] {
        host.documentSaved(saveRequest, {.status = runtime::SavedDocument::Status::saved, .name = "b.filepan"});
    }).join();
    host.runOnUiThread([] {});
    EXPECT(platform.locked([&] { return saved && saved->name == "b.filepan"; }));
    // What another app shares arrives from the shell's thread and reaches the inbox's listener on
    // the UI thread.
    std::vector<std::string> sharedNames;
    std::thread::id sharedOn;
    host.runOnUiThread([&] {
        services->shares.setListener([&](runtime::SharedItems share) {
            for (const auto &item : share.items) {
                sharedNames.push_back(item.name);
            }
            sharedOn = std::this_thread::get_id();
        });
    });
    std::thread([&] {
        runtime::SharedItems share;
        share.items.push_back({.name = "shared.txt", .text = "hi"});
        host.shareReceived(std::move(share));
    }).join();
    host.runOnUiThread([&] {
        EXPECT(sharedNames == std::vector<std::string>{"shared.txt"} && sharedOn == uiThread);
        services->shares.setListener({});
    });

    // The shell reports metered networks, which the services carry on the UI thread.
    host.runOnUiThread([&] { EXPECT(!services->metered.peek()); });
    std::thread([&] { host.setNetwork(true); }).join();
    host.runOnUiThread([&] { EXPECT(services->metered.peek()); });

    // Pausing stops frames; resuming produces the owed one.
    settle(host);
    host.setResumed(false);
    host.setEnvironment({.density = 2.5f, .safeArea = {0, 48, 0, 32}});
    std::this_thread::sleep_for(200ms);
    const auto paused = host.stats();
    std::this_thread::sleep_for(200ms);
    EXPECT(host.stats().framesProduced == paused.framesProduced);
    host.setResumed(true);
    const auto resumed = settle(host);
    EXPECT(resumed.framesProduced > paused.framesProduced);
    // Resuming asks the platform again about notifications, granted or refused in between.
    host.runOnUiThread([&] { EXPECT(services->notificationsAllowed.peek()); });
    platform.notificationsGranted = false;
    host.setResumed(false);
    host.setResumed(true);
    host.runOnUiThread([&] { EXPECT(!services->notificationsAllowed.peek()); });

    created->reset();
    EXPECT(released == 2);
    // The pipeline cache was saved for the next run.
    EXPECT(std::filesystem::exists(dir.path() / "photos" / "vk_pipeline_cache.bin"));
}

}  // namespace

int main() {
    testSummarizeJobs();
    testDocumentPicker();
    testDocumentSaver();
    testAppHost();
    return 0;
}
