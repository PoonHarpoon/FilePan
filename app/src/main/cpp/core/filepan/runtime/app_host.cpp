// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/runtime/app_host.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "filepan/byte_writer.hpp"
#include "filepan/runtime/ime_bridge.hpp"
#include "filepan/runtime/log.hpp"
#include "filepan/runtime/vsync.hpp"
#include "filepan/text/text_system.hpp"
#include "filepan/ui/reactive.hpp"
#include "filepan/vfs/local_backend.hpp"

namespace filepan::runtime {

namespace {

using gfx::vulkan::Renderer;
using gfx::vulkan::RenderTarget;

// A one-shot signal that one thread waits on.
class Latch {
public:
    void set() {
        {
            std::lock_guard lock(mutex_);
            done_ = true;
        }
        condition_.notify_all();
    }
    void wait() {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return done_; });
    }
    bool waitFor(std::chrono::nanoseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] { return done_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool done_ = false;
};

constexpr char kPipelineCacheName[] = "/vk_pipeline_cache.bin";

std::vector<std::uint8_t> readFile(const std::string &path) {
    std::vector<std::uint8_t> data;
    std::FILE *file = std::fopen(path.c_str(), "rbe");
    if (file == nullptr) {
        return data;
    }
    std::uint8_t buffer[65536];
    std::size_t got;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        data.insert(data.end(), buffer, buffer + got);
    }
    std::fclose(file);
    return data;
}

void writeFileAtomically(const std::string &path, const std::vector<std::uint8_t> &data) {
    const std::string temporary = path + ".tmp";
    std::FILE *file = std::fopen(temporary.c_str(), "wbe");
    if (file == nullptr) {
        return;
    }
    const bool ok = std::fwrite(data.data(), 1, data.size(), file) == data.size();
    if (std::fclose(file) == 0 && ok) {
        std::rename(temporary.c_str(), path.c_str());
    } else {
        std::remove(temporary.c_str());
    }
}


}  // namespace


struct AppHost::Impl {
    Impl(Platform &platform, AppHostConfig config)
        : platform(platform), config(std::move(config)) {}

    Platform &platform;
    AppHostConfig config;
    std::unique_ptr<gfx::TextSystem> text;
    ui::UiLoop loop;

    std::atomic<std::uint64_t> framesProduced{0};
    std::atomic<std::uint64_t> framesRendered{0};
    std::atomic<std::uint64_t> renderErrors{0};

    // --- Render thread ---
    std::thread renderThread;
    std::mutex renderMutex;
    std::condition_variable renderCondition;
    std::deque<std::function<void()>> renderCommands;
    struct PendingFrame {
        std::shared_ptr<const gfx::DisplayList> list;
        gfx::Color clear;
    };
    std::optional<PendingFrame> pendingFrame;
    bool renderQuit = false;
    // The UI thread handed a frame the render thread hasn't taken yet.
    std::atomic<bool> frameInFlight{false};
    // Render thread only.
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<RenderTarget> target;
    std::function<void()> releaseSurface;
    std::shared_ptr<const gfx::DisplayList> lastList;
    gfx::Color lastClear;

    // --- UI thread ---
    std::thread uiThread;
    std::atomic<bool> uiQuit{false};
    std::unique_ptr<VsyncWaiter> waiter;
    std::mutex waiterMutex;  // Guards waiter for wake() from other threads.
    // UI thread only.
    std::unique_ptr<ui::UiRoot> root;
    std::unique_ptr<ui::IoPool> io;
    std::shared_ptr<jobs::QueuedDecisionProvider> decisions;
    std::shared_ptr<vfs::SimpleBackendResolver> backends;
    std::shared_ptr<vfs::ArchiveBackend> archives;
    std::shared_ptr<vfs::SftpBackend> sftp;
    std::shared_ptr<vfs::DocumentBackend> documentTrees;
    // Declines until the app installs its sheets.
    std::shared_ptr<PromptRelay> prompts = std::make_shared<PromptRelay>();
    std::unique_ptr<jobs::JobQueue> jobQueue;
    std::unique_ptr<AppServices> services;
    std::unique_ptr<ImeBridge> ime;
    bool surfaceReady = false;
    // Bumped by every surfaceCreated and surfaceDestroyed (on the caller's thread), so a late
    // "ready" for a surface that's already gone is ignored.
    std::atomic<std::uint64_t> surfaceGeneration{0};
    bool resumed = true;
    bool needFullSemantics = false;
    std::unique_ptr<JobNotifier> notifier;
    std::unique_ptr<DocumentPicker> documents;
    std::unique_ptr<DocumentSaver> saver;
    std::unique_ptr<DocumentTreePicker> treePicker;

