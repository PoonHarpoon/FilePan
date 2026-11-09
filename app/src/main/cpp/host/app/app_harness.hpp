// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "filepan/app/app.hpp"
#include "filepan/app/browse_screen.hpp"
#include "filepan/app/home_screen.hpp"
#include "filepan/jobs/decision.hpp"
#include "filepan/jobs/job_queue.hpp"
#include "filepan/text/font_provider.hpp"
#include "filepan/text/text_system.hpp"
#include "filepan/ui/ui.hpp"
#include "filepan/vfs/archive_backend.hpp"
#include "filepan/vfs/local_backend.hpp"
#include "filepan/vfs/sftp_backend.hpp"
#include "test_util.hpp"
#include "vfs/fake_document_provider.hpp"

// The app on a real UiRoot, the real text system (bundled fonts), the real local and archive
// backends and a real I/O pool, with a fake Platform: everything but the renderer and the shell.
// The loop runs on this thread on the real clock; pump() drives it.
namespace filepan::test {

// TEST ONLY: an Ed25519 private key made for these tests (ssh-keygen, no passphrase). No server
// trusts it; it's here so the key screens have a real key to parse.
inline constexpr const char *kTestPrivateKey =
        "-----BEGIN OPENSSH PRIVATE KEY-----\n"
        "b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAMwAAAAtzc2gtZW\n"
        "QyNTUxOQAAACDy2lherOp2TjlKTlBiclYqq1N/BrI4edGl+/O/1tfMVAAAAJBJNZuSSTWb\n"
        "kgAAAAtzc2gtZWQyNTUxOQAAACDy2lherOp2TjlKTlBiclYqq1N/BrI4edGl+/O/1tfMVA\n"
        "AAAEC0qDoIQQLlxOCaeROfriQkKEflJD7NMzFowDve6yk2QvLaWF6s6nZOOUpOUGJyViqr\n"
        "U38Gsjh50aX787/W18xUAAAADGZpbGVwYW4tdGVzdAE=\n"
        "-----END OPENSSH PRIVATE KEY-----\n";
inline constexpr const char *kTestPrivateKeyFingerprint = "SHA256:v8xvB45f09FlUDREUMc4q9Y8MUOLfgnor4sNQotwfRA";

class FakePlatform final : public runtime::Platform {
public:
    bool access = true;
    bool metered = false;
    // The last setLightSystemBars().
    std::optional<bool> lightSystemBars;
    bool notificationsGranted = true;
    int notificationRequests = 0;
    int notificationSettings = 0;
    std::vector<runtime::StorageVolume> volumes;
    std::vector<std::pair<std::string, std::string>> opened;
    std::vector<std::vector<std::string>> shared;
    struct DocumentAnswer {
        std::vector<std::string> paths;
        std::vector<std::string> mimeTypes;
        bool writable = false;
    };
    std::vector<DocumentAnswer> answers;
    std::string clipboard;
    std::vector<bool> active;
    std::vector<runtime::JobSummary> notifications;
    std::vector<runtime::HapticKind> haptics;
    int accessRequests = 0;
    int finishes = 0;

    bool hasAllFilesAccess() override { return access; }
    bool isNetworkMetered() override { return metered; }
    void setLightSystemBars(bool light) override { lightSystemBars = light; }
    bool hasNotificationPermission() override { return notificationsGranted; }
    void requestNotificationPermission() override { ++notificationRequests; }
    void openNotificationSettings() override { ++notificationSettings; }
    void requestAllFilesAccess() override { ++accessRequests; }
    std::vector<runtime::StorageVolume> storageVolumes() override { return volumes; }
    void openFile(const std::string &path, const std::string &mimeType) override { opened.emplace_back(path, mimeType); }
    void shareFiles(const std::vector<std::string> &paths, const std::vector<std::string> &) override {
        shared.push_back(paths);
    }
    void answerDocumentRequest(const std::vector<std::string> &paths, const std::vector<std::string> &mimeTypes,
                               bool writable) override {
        answers.push_back({paths, mimeTypes, writable});
    }
    void setClipboardText(const std::string &text) override { clipboard = text; }
    std::optional<std::string> clipboardText() override {
        return clipboard.empty() ? std::nullopt : std::optional<std::string>(clipboard);
    }
    void showIme(gfx::Rect, const runtime::TextInputState &) override {}
    void hideIme() override {}
    void updateIme(const runtime::TextInputState &) override {}
    void updateSemantics(std::span<const std::uint8_t>) override {}
    void announce(const std::string &) override {}
    void jobsActive(bool on) override { active.push_back(on); }
    void updateJobNotification(const runtime::JobSummary &summary) override { notifications.push_back(summary); }
    void performHaptic(runtime::HapticKind kind) override { haptics.push_back(kind); }
    void finishActivity() override { ++finishes; }
    void openUrl(const std::string &url) override { urls.push_back(url); }
    bool pickDocument(std::uint64_t request, const runtime::DocumentPickOptions &options) override {
        picks.emplace_back(request, options);
        return true;
    }
    bool saveDocument(std::uint64_t request, const runtime::DocumentSaveOptions &options) override {
        saves.emplace_back(request, options);
        return true;
    }
    bool openDocumentTree(std::uint64_t request) override {
        treePicks.push_back(request);
        return true;
    }
    void releaseDocumentTree(const std::string &uri) override { releasedTrees.push_back(uri); }
    std::vector<std::uint64_t> treePicks;
    std::vector<std::string> releasedTrees;

