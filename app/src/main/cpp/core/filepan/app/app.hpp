// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "filepan/app/app_state.hpp"
#include "filepan/app/file_actions.hpp"
#include "filepan/app/secrets.hpp"
#include "filepan/app/settings.hpp"
#include "filepan/app/thumbnails.hpp"
#include "filepan/app/video_thumbnails.hpp"
#include "filepan/media/image_source.hpp"
#include "filepan/media/video_player.hpp"
#include "filepan/runtime/app_host.hpp"
#include "filepan/text/collator.hpp"
#include "filepan/ui/components/components.hpp"
#include "filepan/ui/node.hpp"

// FilePan's app: the root node, its navigation, and what screens share.
//
//   App            The root: background, the active tab's screen, the top bar (the path, or
//                  the filter), overlays and snackbars; tabs live in the overflow menu. Owns the AppState and saves settings.
//   AppContext     What every screen gets: services, state, file actions, overlays, thumbnails.
//   ScreenRegistry Screens other than home and browsing (settings, about, SFTP servers): a later
//                  screen registers a ScreenSpec, and it shows up in the overflow menu and can
//                  be opened by id with AppState::openScreen.
//   ViewerScreen   The full-screen image and video viewer (viewer_screen.hpp), over everything
//                  but the overlays while it's open.
//   TextScreen     The text viewer and editor (text_screen.hpp), in the same place.
//   FileActions    What the dock and menus call (file_actions.hpp): FileOperations
//                  (file_operations.hpp) unless AppOptions::actions replaces it.
//
// Screens: home_screen.hpp (storage home) and browse_screen.hpp (a folder).
namespace filepan::app {

struct AppContext;

struct ScreenSpec {
    std::string id;     // "settings"
    std::string title;  // Shown in the breadcrumb, tab and overflow menu.
    std::string icon;
    std::function<ui::NodePtr(AppContext &context)> build;
    // The back button, while the screen is showing and nothing covers it: true if the screen used
    // it (closing its own state), false to leave the screen. Unset leaves at once.
    std::function<bool(ui::Node &screen)> back;
    // Listed in the overflow menus.
    bool inMenu = true;
    // Whether the settings let it be opened at all (an experimental screen they can turn off). Unset
    // always does.
    std::function<bool(const Settings &settings)> enabled;
};

// The disk usage screen's id (adirstat/disk_usage_screen.hpp), registered only in builds with the
// adirstat module; settings name it to offer turning it off.
inline constexpr const char *kDiskUsageScreen = "disk-usage";

class ScreenRegistry {
public:
    // Replaces a spec with the same id.
    void add(ScreenSpec spec);
    [[nodiscard]] const ScreenSpec *find(const std::string &id) const;
    [[nodiscard]] const std::vector<ScreenSpec> &all() const { return specs_; }

private:
    std::vector<ScreenSpec> specs_;
};

// Sheets, menus and snackbars over the screen. While any overlay is open, the content behind it
// is hidden from semantics and back dismisses the topmost one.
class Overlays {
public:
    // loop: where overlays that went are released, on its next turn (they go from inside their own
    // dismiss callbacks).
    Overlays(ui::Node &layer, ui::components::SnackbarHost &snackbars, ui::UiLoop *loop = nullptr)
        : layer_(layer), snackbars_(snackbars), loop_(loop) {}

    // A bottom sheet, opened at once; returns it (dismissing it removes it).
    // onDismiss runs once it has gone, however it was closed.
    // headerTrailing goes at the header's end (an edit toggle).
    ui::components::BottomSheet &showSheet(std::string title, ui::NodePtr content, bool animate = true,
                                           std::function<void()> onDismiss = {}, ui::NodePtr headerTrailing = nullptr);
    // A centered dialog, opened at once; props.onDismiss runs once it's gone.
    ui::components::Dialog &showDialog(ui::components::DialogProps props, bool animate = true);
    // A menu dropping from anchor (any node on screen), optionally led by a header of controls
    // headerHeight dp tall, and width dp wide.
    void showMenu(std::vector<ui::components::MenuItem> items, const ui::Node &anchor, ui::NodePtr header = nullptr,
                  float headerHeight = 0, float width = 224);
    void showSnackbar(ui::components::SnackbarMessage message);
    // Dismisses the topmost overlay; false if none is open.
    bool back();
    void dismissAll();
    // Tracked: how many sheets and menus are open.
    ui::Signal<int> openCount{0};
    [[nodiscard]] ui::components::BottomSheet *topSheet() const;
    [[nodiscard]] ui::components::SnackbarHost &snackbars() const { return snackbars_; }

private:
    struct Entry {
        std::shared_ptr<ui::Node> node;
        std::function<bool()> back;
        std::function<void()> dismiss;
    };
    void remove(ui::Node *node);
    // Updates openCount, and holds snackbars while a sheet or dialog is up.
    void changed();