    // ---- Render thread ----

    void postRender(std::function<void()> command) {
        {
            std::lock_guard lock(renderMutex);
            renderCommands.push_back(std::move(command));
        }
        renderCondition.notify_one();
    }

    void renderLoop() {
        while (true) {
            std::unique_lock lock(renderMutex);
            renderCondition.wait(lock, [this] {
                return renderQuit || !renderCommands.empty() || pendingFrame.has_value();
            });
            if (!renderCommands.empty()) {
                auto command = std::move(renderCommands.front());
                renderCommands.pop_front();
                lock.unlock();
                command();
                continue;
            }
            if (pendingFrame) {
                PendingFrame frame = std::move(*pendingFrame);
                pendingFrame.reset();
                frameInFlight.store(false, std::memory_order_release);
                lock.unlock();
                // The UI thread may be holding a frame back until this one was taken.
                wakeUi();
                renderFrame(std::move(frame.list), frame.clear);
                continue;
            }
            if (renderQuit) {
                return;
            }
        }
    }

    // Takes a frame the UI thread handed over but the loop hasn't picked up yet.
    std::optional<PendingFrame> takePendingFrame() {
        std::lock_guard lock(renderMutex);
        std::optional<PendingFrame> frame = std::move(pendingFrame);
        pendingFrame.reset();
        if (frame) {
            frameInFlight.store(false, std::memory_order_release);
        }
        return frame;
    }

    void renderFrame(std::shared_ptr<const gfx::DisplayList> list, gfx::Color clear) {
        lastList = std::move(list);
        lastClear = clear;
        renderLast();
    }

