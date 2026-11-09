// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "filepan/gfx/text.hpp"
#include "filepan/gfx/vulkan/renderer.hpp"
#include "filepan/jobs/decision.hpp"
#include "filepan/jobs/job_queue.hpp"
#include "filepan/runtime/assets.hpp"
#include "filepan/runtime/document_picker.hpp"
#include "filepan/runtime/job_events.hpp"
#include "filepan/runtime/platform.hpp"
#include "filepan/runtime/prompt_relay.hpp"
#include "filepan/runtime/shared_items.hpp"
#include "filepan/text/font_provider.hpp"
#include "filepan/ui/environment.hpp"
#include "filepan/ui/input.hpp"
#include "filepan/ui/node.hpp"
#include "filepan/ui/reactive.hpp"
#include "filepan/ui/semantics.hpp"
#include "filepan/ui/text_input.hpp"
#include "filepan/ui/ui_loop.hpp"
#include "filepan/ui/ui_root.hpp"
#include "filepan/vfs/archive_backend.hpp"
#include "filepan/vfs/backend.hpp"
#include "filepan/vfs/document_backend.hpp"
#include "filepan/vfs/sftp_backend.hpp"

// AppHost runs FilePan's native UI: it owns the UI thread, the render thread and everything they
// use, and takes the platform's events from whatever thread they arrive on.
//
// Threads:
// - The UI thread runs UiLoop and owns UiRoot, the content, signals and the AppServices. On
//   Android it's an ALooper thread and frames are paced by its AChoreographer; on a host a timer
//   stands in for vsync. A frame is produced only when UiRoot has something to do, the activity
//   is resumed, a surface exists and the render thread has taken the previous frame.
// - The render thread owns the Renderer and the RenderTarget. It renders the latest frame the UI
//   thread produced, and re-renders the last one when the surface is recreated or resized.
// - The IoPool's workers and the JobQueue's threads post results back to the UI thread.
//
// Every public method below may be called from any thread unless it says otherwise; they post to
// the UI or render thread and return, except surfaceDestroyed() and redraw(), which block.
namespace filepan::runtime {

// What the app content can use. Lives on the UI thread, for the AppHost's lifetime.
struct AppServices {
    Platform &platform;
    ui::UiRoot &root;
    ui::UiLoop &loop;
    ui::IoPool &io;
    jobs::JobQueue &jobs;
    // Pending conflict and error decisions for the jobs.
    jobs::QueuedDecisionProvider &decisions;
    // Serves local files, archives, SFTP servers and document trees.
    vfs::BackendResolver &backends;
    vfs::ArchiveBackend &archives;
    // The app pushes its saved servers here (SftpBackend::setServer).
    vfs::SftpBackend &sftp;
    gfx::vulkan::ImageStore &images;
    // Video frames; supported() says whether hardware buffers (Android video) can be drawn.
    gfx::vulkan::ExternalImageStore &externalImages;
    gfx::TextSystem &text;
    // Refreshed from the platform when the activity resumes.
    ui::Signal<bool> allFilesAccess;
    ui::Signal<bool> notificationsAllowed;
    ui::Signal<bool> resumed;
    // The default network is metered (mobile data); the shell reports changes.
    ui::Signal<bool> metered;
    // Where browsing starts: /storage/emulated/0 on Android, a directory of choice on a host.
    std::string homePath;
    // The app's private files directory (settings live here); empty keeps nothing.
    std::string filesDir;
    // The app's cache directory (thumbnails), which the system may clear; empty keeps nothing.
    std::string cacheDir;
    // BCP 47, for collation and formatting.
    std::string locale;
    // Where the app installs its archive password and SFTP sheets; null where nothing asks.
    PromptRelay *prompts = nullptr;
    // The system document picker; null where there is none.
    DocumentPicker *documents = nullptr;
    // The system picker for saving a document; null where there is none.
    DocumentSaver *saver = nullptr;
    // Document trees the user granted (kDocument locations); the app pushes its saved trees here
    // (DocumentBackend::setTrees). Null where the platform has no documents.
    vfs::DocumentBackend *documentTrees = nullptr;
    // The system picker for adding a document tree; null where there is none.
    DocumentTreePicker *treePicker = nullptr;
    // The bundled assets (licenses); null where there are none.
    const AssetSource *assets = nullptr;
    // The app's version name and build number, for the about screen; empty on a dev host.
    std::string version;
    std::string build;
    // Every job progress update, on the UI thread (after the jobs notification has seen it).
    JobEvents jobEvents;
    // Files and text shared to FilePan; the app listens while it can save them.
    ShareInbox shares;
    // Other apps asking for a document; the app listens while it can answer.
    DocumentRequestInbox documentRequests;
};

// Builds the root content. Called once on the UI thread.
using AppFactory = std::function<ui::NodePtr(AppServices &services)>;

// A window to render into, created and released on the render thread. On Android, create wraps
// the ANativeWindow in a surface target and release drops the window's reference; host tests use
// an offscreen target.
struct SurfaceSource {
    std::function<gfx::vulkan::Result<std::unique_ptr<gfx::vulkan::RenderTarget>>(
            gfx::vulkan::Renderer &renderer)>
            create;
    std::function<void()> release;
};

struct AppHostConfig {
    std::unique_ptr<text::FontProvider> fonts;
    std::string locale = "en";
    std::string homePath;
    // The renderer's pipeline cache is kept here between runs; empty keeps none.
    std::string cacheDir;
    // The app's private files: settings. Empty keeps nothing between runs.
    std::string filesDir;
    // The bundled assets; null for none.
    std::unique_ptr<AssetSource> assets;
    // The app's version name and build number.
    std::string version;
    std::string build;
    AppFactory factory;
    gfx::vulkan::RendererOptions renderer;
    // Run on the UI thread when it starts and just before it exits (JNI attaches and detaches it).
    std::function<void()> onUiThreadStart;
    std::function<void()> onUiThreadExit;
    // The host's stand-in for vsync. Ignored on Android, where AChoreographer paces frames.
    std::int64_t hostFrameIntervalNs = 16'666'667;
};

// Platform state the shell reports, all at once whenever any of it changes.
struct EnvironmentUpdate {
    float density = 1;    // px per dp.
    float fontScale = 1;  // sp per dp.
    ui::Insets safeArea;  // px.
    ui::Insets imeInsets;
    bool reducedMotion = false;
    bool powerSave = false;
    bool night = true;  // Dark mode.