    // TEST ONLY: a trivially reversible stand-in for the Keystore (a marker, then the bytes
    // XORed with 0x5A). It hides nothing; it only lets tests tell sealed blobs from plain text.
    std::optional<std::vector<std::uint8_t>> protectSecret(std::span<const std::uint8_t> plain) override {
        if (!secretsWork) {
            return std::nullopt;
        }
        std::vector<std::uint8_t> out{'T', 'E', 'S', 'T'};
        for (const std::uint8_t b : plain) {
            out.push_back(b ^ 0x5A);
        }
        ++sealed;
        return out;
    }
    std::optional<std::vector<std::uint8_t>> unprotectSecret(std::span<const std::uint8_t> blob) override {
        if (!secretsWork || blob.size() < 4 || blob[0] != 'T' || blob[1] != 'E' || blob[2] != 'S' || blob[3] != 'T') {
            return std::nullopt;
        }
        std::vector<std::uint8_t> out;
        for (std::size_t i = 4; i < blob.size(); ++i) {
            out.push_back(blob[i] ^ 0x5A);
        }
        return out;
    }
    // APK icons: every pixel apkPixel (premultiplied, as Android gives them), or none when unset.
    // Called on I/O threads.
    std::optional<std::vector<std::uint8_t>> apkIcon(const std::string &path, int sizePx) override {
        const std::lock_guard lock(apkMutex);
        apkIcons.emplace_back(path, sizePx);
        if (!apkPixel) {
            return std::nullopt;
        }
        std::vector<std::uint8_t> pixels;
        for (int i = 0; i < sizePx * sizePx; ++i) {
            pixels.insert(pixels.end(), apkPixel->begin(), apkPixel->end());
        }
        return pixels;
    }
    std::optional<std::array<std::uint8_t, 4>> apkPixel;
    std::mutex apkMutex;
    std::vector<std::pair<std::string, int>> apkIcons;  // Under apkMutex.

    bool secretsWork = true;
    int sealed = 0;
    std::vector<std::string> urls;
    std::vector<std::pair<std::uint64_t, runtime::DocumentPickOptions>> picks;
    std::vector<std::pair<std::uint64_t, runtime::DocumentSaveOptions>> saves;
};

// Files with fixed sizes and times (noon UTC, so the date is the same in any time zone).
inline void makeFile(const std::filesystem::path &path, std::size_t size, int year, int month, int day) {
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream out(path, std::ios::binary);
        out.write("x", size > 0 ? 1 : 0);
    }
    // Sparse past the first byte, so big files cost nothing.
    std::error_code ignored;
    std::filesystem::resize_file(path, size, ignored);
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 12;
    const time_t t = timegm(&tm);
    const struct timespec times[2] = {{t, 0}, {t, 0}};
    utimensat(AT_FDCWD, path.c_str(), times, 0);
}

// Sets a file's or folder's modification time to noon UTC on the day.
inline void setTime(const std::filesystem::path &path, int year, int month, int day) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 12;
    const time_t t = timegm(&tm);
    const struct timespec times[2] = {{t, 0}, {t, 0}};
    utimensat(AT_FDCWD, path.c_str(), times, 0);
}

inline void makeFolder(const std::filesystem::path &path, int year, int month, int day) {
    std::filesystem::create_directories(path);
    setTime(path, year, month, day);
}