    void renderLast() {
        if (!target || !lastList) {
            return;
        }
        if (auto result = renderer->render(*target, *lastList, lastClear); !result) {
            renderErrors.fetch_add(1, std::memory_order_relaxed);
            FILEPAN_LOGE("render failed: %s (%d)", result.error().message.c_str(),
                         result.error().result);
        } else {
            framesRendered.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void destroyTarget() {
        if (!target) {
            return;
        }
        renderer->waitIdle();
        target.reset();
        if (releaseSurface) {
            releaseSurface();
            releaseSurface = nullptr;
        }
    }

    void savePipelineCache() {
        if (!config.cacheDir.empty() && renderer) {
            writeFileAtomically(config.cacheDir + kPipelineCacheName, renderer->pipelineCacheData());
        }
    }

    // ---- UI thread ----

    void wakeUi() {
        std::lock_guard lock(waiterMutex);
        if (waiter) {
            waiter->wake();
        }
    }

    void postUi(std::function<void()> task) { loop.post(std::move(task)); }

    [[nodiscard]] bool canProduceFrame() const {
        return resumed && surfaceReady && !frameInFlight.load(std::memory_order_acquire);
    }

    void onVsync(std::int64_t frameTimeNs) {
        if (canProduceFrame()) {
            loop.runFrame(frameTimeNs);
        }
    }

    // Replaces UiRoot's own frame callback: hands frames to the render thread and sends
    // semantics updates.
    void runFrame(std::int64_t frameTimeNs) {
        const auto frame = root->frame(frameTimeNs);
        if (frame && root->lastFrameStats().recorded) {
            framesProduced.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard lock(renderMutex);
                pendingFrame = PendingFrame{frame->displayList, root->env().theme.peek().colors.ground};
                frameInFlight.store(true, std::memory_order_release);
            }
            renderCondition.notify_one();
        }
        sendSemantics();
    }

    void sendSemantics() {
        if (!root->accessibilityEnabled()) {
            return;
        }
        ByteWriter writer;
        if (root->takeSemanticsUpdate(writer, needFullSemantics)) {
            needFullSemantics = false;
            platform.updateSemantics({writer.data(), writer.size()});
        }
    }

    void onJobProgress(const jobs::JobProgress &progress) {
        if (notifier) {
            notifier->onProgress(progress);
        }
        if (services) {
            services->jobEvents.publish(progress);
        }
    }

    void startUi(Latch &ready) {
        if (config.onUiThreadStart) {
            config.onUiThreadStart();
        }
        {
            std::lock_guard lock(waiterMutex);
            waiter = VsyncWaiter::create([this](std::int64_t time) { onVsync(time); },
                                         config.hostFrameIntervalNs, [this] { return loop.now(); });
        }
        loop.setWakeup([this] { wakeUi(); });

        root = std::make_unique<ui::UiRoot>(loop, text.get());
        loop.setFrameCallback([this](std::int64_t time) { runFrame(time); });
        ime = std::make_unique<ImeBridge>(platform, root->textInput());
        root->textInput().setPlatform(ime.get());
        io = std::make_unique<ui::IoPool>(loop);
        backends = std::make_shared<vfs::SimpleBackendResolver>();
        backends->set(vfs::BackendKind::kLocal, std::make_shared<vfs::LocalBackend>());
        archives = std::make_shared<vfs::ArchiveBackend>(*backends, prompts);
        backends->set(vfs::BackendKind::kArchive, archives);
        sftp = std::make_shared<vfs::SftpBackend>(
                prompts, vfs::SftpBackendOptions{.securityKeys = platform.securityKeySource()});
        backends->set(vfs::BackendKind::kSftp, sftp);
        if (auto provider = platform.documentProvider()) {
            documentTrees = std::make_shared<vfs::DocumentBackend>(std::move(provider));
            backends->set(vfs::BackendKind::kDocument, documentTrees);
        }
        decisions = std::make_shared<jobs::QueuedDecisionProvider>();
        notifier = std::make_unique<JobNotifier>(platform, loop);
        documents = std::make_unique<DocumentPicker>(platform);
        saver = std::make_unique<DocumentSaver>(platform);
        treePicker = std::make_unique<DocumentTreePicker>(platform);
        jobQueue = std::make_unique<jobs::JobQueue>(
                backends, decisions, [this](const jobs::JobProgress &progress) {
                    postUi([this, progress] { onJobProgress(progress); });
                });
        services = std::make_unique<AppServices>(AppServices{
                .platform = platform,
                .root = *root,
                .loop = loop,
                .io = *io,
                .jobs = *jobQueue,
                .decisions = *decisions,
                .backends = *backends,
                .archives = *archives,
                .sftp = *sftp,
                .images = renderer->images(),
                .externalImages = renderer->externalImages(),
                .text = *text,
                .allFilesAccess = ui::Signal<bool>(platform.hasAllFilesAccess()),
                .notificationsAllowed = ui::Signal<bool>(platform.hasNotificationPermission()),
                .resumed = ui::Signal<bool>(resumed),
                .metered = ui::Signal<bool>(platform.isNetworkMetered()),
                .homePath = config.homePath,
                .filesDir = config.filesDir,
                .cacheDir = config.cacheDir,
                .locale = config.locale,
                .prompts = prompts.get(),
                .documents = documents.get(),
                .saver = saver.get(),
                .documentTrees = documentTrees.get(),
                .treePicker = treePicker.get(),
                .assets = config.assets.get(),
                .version = config.version,
                .build = config.build,
        });
        if (config.factory) {
            root->setContent(config.factory(*services));
        }
        ready.set();
    }

    void runUi() {
        while (!uiQuit.load(std::memory_order_acquire)) {
            const auto deadline = loop.runPending();
            if (uiQuit.load(std::memory_order_acquire)) {
                break;
            }
            if (loop.frameRequested() && canProduceFrame()) {
                waiter->requestVsync();
            }
            waiter->wait(deadline);
        }
    }

    void stopUi() {
        root->setContent(nullptr);
        services.reset();
        // Cancels running jobs and waits for them; their last progress posts are dropped.
        jobQueue.reset();
        notifier.reset();
        documents.reset();
        saver.reset();
        treePicker.reset();
        io.reset();
        root->textInput().setPlatform(nullptr);
        ime.reset();
        root.reset();
        loop.setWakeup(nullptr);
        {
            std::lock_guard lock(waiterMutex);
            waiter.reset();
        }
        if (config.onUiThreadExit) {
            config.onUiThreadExit();
        }
    }
};

AppHost::AppHost(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

std::expected<std::unique_ptr<AppHost>, std::string> AppHost::create(Platform &platform,
                                                                     AppHostConfig config) {
    auto impl = std::make_unique<Impl>(platform, std::move(config));
    Impl &self = *impl;
    self.text = text::createTextSystem(std::move(self.config.fonts), {.locale = self.config.locale});

    // The renderer is created on its own thread, which then owns it.
    std::string rendererError;
    Latch rendererReady;
    self.renderThread = std::thread([&self, &rendererError, &rendererReady] {
        gfx::vulkan::RendererOptions options = self.config.renderer;
        if (!self.config.cacheDir.empty() && options.pipelineCache.empty()) {
            options.pipelineCache = readFile(self.config.cacheDir + kPipelineCacheName);
        }
        auto created = Renderer::create(*self.text, options);
        if (!created) {
            rendererError = created.error().message;
            rendererReady.set();
            return;
        }
        self.renderer = std::move(*created);
        rendererReady.set();
        self.renderLoop();
        self.destroyTarget();
        self.savePipelineCache();
        self.renderer.reset();
    });
    rendererReady.wait();
    if (!self.renderer) {
        self.renderThread.join();
        return std::unexpected("Couldn't create the renderer: " + rendererError);
    }

    Latch uiReady;
    self.uiThread = std::thread([&self, &uiReady] {
        self.startUi(uiReady);
        self.runUi();
        self.stopUi();
    });
    uiReady.wait();
    return std::unique_ptr<AppHost>(new AppHost(std::move(impl)));
}

AppHost::~AppHost() {
    impl_->uiQuit.store(true, std::memory_order_release);
    impl_->wakeUi();
    impl_->uiThread.join();
    {
        std::lock_guard lock(impl_->renderMutex);
        impl_->renderQuit = true;
    }
    impl_->renderCondition.notify_one();
    impl_->renderThread.join();
}

void AppHost::surfaceCreated(SurfaceSource source, int width, int height) {
    Impl &self = *impl_;
    const std::uint64_t generation = self.surfaceGeneration.fetch_add(1) + 1;
    self.postRender([&self, source = std::move(source), width, height, generation]() mutable {
        self.destroyTarget();
        auto created = source.create(*self.renderer);
        if (!created) {
            self.renderErrors.fetch_add(1, std::memory_order_relaxed);
            FILEPAN_LOGE("couldn't create the surface target: %s", created.error().message.c_str());
            if (source.release) {
                source.release();
            }
            return;
        }
        self.target = std::move(*created);
        self.releaseSurface = std::move(source.release);
        // Show what we had at once; the UI thread follows with a frame at the new size if needed.
        self.renderLast();
        self.postUi([&self, width, height, generation] {
            self.surfaceReady = generation == self.surfaceGeneration.load();
            self.root->env().windowSize.set(
                    {static_cast<float>(width), static_cast<float>(height)});
        });
    });
}

void AppHost::surfaceChanged(int width, int height) {
    Impl &self = *impl_;
    self.postRender([&self, width, height] {
        if (self.target) {
            self.target->resize(width, height);
        }
    });
    self.postUi([&self, width, height] {
        self.root->env().windowSize.set({static_cast<float>(width), static_cast<float>(height)});
    });
}

void AppHost::surfaceDestroyed() {
    Impl &self = *impl_;
    self.surfaceGeneration.fetch_add(1);
    self.postUi([&self] { self.surfaceReady = false; });
    Latch done;
    self.postRender([&self, &done] {
        self.destroyTarget();
        done.set();
    });
    done.wait();
}

void AppHost::redraw() {
    Impl &self = *impl_;
    auto done = std::make_shared<Latch>();
    // A frame the UI thread owes (a resize) is produced now rather than at the next vsync, then
    // the render thread draws the newest frame it has.
    self.postUi([&self, done] {
        if (self.loop.frameRequested() && self.resumed && self.surfaceReady) {
            self.loop.runFrame(self.loop.now());
        }
        self.postRender([&self, done] {
            if (auto frame = self.takePendingFrame()) {
                self.wakeUi();
                self.renderFrame(std::move(frame->list), frame->clear);
            } else {
                self.renderLast();
            }
            done->set();
        });
    });
    if (!done->waitFor(std::chrono::seconds(1))) {
        FILEPAN_LOGW("redraw timed out");
    }
}

void AppHost::pointerEvents(std::vector<ui::PointerEvent> events) {
    Impl &self = *impl_;
    self.postUi([&self, events = std::move(events)] {
        for (const auto &event : events) {
            self.root->dispatchPointer(event);
        }
    });
}

void AppHost::keyEvent(ui::KeyEvent event) {
    Impl &self = *impl_;
    self.postUi([&self, event] { self.root->dispatchKey(event); });
}

void AppHost::back() {
    Impl &self = *impl_;
    self.postUi([&self] {
        if (!self.root->dispatchBack()) {
            self.platform.finishActivity();
        }
    });
}

void AppHost::imeEditingValue(ui::TextEditingValue value, std::uint64_t session) {
    Impl &self = *impl_;
    self.postUi([&self, value = std::move(value), session] { self.ime->fromIme(value, session); });
}

void AppHost::imeAction(ui::ImeAction action, std::uint64_t session) {
    Impl &self = *impl_;
    self.postUi([&self, action, session] { self.ime->action(action, session); });
}

void AppHost::setEnvironment(EnvironmentUpdate update) {
    Impl &self = *impl_;
    self.postUi([&self, update] {
        ui::Environment &env = self.root->env();
        ui::batch([&] {
            env.density.set(update.density);
            env.fontScale.set(update.fontScale);
            env.safeArea.set(update.safeArea);
            env.imeInsets.set(update.imeInsets);
            env.reducedMotion.set(update.reducedMotion);
            env.powerSave.set(update.powerSave);
            env.night.set(update.night);
        });
    });
}

void AppHost::setResumed(bool resumed) {
    Impl &self = *impl_;
    self.postUi([&self, resumed] {
        self.resumed = resumed;
        ui::batch([&] {
            self.services->resumed.set(resumed);
            if (resumed) {
                // The user may have granted or revoked it in settings.
                self.services->allFilesAccess.set(self.platform.hasAllFilesAccess());
                self.services->notificationsAllowed.set(self.platform.hasNotificationPermission());
            }
        });
    });
}

void AppHost::setNetwork(bool metered) {
    Impl &self = *impl_;
    self.postUi([&self, metered] { self.services->metered.set(metered); });
}

void AppHost::setAccessibilityEnabled(bool enabled) {
    Impl &self = *impl_;
    self.postUi([&self, enabled] {
        if (enabled && !self.root->accessibilityEnabled()) {
            self.needFullSemantics = true;
        }
        self.root->setAccessibilityEnabled(enabled);
    });
}

void AppHost::documentPicked(std::uint64_t request, PickedDocument document) {
    Impl &self = *impl_;
    self.postUi([&self, request, document = std::move(document)]() mutable {
        if (self.documents) {
            self.documents->deliver(request, std::move(document));
        }
    });
}

void AppHost::documentSaved(std::uint64_t request, SavedDocument document) {
    Impl &self = *impl_;
    self.postUi([&self, request, document = std::move(document)]() mutable {
        if (self.saver) {
            self.saver->deliver(request, std::move(document));
        }
    });
}

void AppHost::documentTreePicked(std::uint64_t request, PickedTree tree) {
    Impl &self = *impl_;
    self.postUi([&self, request, tree = std::move(tree)]() mutable {
        if (self.treePicker) {
            self.treePicker->deliver(request, std::move(tree));
        }
    });
}

void AppHost::shareReceived(SharedItems share) {
    Impl &self = *impl_;
    // A posted task must be copyable; the descriptors move through a shared holder.
    auto held = std::make_shared<SharedItems>(std::move(share));
    self.postUi([&self, held] {
        if (self.services) {
            self.services->shares.deliver(std::move(*held));
        }
    });
}

void AppHost::documentRequested(DocumentRequest request) {
    Impl &self = *impl_;
    self.postUi([&self, request = std::move(request)]() mutable {
        if (self.services) {
            self.services->documentRequests.deliver(std::move(request));
        }
    });
}

void AppHost::requestFullSemantics() {
    Impl &self = *impl_;
    self.postUi([&self] {
        self.needFullSemantics = true;
        // Sends now if the snapshot is current; otherwise the next frame does.
        if (!self.root->needsFrame()) {
            self.sendSemantics();
        } else {
            self.root->requestFrame();
        }
    });
}

void AppHost::accessibilityAction(std::uint32_t nodeId, ui::Action action, ui::ActionArgs args) {
    Impl &self = *impl_;
    self.postUi([&self, nodeId, action, args = std::move(args)] {
        self.root->performAccessibilityAction(nodeId, action, args);
    });
}

AppHost::Stats AppHost::stats() const {
    return {impl_->framesProduced.load(std::memory_order_relaxed),
            impl_->framesRendered.load(std::memory_order_relaxed),
            impl_->renderErrors.load(std::memory_order_relaxed)};
}

void AppHost::runOnUiThread(std::function<void()> fn) {
    Latch done;
    impl_->postUi([&fn, &done] {
        fn();
        done.set();
    });
    done.wait();
}

void AppHost::runOnRenderThread(std::function<void(Renderer &, RenderTarget *)> fn) {
    Impl &self = *impl_;
    Latch done;
    self.postRender([&self, &fn, &done] {
        fn(*self.renderer, self.target.get());
        done.set();
    });
    done.wait();
}

}  // namespace filepan::runtime