    bool operator==(const EnvironmentUpdate &) const = default;
};

class AppHost {
public:
    // Starts the render thread (creating the renderer) and then the UI thread (building the
    // content), and returns once both are running. Fails if the renderer can't be created.
    static std::expected<std::unique_ptr<AppHost>, std::string> create(Platform &platform,
                                                                       AppHostConfig config);
    // Stops the UI thread (destroying the content), then the render thread.
    ~AppHost();
    AppHost(const AppHost &) = delete;
    AppHost &operator=(const AppHost &) = delete;

    // --- Surface (from SurfaceHolder.Callback) ---
    void surfaceCreated(SurfaceSource source, int width, int height);
    void surfaceChanged(int width, int height);
    // Blocks until the render thread has finished with the window and released it.
    void surfaceDestroyed();
    // Blocks until the last frame was rendered again into the current surface (or 1 s passed):
    // SurfaceHolder.Callback2.surfaceRedrawNeeded.
    void redraw();

    // --- Input ---
    // One MotionEvent's samples, historical ones first, in order.
    void pointerEvents(std::vector<ui::PointerEvent> events);
    void keyEvent(ui::KeyEvent event);
    // Calls Platform::finishActivity if nothing handled it.
    void back();

    // --- IME (offsets are UTF-8 bytes; the shell converts) ---
    // The IME edited the text: its whole new state.
    // session is the TextInputState::session the IME's view was shown with; a stale one is ignored.
    void imeEditingValue(ui::TextEditingValue value, std::uint64_t session);
    void imeAction(ui::ImeAction action, std::uint64_t session);

    // --- Environment and lifecycle ---
    void setEnvironment(EnvironmentUpdate update);
    // Pausing stops frame production; resuming restarts it and refreshes allFilesAccess and
    // notificationsAllowed.
    void setResumed(bool resumed);
    // The default network changed to (or from) a metered one.
    void setNetwork(bool metered);

    // --- Accessibility ---
    // Turning it on sends a full semantics snapshot with the next frame.
    void setAccessibilityEnabled(bool enabled);
    // The shell lost its node map (a new provider): send everything again.
    void requestFullSemantics();
    void accessibilityAction(std::uint32_t nodeId, ui::Action action, ui::ActionArgs args);

    // --- Document picker ---
    // The answer to Platform::pickDocument(request, ...).
    void documentPicked(std::uint64_t request, PickedDocument document);
    // The answer to Platform::saveDocument(request, ...).
    void documentSaved(std::uint64_t request, SavedDocument document);
    // The answer to Platform::openDocumentTree(request).
    void documentTreePicked(std::uint64_t request, PickedTree tree);

    // --- Sharing to FilePan ---
    // Files or text another app shared (shared_items.hpp), for the app to offer to save.
    void shareReceived(SharedItems share);
    // Another app asking for a document, or going away without an answer (shared_items.hpp).
    void documentRequested(DocumentRequest request);

    // --- Tests and diagnostics ---
    struct Stats {
        std::uint64_t framesProduced = 0;  // By the UI thread.
        std::uint64_t framesRendered = 0;  // Presented or drawn offscreen.
        std::uint64_t renderErrors = 0;
    };
    [[nodiscard]] Stats stats() const;
    // Runs fn on the UI thread and waits for it.
    void runOnUiThread(std::function<void()> fn);
    // Runs fn on the render thread with the renderer and the current target (null without a
    // surface), and waits for it.
    void runOnRenderThread(
            std::function<void(gfx::vulkan::Renderer &, gfx::vulkan::RenderTarget *)> fn);

    struct Impl;

private:
    explicit AppHost(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace filepan::runtime