class AppHarness {
public:
    static constexpr float kDensity = 2.625f;
    static constexpr int kWidth = 1080;
    static constexpr int kHeight = 2400;

    // home: where browsing starts; files: the settings directory.
    // shared: a text system to use instead of a new one (the renderer's, for goldens).
    AppHarness(std::string home, std::string files, app::AppOptions options = {}, bool start = true,
               gfx::TextSystem *shared = nullptr)
        : home_(std::move(home)), files_(std::move(files)), options_(std::move(options)) {
        if (shared == nullptr) {
            owned = text::createTextSystem(text::createDirectoryFontProvider(FILEPAN_FONTS_DIR), {.locale = "en"});
            shared = owned.get();
        }
        text = shared;
        root = std::make_unique<ui::UiRoot>(loop, text);
        root->env().density.set(kDensity);
        root->env().windowSize.set({static_cast<float>(kWidth), static_cast<float>(kHeight)});
        root->env().safeArea.set({0, 63, 0, 63});
        io = std::make_unique<ui::IoPool>(loop);
        backends = std::make_shared<vfs::SimpleBackendResolver>();
        backends->set(vfs::BackendKind::kLocal, std::make_shared<vfs::LocalBackend>());
        archives = std::make_shared<vfs::ArchiveBackend>(*backends, prompts);
        backends->set(vfs::BackendKind::kArchive, archives);
        sftp = std::make_shared<vfs::SftpBackend>(prompts);
        backends->set(vfs::BackendKind::kSftp, sftp);
        backends->set(vfs::BackendKind::kDocument, documentTrees);
        decisions = std::make_shared<jobs::QueuedDecisionProvider>();
        // As AppHost does: progress goes to the UI thread, the notification, then the app.
        notifier = std::make_unique<runtime::JobNotifier>(platform, loop);
        jobQueue = std::make_unique<jobs::JobQueue>(backends, decisions, [this](const jobs::JobProgress &progress) {
            loop.post([this, progress] {
                notifier->onProgress(progress);
                if (services) {
                    services->jobEvents.publish(progress);
                }
            });
        });
        platform.volumes = {{.path = home_, .description = "Internal shared storage", .primary = true,
                             .state = "mounted", .totalBytes = 256'000'000'000, .freeBytes = 79'200'000'000}};
        if (start) {
            this->start();
        }
    }

    ~AppHarness() { stop(); }

    void start() {
        services = std::make_unique<runtime::AppServices>(runtime::AppServices{
                .platform = platform,
                .root = *root,
                .loop = loop,
                .io = *io,
                .jobs = *jobQueue,
                .decisions = *decisions,
                .backends = *backends,
                .archives = *archives,
                .sftp = *sftp,
                .images = imageStore != nullptr ? *imageStore : images,
                .externalImages = externalImages,
                .text = *text,
                .allFilesAccess = ui::Signal<bool>(platform.access),
                .notificationsAllowed = ui::Signal<bool>(platform.notificationsGranted),
                .resumed = ui::Signal<bool>(true),
                .metered = ui::Signal<bool>(platform.metered),
                .homePath = home_,
                .filesDir = files_,
                .cacheDir = cacheDir,
                .locale = "en-US",
                .prompts = prompts.get(),
                .documents = &documents,
                .saver = &saver,
                .documentTrees = documentTrees.get(),
                .treePicker = &treePicker,
                .assets = assets.get(),
                .version = version,
                .build = build,
        });
        app = app::App::create(*services, options_);
        root->setContent(app->root());
        settle();
    }

    // Destroys the App as the AppHost does on exit (which saves its settings).
    void stop() {
        if (!services) {
            return;
        }
        app.reset();
        root->setContent(nullptr);
        loop.runPending();
        services.reset();
    }

    void frame() {
        if (auto produced = root->frame(loop.now())) {
            lastFrame = produced->displayList;
        }
    }