    ui::Node &layer_;
    ui::components::SnackbarHost &snackbars_;
    std::vector<Entry> stack_;
    ui::UiLoop *loop_ = nullptr;
};

// An image or video the viewer shows: a local file, or one on an SFTP server.
struct MediaItem {
    enum class Kind : std::uint8_t { image, video };
    // The local path, or the path on the server.
    std::string path;
    std::string name;
    std::string mimeType;
    Kind kind = Kind::image;
    // For the file actions in its menu.
    vfs::Location directory;
    vfs::Entry entry;
    vfs::Location location;
};

// The viewer's decoders; tests replace them.
struct ViewerOptions {
    media::ImageOpener imageOpener = media::openImage;
    std::function<std::shared_ptr<media::VideoBackend>()> videoBackend = media::createVideoBackend;
};

struct AppContext {
    runtime::AppServices &services;
    AppState &state;
    FileActions &actions;
    ScreenRegistry &screens;
    Overlays &overlays;
    ThumbnailCache &thumbnails;
    // ICU collation with numeric ordering for the services' locale; null falls back to vfs's
    // natural order.
    const vfs::NameCollator *collator = nullptr;
    // Mounted storage volumes, refreshed on resume and when access changes.
    ui::Signal<std::vector<runtime::StorageVolume>> volumes;
    // The active screen's scroll offset, which hides the top bar's second row.
    ui::Signal<float> barScroll{0.0f};
    // The top bar's expanded height in dp (status bar included): lists pad their top by it.
    ui::Signal<float> barHeight{0.0f};

    // Opens the full-screen viewer on items[index]; swiping moves through the rest. Set by the
    // App.
    std::function<void(std::vector<MediaItem> items, std::size_t index)> openViewer;
    // Opens a text file (its folder and its one entry) in the text editor. Set by the App.
    std::function<void(const FileTargets &file)> openText;
    // Opens a file's folder in the active tab, with the file scrolled to and briefly tinted. Set
    // by the App.
    std::function<void(const vfs::Location &file)> reveal;
    // Local favorites whose target isn't there (favorites_sheet.hpp). Set by the App.
    ui::Signal<std::vector<vfs::Location>> missingFavorites;
    // Mobile data mode is on and the network is metered: remote fetches nobody asked for (remote
    // thumbnails, viewer prefetch, folder counts on servers) pause. Set by the App.
    ui::Signal<bool> dataSaving{false};
    // Expires with the App: work that finishes later (on the I/O pool) checks it before touching
    // the context. Set by the App.
    std::weak_ptr<const void> lifetime;
    // Shows the intro again (Settings). Set by the App.
    std::function<void()> showOnboarding;

    void notify(const std::string &text) { overlays.showSnackbar({.text = text}); }
};

struct AppOptions {
    // Replaces FileOperations.
    std::function<std::unique_ptr<FileActions>(runtime::AppServices &, AppState &, Overlays &)> actions;
    // Extra screens to register.
    std::vector<ScreenSpec> screens;
    // Replaces the thumbnail decoder; an empty one decodes nothing. Unset, the app uses
    // fileThumbnailDecoder(thumbnailSources), with the platform's APK icons unless they're set.
    std::optional<ImageDecoder> decoder;
    FileThumbnailSources thumbnailSources;
    ViewerOptions viewer;
    // Delay before a settings change is written, in ms.
    std::int64_t saveDelayMs = 300;
    // Shows the intro on start until it's been seen (Settings::onboarded). Off by default so tests
    // and renders start on the storage home; createApp turns it on.
    bool onboarding = false;
};

class HomeScreen;
class BrowseScreen;
class PromptSheets;
class ServerSync;
class ViewerScreen;
class TextScreen;
class OnboardingScreen;

class App : public std::enable_shared_from_this<App> {
public:
    static std::shared_ptr<App> create(runtime::AppServices &services, AppOptions options = {});
    ~App();
    App(const App &) = delete;
    App &operator=(const App &) = delete;

    // The node the AppHost shows; it keeps the App alive. The App keeps it only until this is
    // first called.
    [[nodiscard]] ui::NodePtr root();
    [[nodiscard]] AppState &state() { return *state_; }
    [[nodiscard]] AppContext &context() { return *context_; }
    [[nodiscard]] HomeScreen &home() const { return *home_; }
    [[nodiscard]] BrowseScreen &browse() const { return *browse_; }
    [[nodiscard]] ui::components::TopBar &topBar() const { return *topBar_; }
    [[nodiscard]] ui::components::Breadcrumb &breadcrumb() const { return *breadcrumb_; }
    [[nodiscard]] ui::Node &overflowButton() const { return *overflow_; }
    [[nodiscard]] ui::Node &homeButton() const { return *homeButton_; }
    [[nodiscard]] ui::Node &closeSearchButton() const { return *closeSearch_; }
    // The registered screen showing, if any.
    [[nodiscard]] ui::Node *screenNode() const { return screen_.get(); }
    [[nodiscard]] const SettingsStore &settingsStore() const { return *store_; }
    // The host key, login and archive password sheets.
    [[nodiscard]] PromptSheets &prompts() const { return *prompts_; }
    // The viewer, while it's open.
    [[nodiscard]] ViewerScreen *viewer() const { return viewer_.get(); }
    // The text editor, while it's open.
    [[nodiscard]] TextScreen *textScreen() const { return text_.get(); }

    void openViewer(std::vector<MediaItem> items, std::size_t index);
    void closeViewer();
    void openText(const FileTargets &file);
    void closeText();
    // The intro, over everything but the overlays (onboarding_screen.hpp).
    void showOnboarding();
    [[nodiscard]] OnboardingScreen *onboarding() const { return onboarding_.get(); }
    // A server refused a connection the viewer's prefetching wanted (SftpBackend's callback,
    // brought to the UI thread): the viewer says so.
    void poolLimited(const vfs::PoolLimit &limit);
    // What to call a server by id: its saved name, or its host.
    [[nodiscard]] std::string serverName(const std::string &id) const;

    // System back: overlays, then the selection and filter, then the route stack.
    bool back();
    // Writes the settings now if a save is pending.
    void flushSettings();
    // Opens the overflow menu for the current screen.
    void showOverflow();
    // Opens the tabs sheet: every tab with its path, to switch to or close, and a new tab.
    void showTabs();
    // Opens the menu for one tab (close, close others, duplicate).
    void showTabMenu(std::uint64_t id, const ui::Node &anchor);

private:
    App(runtime::AppServices &services, AppOptions options);
    void build();
    void refreshVolumes();
    // Stats the local favorites on the pool and sets AppContext::missingFavorites.
    void checkFavorites(std::vector<Favorite> favorites);
    void scheduleSave(Settings settings);
    void showRoute(const Route &route);
    // The intro finished: it's been seen; addServer goes on to a new server.
    void closeOnboarding(bool addServer);

    runtime::AppServices &services_;
    AppOptions options_;
    // Shared with saves running on the I/O pool, which may finish after the App has gone.
    std::shared_ptr<SettingsStore> store_;
    std::unique_ptr<PlatformSecretCodec> secrets_;
    // The file kept a secret as entered (an older build's): rewrite it at once.
    bool plaintextSecrets_ = false;
    std::unique_ptr<AppState> state_;
    std::unique_ptr<text::Collator> icu_;
    std::unique_ptr<vfs::NameCollator> collator_;
    std::unique_ptr<ThumbnailCache> thumbnails_;
    ScreenRegistry registry_;
    std::unique_ptr<FileActions> actions_;
    std::unique_ptr<Overlays> overlays_;
    std::unique_ptr<AppContext> context_;

    std::weak_ptr<ui::Node> root_;
    // Until root() is first called.
    std::shared_ptr<ui::Node> ownedRoot_;
    std::shared_ptr<HomeScreen> home_;
    std::shared_ptr<BrowseScreen> browse_;
    std::shared_ptr<ui::Node> screenSlot_;
    std::shared_ptr<ui::Node> screen_;
    std::string screenId_;
    std::shared_ptr<ui::Node> homeBackground_;
    std::shared_ptr<ui::Node> browseBackground_;
    std::shared_ptr<ui::Node> content_;
    std::shared_ptr<ui::components::TopBar> topBar_;
    std::shared_ptr<ui::components::Icon> remoteGlyph_;
    std::shared_ptr<ui::components::Breadcrumb> breadcrumb_;
    std::shared_ptr<ui::Node> filterSlot_;
    std::shared_ptr<ui::components::EditableText> filterField_;
    std::shared_ptr<ui::Node> closeSearch_;
    std::shared_ptr<ui::Node> overflow_;
    std::shared_ptr<ui::Node> homeButton_;
    std::shared_ptr<ui::Node> viewerSlot_;
    std::shared_ptr<ViewerScreen> viewer_;
    std::shared_ptr<TextScreen> text_;
    ui::Signal<bool> viewerOpen_{false};
    std::shared_ptr<ui::Node> onboardingSlot_;
    std::shared_ptr<OnboardingScreen> onboarding_;
    ui::Signal<bool> onboardingOpen_{false};

    std::optional<std::string> pendingSave_;
    std::uint64_t pendingGeneration_ = 0;
    std::optional<ui::UiLoop::TimerId> saveTimer_;
    std::shared_ptr<PromptSheets> prompts_;
    std::unique_ptr<ServerSync> serverSync_;
};

// The AppFactory the shell uses.
ui::NodePtr createApp(runtime::AppServices &services);

}  // namespace filepan::app