    // Runs the loop and frames on the real clock for ms.
    void pump(int ms = 16) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        do {
            loop.runPending();
            frame();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } while (std::chrono::steady_clock::now() < end);
    }

    bool waitUntil(const std::function<bool()> &pred, int timeoutMs = 5000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < end) {
            loop.runPending();
            frame();
            if (pred()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    // Lets listings, counts and animations finish.
    void settle(int ms = 400) {
        waitUntil([&] {
            auto &model = app->browse().model();
            return model.status.peek() != app::DirectoryModel::Status::loading;
        });
        pump(ms);
    }

    // --- Input, in window dp ---
    void pointer(ui::PointerAction action, gfx::Vec2 dp) {
        root->dispatchPointer({.pointer = 0, .action = action, .position = {dp.x * kDensity, dp.y * kDensity},
                               .timeNs = loop.now()});
    }
    void tap(gfx::Vec2 dp) {
        pointer(ui::PointerAction::down, dp);
        pump(30);
        pointer(ui::PointerAction::up, dp);
        pump(30);
    }
    void tap(const ui::Node &node) { tap(center(node)); }
    void longPress(gfx::Vec2 dp, bool release = true) {
        pointer(ui::PointerAction::down, dp);
        pump(600);
        if (release) {
            pointer(ui::PointerAction::up, dp);
            pump(30);
        }
    }
    gfx::Vec2 center(const ui::Node &node) const {
        const gfx::Transform t = ui::components::nodeToWindow(node, kDensity);
        return {t.tx / kDensity + node.frame().width / 2, t.ty / kDensity + node.frame().height / 2};
    }

    app::BrowseScreen &browse() { return app->browse(); }
    app::AppState &state() { return app->state(); }

    // The names the browsing list shows, in order.
    std::vector<std::string> names() {
        std::vector<std::string> out;
        for (const auto &entry : browse().model().visible()) {
            out.push_back(entry.name);
        }
        return out;
    }

    // The row (or tile) node showing name, if it's bound.
    ui::Node *rowFor(const std::string &name) {
        auto &b = browse();
        const auto entry = b.model().indexOf(name);
        if (!entry) {
            return nullptr;
        }
        const auto item = b.itemOf(*entry);
        if (!item) {
            return nullptr;
        }
        ui::Node *node = b.list().nodeFor(*item);
        if (node == nullptr || b.items()[*item].type != app::BrowseScreen::Item::tiles) {
            return node;
        }
        return node->children()[*entry - b.items()[*item].begin].get();
    }

    FakePlatform platform;
    std::unique_ptr<gfx::TextSystem> owned;
    gfx::TextSystem *text = nullptr;
    ui::UiLoop loop;
    std::unique_ptr<ui::UiRoot> root;
    std::unique_ptr<ui::IoPool> io;
    std::shared_ptr<vfs::SimpleBackendResolver> backends;
    std::shared_ptr<vfs::ArchiveBackend> archives;
    std::shared_ptr<vfs::SftpBackend> sftp;
    // The app installs its prompt sheets here; archives and SFTP ask through it.
    std::shared_ptr<runtime::PromptRelay> prompts = std::make_shared<runtime::PromptRelay>();
    // The document picker; the platform records each pick, and tests answer with deliver().
    runtime::DocumentPicker documents{platform};
    runtime::DocumentSaver saver{platform};
    // Document trees over host directories (a tree's URI is a directory's path), and the picker for
    // them; the platform records each pick, and tests answer with deliver().
    std::shared_ptr<test::FakeDocumentProvider> documentProvider = std::make_shared<test::FakeDocumentProvider>();
    std::shared_ptr<vfs::DocumentBackend> documentTrees = std::make_shared<vfs::DocumentBackend>(documentProvider);
    runtime::DocumentTreePicker treePicker{platform};
    std::unique_ptr<runtime::AssetSource> assets = runtime::createDirectoryAssetSource(FILEPAN_ASSETS_DIR);
    // What the about screen shows.
    std::string version = "0.1.0";
    std::string build = "1";
    // The app's cache directory (the thumbnails' disk tier), set before start(); empty has none.
    std::string cacheDir;
    std::shared_ptr<jobs::QueuedDecisionProvider> decisions;
    std::unique_ptr<runtime::JobNotifier> notifier;
    std::unique_ptr<jobs::JobQueue> jobQueue;
    gfx::vulkan::ImageStore images;
    // Where the app uploads its images instead, set before start(): a renderer's, for goldens
    // that show thumbnails.
    gfx::vulkan::ImageStore *imageStore = nullptr;
    gfx::vulkan::ExternalImageStore externalImages;
    std::unique_ptr<runtime::AppServices> services;
    std::shared_ptr<app::App> app;
    std::shared_ptr<const gfx::DisplayList> lastFrame;

private:
    std::string home_;
    std::string files_;
    app::AppOptions options_;
};

}  // namespace filepan::test
