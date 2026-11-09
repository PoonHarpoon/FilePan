// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/app.hpp"

#include <algorithm>
#include <cerrno>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "filepan/app/browse_screen.hpp"
#include "filepan/app/favorites_sheet.hpp"
#include "filepan/app/file_operations.hpp"
#include "filepan/app/format.hpp"
#include "filepan/app/hidden_items_screen.hpp"
#include "filepan/app/onboarding_screen.hpp"
#include "filepan/app/home_screen.hpp"
#include "filepan/app/about_screen.hpp"
#include "filepan/app/places.hpp"
#include "filepan/app/prompt_sheets.hpp"
#include "filepan/app/remote_media.hpp"
#include "filepan/app/screen_parts.hpp"
#include "filepan/app/settings_screen.hpp"
#include "filepan/app/sftp_screens.hpp"
#include "filepan/app/text_screen.hpp"
#include "filepan/app/viewer_screen.hpp"
#include "filepan/runtime/log.hpp"

#if FILEPAN_ADIRSTAT
#include "filepan/app/adirstat/disk_usage_screen.hpp"
#endif

namespace filepan::app {

namespace {

using namespace ui;
using namespace ui::components;

constexpr const char *kSettingsFile = "/settings.txt";
// Thumbnails of local files, under the cache directory.
constexpr const char *kThumbnailsDir = "/thumbnails";
// Temporary thumbnails of remote files, apart from those.
constexpr const char *kRemoteThumbnailsDir = "/thumbnails-remote";

// The kit's haptics as the platform's. Plain taps stay silent: every button would buzz.
std::optional<runtime::HapticKind> toHapticKind(Haptic kind) {
    switch (kind) {
        case Haptic::tap: return std::nullopt;
        case Haptic::toggle:
        case Haptic::tick: return runtime::HapticKind::tick;
        case Haptic::longPress: return runtime::HapticKind::longPress;
        case Haptic::reject: return runtime::HapticKind::reject;
    }
    return std::nullopt;
}

ThumbnailStoreLimits thumbnailLimits(const Settings &settings) {
    return {.maxBytes = thumbnailCacheBytes(settings), .maxAgeSeconds = thumbnailDecaySeconds(settings)};
}

Style fill() {
    Style s;
    s.position = Position::absolute;
    s.left = 0;
    s.top = 0;
    s.right = 0;
    s.bottom = 0;
    return s;
}

// vfs's comparator plug for the ICU collator.
class IcuNameCollator final : public vfs::NameCollator {
public:
    explicit IcuNameCollator(const text::Collator &collator) : collator_(collator) {}
    [[nodiscard]] int compare(std::string_view a, std::string_view b) const override {
        return collator_.compare(a, b);
    }

private:
    const text::Collator &collator_;
};

// The content node: keeps the App alive for as long as the AppHost shows it.
class AppRoot final : public Box {
public:
    std::shared_ptr<App> app;
};

std::string routeLabel(const Route &route, const AppContext &context) {
    switch (route.kind) {
    case Route::Kind::home:
        return "Storage";
    case Route::Kind::browse:
        return route.location ? locationLabel(*route.location, context.volumes.peek()) : std::string();
    case Route::Kind::screen:
        if (const ScreenSpec *spec = context.screens.find(route.screenId)) {
            return spec->title;
        }
        return route.screenId;
    }
    return {};
}

// The top bar's path for a route: the breadcrumb's labels, from the root.
std::vector<std::string> pathLabels(const Route &route, const AppContext &context,
                                    const std::vector<runtime::StorageVolume> &volumes) {
    std::vector<std::string> labels;
    if (route.kind == Route::Kind::browse && route.location) {
        for (const Crumb &crumb : crumbsFor(*route.location, volumes)) {
            labels.push_back(crumb.label);
        }
    } else {
        labels.push_back("Storage");
        if (route.kind == Route::Kind::screen) {
            labels.push_back(routeLabel(route, context));
        }
    }
    return labels;
}

}  // namespace

// --- ScreenRegistry ---

void ScreenRegistry::add(ScreenSpec spec) {
    if (auto at = std::ranges::find_if(specs_, [&](const ScreenSpec &s) { return s.id == spec.id; });
        at != specs_.end()) {
        *at = std::move(spec);
    } else {
        specs_.push_back(std::move(spec));
    }
}

const ScreenSpec *ScreenRegistry::find(const std::string &id) const {
    const auto at = std::ranges::find_if(specs_, [&](const ScreenSpec &s) { return s.id == id; });
    return at == specs_.end() ? nullptr : &*at;
}

// --- Overlays ---

BottomSheet &Overlays::showSheet(std::string title, NodePtr content, bool animate, std::function<void()> onDismiss,
                                 NodePtr headerTrailing) {
    auto holder = std::make_shared<Node *>(nullptr);
    auto sheet = BottomSheet::create({.title = std::move(title),
                                      .content = std::move(content),
                                      .onDismiss = [this, holder, onDismiss = std::move(onDismiss)] {
                                          const auto after = onDismiss;
                                          remove(*holder);
                                          if (after) {
                                              after();
                                          }
                                      },
                                      .headerTrailing = std::move(headerTrailing)});
    *holder = sheet.get();
    BottomSheet *raw = sheet.get();
    stack_.push_back({sheet, [raw] { return raw->handleBack(); }, [raw] { raw->dismiss(); }});
    layer_.appendChild(sheet);
    changed();
    sheet->open(animate);
    return *raw;
}

Dialog &Overlays::showDialog(DialogProps props, bool animate) {
    auto holder = std::make_shared<Node *>(nullptr);
    props.onDismiss = [this, holder, onDismiss = std::move(props.onDismiss)] {
        const auto after = onDismiss;
        remove(*holder);
        if (after) {
            after();
        }
    };
    auto dialog = Dialog::create(std::move(props));
    *holder = dialog.get();
    Dialog *raw = dialog.get();
    stack_.push_back({dialog, [raw] { return raw->handleBack(); }, [raw] { raw->dismiss(); }});
    layer_.appendChild(dialog);
    changed();
    raw->open(animate);
    return *raw;
}

void Overlays::showMenu(std::vector<MenuItem> items, const Node &anchor, NodePtr header, float headerHeight,
                        float width) {
    const float density = environment().density.peek();
    const gfx::Transform t = nodeToWindow(anchor, density);
    const gfx::Rect frame = anchor.frame();
    const gfx::Rect rect{t.tx / density, t.ty / density, frame.width, frame.height};
    auto holder = std::make_shared<Node *>(nullptr);
    auto menu = Menu::create({.items = std::move(items),
                              .anchor = rect,
                              .onDismiss = [this, holder] { remove(*holder); },
                              .width = width,
                              .header = std::move(header),
                              .headerHeight = headerHeight});
    *holder = menu.get();
    Menu *raw = menu.get();
    stack_.push_back({menu, [raw] { return raw->handleBack(); }, [raw] { raw->dismiss(); }});
    layer_.appendChild(menu);
    changed();
}

void Overlays::showSnackbar(SnackbarMessage message) { snackbars_.show(std::move(message)); }

void Overlays::remove(Node *node) {
    const auto at = std::ranges::find_if(stack_, [&](const Entry &e) { return e.node.get() == node; });
    if (at == stack_.end()) {
        return;
    }
    // This usually runs inside the overlay's own dismiss callback: it's released on the next turn.
    auto keep = at->node;
    stack_.erase(at);
    layer_.removeChild(node);
    changed();
    if (loop_ != nullptr) {
        loop_->post([keep = std::move(keep)] {});
    }
}

void Overlays::changed() {
    openCount.set(static_cast<int>(stack_.size()));
    // A bar over a sheet or dialog would cover its buttons: messages wait until they're gone.
    const bool modal = std::ranges::any_of(stack_, [](const Entry &e) {
        return e.node->debugName() == "BottomSheet" || e.node->debugName() == "Dialog";
    });
    snackbars_.setHeld(modal);
}

bool Overlays::back() {
    while (!stack_.empty()) {
        Entry &top = stack_.back();
        if (top.back()) {
            return true;
        }
        // Already closing: it'll go by itself; look below it.
        remove(top.node.get());
    }
    return false;
}

void Overlays::dismissAll() {
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
        it->dismiss();
    }
}

BottomSheet *Overlays::topSheet() const {
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
        if (it->node->debugName() == "BottomSheet") {
            return static_cast<BottomSheet *>(it->node.get());
        }
    }
    return nullptr;
}

// --- App ---

std::shared_ptr<App> App::create(runtime::AppServices &services, AppOptions options) {
    auto app = std::shared_ptr<App>(new App(services, std::move(options)));
    app->build();
    return app;
}

App::App(runtime::AppServices &services, AppOptions options)
    : services_(services),
      options_(std::move(options)),
      store_(std::make_shared<SettingsStore>(services.filesDir.empty() ? std::string()
                                                                        : services.filesDir + kSettingsFile)) {
    secrets_ = std::make_unique<PlatformSecretCodec>(services.platform);
    // The platform outlives the app.
    setHapticHandler([&platform = services.platform](Haptic kind) {
        if (auto mapped = toHapticKind(kind)) {
            platform.performHaptic(*mapped);
        }
    });
    Settings settings = store_->load(secrets_.get(), &plaintextSecrets_);
    state_ = std::make_unique<AppState>(std::move(settings), services.homePath);
    icu_ = text::Collator::create(services.locale.empty() ? std::string("en") : services.locale);
    if (icu_) {
        collator_ = std::make_unique<IcuNameCollator>(*icu_);
    }
    thumbnails_ = std::make_unique<ThumbnailCache>(
            services.io, services.images,
            options_.decoder ? *options_.decoder : fileThumbnailDecoder([&] {
                FileThumbnailSources sources = options_.thumbnailSources;
                if (!sources.apk) {
                    // The platform outlives the app, and apkIcon may be called on I/O threads.
                    sources.apk = [&platform = services.platform](const std::string &path, int sizePx) {
                        return decodeApkThumbnail(platform, path, sizePx);
                    };
                }
                return sources;
            }()),
            ThumbnailCacheOptions{
                    .diskDirectory = services.cacheDir.empty() ? std::string() : services.cacheDir + kThumbnailsDir,
                    .diskLimits = thumbnailLimits(state_->settings.peek()),
                    .backends = &services.backends,
                    .remoteDirectory =
                            services.cacheDir.empty() ? std::string() : services.cacheDir + kRemoteThumbnailsDir,
            });
    thumbnails_->setRemote(state_->settings.peek().remoteThumbnails,
                           remoteThumbnailMaxBytes(state_->settings.peek()));
    if (!services.cacheDir.empty()) {
        // What the viewer spilled in a run that was killed (its files are unlinked as made, but
        // not on file systems without O_TMPFILE until a moment later).
        services.io.execute([directory = services.cacheDir + kRemoteSpillDir] { clearRemoteSpill(directory); });
    }
}

App::~App() {
    setHapticHandler(nullptr);
    services_.sftp.setOnPoolLimited(nullptr);
    if (services_.prompts != nullptr) {
        services_.prompts->setPasswordProvider(nullptr);
        services_.prompts->setSftpPrompts(nullptr);
    }
    if (prompts_) {
        prompts_->shutdown();
    }
    flushSettings();
    if (saveTimer_) {
        services_.loop.cancel(*saveTimer_);
    }
}

NodePtr App::root() {
    // The first call hands the root over; from then on whoever shows it keeps it (and the App).
    if (ownedRoot_) {
        return std::move(ownedRoot_);
    }
    return root_.lock();
}

void App::build() {
    auto root = std::make_shared<AppRoot>();
    root->app = shared_from_this();
    root->setDebugName("App");
    root_ = root;
    ownedRoot_ = root;

    homeBackground_ = Background::create({.grid = true});
    browseBackground_ = Background::create();
    root->appendChild(homeBackground_);
    root->appendChild(browseBackground_);

    content_ = Box::create(fill());
    root->appendChild(content_);

    auto overlayLayer = Box::create(fill());
    overlayLayer->setHitTestBehavior(HitTestBehavior::deferToChild);
    auto snackbars = SnackbarHost::create(&services_.loop);
    overlays_ = std::make_unique<Overlays>(*overlayLayer, *snackbars, &services_.loop);

    if (options_.actions) {
        actions_ = options_.actions(services_, *state_, *overlays_);
    }
    if (!actions_) {
        actions_ = std::make_unique<FileOperations>(services_, *state_, *overlays_);
    }
    context_ = std::unique_ptr<AppContext>(new AppContext{
            .services = services_,
            .state = *state_,
            .actions = *actions_,
            .screens = registry_,
            .overlays = *overlays_,
            .thumbnails = *thumbnails_,
            .collator = collator_.get(),
    });

    actions_->attach(*context_);
    {
        std::weak_ptr<App> weak = weak_from_this();
        context_->lifetime = weak;
        context_->openText = [weak](const FileTargets &file) {
            if (auto self = weak.lock()) {
                self->openText(file);
            }
        };
        context_->openViewer = [weak](std::vector<MediaItem> items, std::size_t index) {
            if (auto self = weak.lock()) {
                self->openViewer(std::move(items), index);
            }
        };
        context_->showOnboarding = [weak] {
            if (auto self = weak.lock()) {
                self->showOnboarding();
            }
        };
        context_->reveal = [weak](const vfs::Location &file) {
            if (auto self = weak.lock()) {
                self->closeViewer();
                self->browse_->reveal(file);
            }
        };
    }

    registry_.add(settingsScreenSpec());
    registry_.add(hiddenItemsScreenSpec());
    for (auto &spec : sftpScreenSpecs()) {
        registry_.add(std::move(spec));
    }
    for (auto &spec : aboutScreenSpecs()) {
        registry_.add(std::move(spec));
    }
    for (auto &spec : options_.screens) {
        registry_.add(std::move(spec));
    }
    options_.screens.clear();

    App *self = this;
    serverSync_ = std::make_unique<ServerSync>(services_.sftp);
    prompts_ = PromptSheets::create(*context_, [self](const std::string &id) { return self->serverName(id); });
    {
        // Comes on an I/O thread.
        std::weak_ptr<App> weak = weak_from_this();
        services_.sftp.setOnPoolLimited([weak, &loop = services_.loop](const vfs::PoolLimit &limit) {
            loop.post([weak, limit] {
                if (auto self = weak.lock()) {
                    self->poolLimited(limit);
                }
            });
        });
    }
    if (services_.prompts != nullptr) {
        services_.prompts->setPasswordProvider(prompts_);
        services_.prompts->setSftpPrompts(prompts_);
    }
    root->build([&] {
        home_ = HomeScreen::create(*context_);
        browse_ = BrowseScreen::create(*context_);
        screenSlot_ = Box::create(fill());
        content_->appendChild(home_);
        content_->appendChild(browse_);
        content_->appendChild(screenSlot_);

        // --- The top bar: the path on one line, or the filter while searching ---
        auto bar = Box::create({.direction = Direction::row, .alignItems = Align::center, .padding = {16, 0, 4, 0}});
        homeButton_ = IconButton::create({.icon = "home",
                                          .label = "Go home",
                                          .style = IconButtonStyle::plain,
                                          .size = 40,
                                          .onTap = [self] { openHome(*self->context_); }});
        homeButton_->updateStyle([](Style &s) { s.margin = {-12, 0, 0, 0}; });
        bar->appendChild(homeButton_);
        remoteGlyph_ = Icon::create("lan", 18);
        remoteGlyph_->updateStyle([](Style &s) {
            s.display = false;
            s.margin = {0, 0, 8, 0};
        });
        remoteGlyph_->bind([g = remoteGlyph_.get()] {
            const Theme &th = theme();
            untrack([&] { g->setColor(ink(th, 0.7f)); });
        });
        bar->appendChild(remoteGlyph_);
        breadcrumb_ = Breadcrumb::create({
                .segments =
                        [self] {
                            const Route route = self->state_->active()->trackCurrent();
                            return pathLabels(route, *self->context_, self->context_->volumes());
                        },
                .onSelect =
                        [self](std::size_t index) {
                            const Route route = self->state_->peekActive()->current();
                            if (index == 0) {
                                if (route.kind != Route::Kind::home) {
                                    self->state_->navigateHome();
                                }
                                return;
                            }
                            if (route.kind == Route::Kind::browse && route.location) {
                                const auto crumbs = crumbsFor(*route.location, self->context_->volumes.peek());
                                if (index + 1 < crumbs.size() && crumbs[index].target) {
                                    self->state_->navigate(*crumbs[index].target);
                                }
                            }
                        },
        });
        breadcrumb_->updateStyle([](Style &s) {
            s.grow = 1;
            s.shrink = 1;
            s.basis = 0;
        });
        bar->appendChild(breadcrumb_);

        // The in-folder filter replaces the path while searching.
        closeSearch_ = IconButton::create({.icon = "arrow_back",
                                           .label = "Close search",
                                           .style = IconButtonStyle::plain,
                                           .size = 40,
                                           .onTap = [self] { self->state_->peekActive()->setSearching(false); }});
        closeSearch_->updateStyle([](Style &s) {
            s.display = false;
            s.margin = {-12, 0, 0, 0};
        });
        bar->appendChild(closeSearch_);
        filterSlot_ = Box::create({.display = false, .direction = Direction::row, .alignItems = Align::center, .gap = 8,
                                   .padding = {12, 0, 12, 0}, .margin = {4, 0, 4, 0}, .grow = 1, .shrink = 1, .basis = 0,
                                   .height = 36},
                                  {.radii = gfx::CornerRadii::uniform(kCorner), .borderWidth = 1});
        auto filterIcon = Icon::create("filter_list", 18);
        filterSlot_->appendChild(filterIcon);
        filterSlot_->bind([slot = static_cast<Box *>(filterSlot_.get()), icon = filterIcon.get()] {
            const Theme &th = theme();
            untrack([&] {
                const float well = th.colors.light ? th.colors.wellAlpha : 0.28f;
                slot->updateDecoration([&](BoxDecoration &d) {
                    d.background = gfx::Paint{gfx::Color{0, 0, 0, well}};
                    d.border = th.colors.film.withAlpha(0.10f);
                });
                icon->setColor(ink(th, 0.7f));
            });
        });
        filterField_ = EditableText::create({
                .placeholder = "Filter this folder",
                .config = {.action = ImeAction::search},
                .onChange = [self](const std::string &text) { self->state_->peekActive()->filter.set(text); },
                .onSubmit = [self] { self->services_.root.requestFocus(nullptr); },
        });
        filterField_->updateStyle([](Style &s) {
            s.grow = 1;
            s.shrink = 1;
            s.basis = 0;
        });
        filterField_->updateSemantics([](Semantics &s) { s.label = "Filter this folder"; });
        filterSlot_->appendChild(filterField_);
        bar->appendChild(filterSlot_);

        overflow_ = IconButton::create({.icon = "more_vert",
                                        .label = "More options",
                                        .style = IconButtonStyle::plain,
                                        .size = 40,
                                        .onTap = [self] { self->showOverflow(); }});
        bar->appendChild(overflow_);

        Signal<float> barScroll = context_->barScroll;
        topBar_ = TopBar::create({.content = bar, .scrollOffset = barScroll});
        root->appendChild(topBar_);
        // The viewer covers the screens and the top bar; overlays still go over it.
        viewerSlot_ = Box::create(fill());
        viewerSlot_->setHitTestBehavior(HitTestBehavior::deferToChild);
        root->appendChild(viewerSlot_);
        // The intro covers even the viewer.
        onboardingSlot_ = Box::create(fill());
        onboardingSlot_->setHitTestBehavior(HitTestBehavior::deferToChild);
        root->appendChild(onboardingSlot_);
        root->appendChild(overlayLayer);
        root->appendChild(snackbars);

        // --- Bindings ---
        root->bind([self] {
            (void)environment().density();
            (void)environment().safeArea();
            (void)theme();
            untrack([&] { self->context_->barHeight.set(self->topBar_->height()); });
        });
        // The theme follows the settings, and for Clean the system's dark mode.
        root->bind([self] {
            const Settings &s = self->state_->settings();
            const ThemePreset preset = s.preset;
            const std::optional<gfx::Color> accent = s.accent;
            const bool night = environment().night();
            untrack([&] { environment().theme.set(makeTheme(preset, accent, night)); });
        });
        // The system bars' icons follow the ground.
        root->bind([self] {
            const bool light = theme().colors.light;
            untrack([&] { self->services_.platform.setLightSystemBars(light); });
        });
        // Saved servers go to the SFTP backend.
        root->bind([self] {
            const auto servers = self->state_->settings().servers;
            untrack([&] { self->serverSync_->apply(servers); });
        });
        // And saved document trees to the document backend.
        root->bind([self] {
            const auto trees = self->state_->settings().trees;
            untrack([&] {
                if (self->services_.documentTrees != nullptr) {
                    std::map<std::string, std::string> uris;
                    for (const DocumentTree &tree : trees) {
                        uris.emplace(tree.id, tree.uri);
                    }
                    self->services_.documentTrees->setTrees(std::move(uris));
                }
            });
        });
        // Thumbnails on or off: the rows bound now pick it up.
        root->bind([self, first = std::make_shared<bool>(true)] {
            const bool on = self->state_->settings().thumbnails;
            untrack([&] {
                if (self->thumbnails_->enabled() == on) {
                    return;
                }
                self->thumbnails_->setEnabled(on);
                if (!*first || !on) {
                    self->browse_->refresh();
                }
            });
            *first = false;
        });
        // Mobile data mode.
        root->bind([self] {
            const bool saving = self->state_->settings().dataSaver && self->services_.metered();
            untrack([&] { self->context_->dataSaving.set(saving); });
        });
        // Remote thumbnails on or off (the setting, or mobile data mode on a metered network), and
        // the largest file fetched for one.
        root->bind([self, setting = std::make_shared<bool>(self->state_->settings.peek().remoteThumbnails)] {
            const Settings &s = self->state_->settings();
            const bool on = s.remoteThumbnails && !self->context_->dataSaving();
            const std::uint64_t maxBytes = remoteThumbnailMaxBytes(s);
            const bool settingChanged = *setting != s.remoteThumbnails;
            *setting = s.remoteThumbnails;
            untrack([&] {
                const bool changed = self->thumbnails_->remoteEnabled() != on;
                self->thumbnails_->setRemote(on, maxBytes);
                if (!changed) {
                    return;
                }
                // The network changing shouldn't list the folder again; the rows ask again.
                if (settingChanged) {
                    self->browse_->refresh();
                } else {
                    self->browse_->rebindVisible();
                }
            });
        });
        // How many connections per server the viewer's prefetching may use.
        root->bind([self] {
            const std::uint32_t connections = self->state_->settings().remoteConnections;
            untrack([&] { self->services_.sftp.setParallelConnections(connections); });
        });
        // Remote thumbnails go once no tab shows their remote.
        root->bind([self] {
            std::unordered_set<std::string> open;
            for (const auto &tab : self->state_->tabs()) {
                const Route route = tab->trackCurrent();
                if (route.location && !route.location->isLocal()) {
                    open.insert(ThumbnailCache::remoteGroup(*route.location));
                }
            }
            untrack([&] { self->thumbnails_->keepRemoteGroups(open); });
        });
        // Their temporary files go when the app goes to the background.
        root->bind([self] {
            const bool resumed = self->services_.resumed();
            untrack([&] {
                if (!resumed) {
                    self->thumbnails_->dropRemoteTemp();
                }
            });
        });
        // The thumbnail disk cache's size and decay: a change sweeps it.
        root->bind([self] {
            const ThumbnailStoreLimits limits = thumbnailLimits(self->state_->settings());
            untrack([&] { self->thumbnails_->setDiskLimits(limits); });
        });
        // Volumes: on start, on resume, and when access changes.
        root->bind([self] {
            (void)self->services_.allFilesAccess();
            (void)self->services_.resumed();
            untrack([&] { self->refreshVolumes(); });
        });
        // Which local favorites are gone: on start, on resume, and when they change.
        root->bind([self] {
            const std::vector<Favorite> favorites = self->state_->settings().favorites;
            (void)self->services_.resumed();
            untrack([&] { self->checkFavorites(favorites); });
        });
        // The active tab's route picks the screen.
        root->bind([self] {
            const Route route = self->state_->active()->trackCurrent();
            untrack([&] { self->showRoute(route); });
        });
        // The top bar's row 2 follows the active screen's scrolling.
        root->bind([self] {
            const Route route = self->state_->active()->trackCurrent();
            float offset = 0;
            if (route.kind == Route::Kind::home) {
                offset = self->home_->scroller().offsetSignal()();
            } else if (route.kind == Route::Kind::browse) {
                offset = self->browse_->list().offsetSignal()();
            }
            untrack([&] { self->context_->barScroll.set(offset); });
        });
        // Searching swaps the path for the filter field; a server's folders get its glyph.
        root->bind([self] {
            auto tab = self->state_->active();
            const bool searching = tab->searching();
            const Route route = tab->trackCurrent();
            untrack([&] {
                const bool show = searching && route.kind == Route::Kind::browse;
                const bool remote = route.kind == Route::Kind::browse && route.location &&
                                    route.location->kind() == vfs::BackendKind::kSftp;
                self->remoteGlyph_->updateStyle([&](Style &s) { s.display = remote && !show; });
                self->homeButton_->updateStyle([&](Style &s) { s.display = !show; });
                self->breadcrumb_->updateStyle([&](Style &s) { s.display = !show; });
                self->closeSearch_->updateStyle([&](Style &s) { s.display = show; });
                self->filterSlot_->updateStyle([&](Style &s) { s.display = show; });
                if (show) {
                    self->topBar_->show(true);
                    if (self->filterField_->editingValue().text != tab->filter.peek()) {
                        self->filterField_->setText(tab->filter.peek());
                    }
                    self->filterField_->requestFocus();
                } else if (self->filterField_->focused()) {
                    self->filterField_->clearFocus();
                    self->filterField_->setText({});
                }
            });
        });
        // While an overlay is open, what's behind it leaves semantics.
        root->bind([self] {
            const bool overlaid = self->overlays_->openCount() > 0;
            const bool introduced = self->onboardingOpen_();
            const bool covered = overlaid || introduced || self->viewerOpen_();
            untrack([&] {
                for (Node *node : {self->content_.get(), static_cast<Node *>(self->topBar_.get())}) {
                    node->updateSemantics([&](Semantics &s) { s.hidden = covered; });
                }
                self->viewerSlot_->updateSemantics([&](Semantics &s) { s.hidden = overlaid || introduced; });
                self->onboardingSlot_->updateSemantics([&](Semantics &s) { s.hidden = overlaid; });
            });
        });
        // Settings are saved shortly after they change.
        root->bind([self, first = std::make_shared<bool>(true)] {
            Settings snapshot = self->state_->snapshot();
            untrack([&] {
                if (*first) {
                    *first = false;
                    return;
                }
                self->scheduleSave(std::move(snapshot));
            });
        });
        if (plaintextSecrets_) {
            // An older file kept passwords as entered: replace it now, sealed.
            scheduleSave(state_->snapshot());
            flushSettings();
        }
        std::weak_ptr<App> weak = weak_from_this();
        services_.root.addBackHandler([weak] {
            auto app = weak.lock();
            return app && app->back();
        });
        if (options_.onboarding && state_->settings.peek().onboarded < kOnboardingVersion) {
            showOnboarding();
        }
    });
}

void App::refreshVolumes() {
    std::vector<runtime::StorageVolume> volumes = services_.platform.storageVolumes();
    if (volumes.empty() && !services_.homePath.empty()) {
        // A host or an odd device: the home directory stands in for the primary volume.
        volumes.push_back({.path = services_.homePath, .description = "Internal", .primary = true, .state = "mounted"});
    }
    context_->volumes.set(std::move(volumes));
}

void App::checkFavorites(std::vector<Favorite> favorites) {
    std::vector<vfs::Location> local;
    for (const Favorite &favorite : favorites) {
        // Only local ones: checking a server's would connect to it.
        if (favorite.location.isLocal()) {
            local.push_back(favorite.location);
        }
    }
    std::weak_ptr<App> weak = weak_from_this();
    vfs::BackendResolver *backends = &services_.backends;
    services_.io.submit(
            [backends, local] {
                std::vector<vfs::Location> missing;
                for (const vfs::Location &location : local) {
                    vfs::Backend *backend = backends->backendFor(location);
                    const auto stat = backend != nullptr ? backend->lstat(location) : Result<vfs::Entry>(fail(EIO, ""));
                    if (!stat && (stat.error().code == ENOENT || stat.error().code == ENOTDIR)) {
                        missing.push_back(location);
                    }
                }
                return missing;
            },
            [weak](std::vector<vfs::Location> missing) {
                auto self = weak.lock();
                if (self && missing != self->context_->missingFavorites.peek()) {
                    self->context_->missingFavorites.set(std::move(missing));
                }
            });
}

void App::showRoute(const Route &route) {
    const bool home = route.kind == Route::Kind::home;
    const bool browse = route.kind == Route::Kind::browse;
    const bool screen = route.kind == Route::Kind::screen;
    home_->updateStyle([&](Style &s) { s.display = home; });
    browse_->updateStyle([&](Style &s) { s.display = browse; });
    homeBackground_->updateStyle([&](Style &s) { s.display = home; });
    browseBackground_->updateStyle([&](Style &s) { s.display = !home; });
    screenSlot_->updateStyle([&](Style &s) { s.display = screen; });
    if (!screen) {
        if (screen_) {
            screenSlot_->clearChildren();
            screen_.reset();
            screenId_.clear();
        }
        return;
    }
    if (screen_ && screenId_ == route.screenId) {
        return;
    }
    screenSlot_->clearChildren();
    screen_.reset();
    screenId_ = route.screenId;
    if (const ScreenSpec *spec = registry_.find(route.screenId); spec != nullptr && spec->build &&
        (!spec->enabled || spec->enabled(state_->settings.peek()))) {
        screen_ = spec->build(*context_);
        if (screen_) {
            screenSlot_->appendChild(screen_);
        }
    }
}

void App::openViewer(std::vector<MediaItem> items, std::size_t index) {
    if (items.empty()) {
        return;
    }
    closeViewer();
    std::weak_ptr<App> weak = weak_from_this();
    viewer_ = ViewerScreen::create(*context_, std::move(items), index, options_.viewer, [weak] {
        if (auto self = weak.lock()) {
            self->closeViewer();
        }
    });
    viewerSlot_->appendChild(viewer_);
    viewerOpen_.set(true);
}

std::string App::serverName(const std::string &id) const {
    for (const auto &server : state_->settings.peek().servers) {
        if (server.id == id) {
            return server.name.empty() ? server.server.host : server.name;
        }
    }
    // A connection test: "test-connection:<n>:<name>".
    const std::size_t colon = id.rfind(':');
    return colon == std::string::npos || colon + 1 == id.size() ? id : id.substr(colon + 1);
}

void App::poolLimited(const vfs::PoolLimit &limit) {
    if (viewer_) {
        viewer_->poolLimited(limit, serverName(limit.serverId));
    }
}

void App::showOnboarding() {
    if (onboarding_) {
        return;
    }
    std::weak_ptr<App> weak = weak_from_this();
    onboarding_ = OnboardingScreen::create(*context_, [weak](OnboardingScreen::Exit exit) {
        if (auto self = weak.lock()) {
            self->closeOnboarding(exit == OnboardingScreen::Exit::addServer);
        }
    });
    onboardingSlot_->appendChild(onboarding_);
    onboardingOpen_.set(true);
}

void App::closeOnboarding(bool addServer) {
    if (!onboarding_) {
        return;
    }
    state_->updateSettings([](Settings &s) { s.onboarded = kOnboardingVersion; });
    // From inside its own buttons: released on the next turn.
    auto keep = std::move(onboarding_);
    onboardingSlot_->removeChild(keep.get());
    onboardingOpen_.set(false);
    services_.loop.post([keep = std::move(keep)] {});
    if (addServer) {
        state_->openScreen(kServersScreen);
        state_->openScreen(kNewServerScreen);
    }
}

void App::closeViewer() {
    if (!viewer_) {
        return;
    }
    // Usually from inside the viewer's own back button: released on the next turn.
    auto keep = std::move(viewer_);
    viewerSlot_->removeChild(keep.get());
    viewerOpen_.set(text_ != nullptr);
    services_.loop.post([keep = std::move(keep)] {});
}

void App::openText(const FileTargets &file) {
    if (file.entries.size() != 1) {
        return;
    }
    closeText();
    std::weak_ptr<App> weak = weak_from_this();
    text_ = TextScreen::create(*context_, file, [weak] {
        if (auto self = weak.lock()) {
            self->closeText();
        }
    });
    viewerSlot_->appendChild(text_);
    viewerOpen_.set(true);
}

void App::closeText() {
    if (!text_) {
        return;
    }
    // From inside its own back button or dialog: released on the next turn.
    auto keep = std::move(text_);
    viewerSlot_->removeChild(keep.get());
    viewerOpen_.set(viewer_ != nullptr);
    services_.loop.post([keep = std::move(keep)] {});
}

bool App::back() {
    if (overlays_->back()) {
        return true;
    }
    if (onboarding_) {
        return onboarding_->back();
    }
    if (viewer_) {
        viewer_->close();
        return true;
    }
    if (text_) {
        text_->back();
        return true;
    }
    if (screen_) {
        if (const ScreenSpec *spec = registry_.find(screenId_); spec != nullptr && spec->back && spec->back(*screen_)) {
            return true;
        }
    }
    return state_->back();
}

void App::scheduleSave(Settings settings) {
    pendingSave_ = serializeSettings(settings, secrets_.get());
    pendingGeneration_ = store_->nextGeneration();
    if (saveTimer_) {
        return;
    }
    std::weak_ptr<App> weak = weak_from_this();
    saveTimer_ = services_.loop.postDelayed(
            [weak] {
                auto self = weak.lock();
                if (!self || !self->pendingSave_) {
                    return;
                }
                self->saveTimer_.reset();
                std::shared_ptr<SettingsStore> store = self->store_;
                // Written on the pool; the store orders concurrent saves by generation.
                self->services_.io.execute([store, text = std::move(*self->pendingSave_),
                                            generation = self->pendingGeneration_] {
                    if (auto saved = store->save(text, generation); !saved) {
                        FILEPAN_LOGW("couldn't save settings: %s", saved.error().message.c_str());
                    }
                });
                self->pendingSave_.reset();
            },
            options_.saveDelayMs * 1'000'000);
}

void App::flushSettings() {
    if (!pendingSave_) {
        return;
    }
    if (auto saved = store_->save(*pendingSave_, pendingGeneration_); !saved) {
        FILEPAN_LOGW("couldn't save settings: %s", saved.error().message.c_str());
    }
    pendingSave_.reset();
}

void App::showOverflow() {
    std::vector<MenuItem> items;
    App *self = this;
    const Route route = state_->peekActive()->current();
    const bool browsing = route.kind == Route::Kind::browse && route.location;
    items.push_back({.icon = "tab",
                     .label = "Tabs",
                     .onSelect = [self] { self->showTabs(); },
                     .trailing = std::to_string(state_->tabs.peek().size())});
    if (browsing) {
        const vfs::Location here = *route.location;
        const bool hidden = state_->settings.peek().showHidden;
        items.push_back({hidden ? "visibility_off" : "visibility", hidden ? "Hide hidden files" : "Show hidden files",
                         [self, hidden] {
                             self->state_->updateSettings([&](Settings &s) { s.showHidden = !hidden; });
                         }});
        const bool favorite = untrack([&] { return state_->isFavorite(here); });
        items.push_back({.icon = "star",
                         .label = favorite ? "Remove from favorites" : "Add to favorites",
                         .onSelect = [self, here] { toggleFavoriteFolder(*self->context_, here); },
                         .filled = favorite});
        items.push_back({"tab", "Open in new tab", [self, here] { self->state_->addTab(here); }});
        items.push_back({"refresh", "Refresh", [self] { self->browse_->refresh(RefreshFeedback::shown); }});
    }
    for (const ScreenSpec &spec : registry_.all()) {
        if (spec.inMenu && !(route.kind == Route::Kind::screen && route.screenId == spec.id) &&
            (!spec.enabled || spec.enabled(state_->settings.peek()))) {
            AppState *state = state_.get();
            items.push_back({spec.icon, spec.title, [state, id = spec.id] { state->openScreen(id); }});
        }
    }
    if (browsing) {
        overlays_->showMenu(std::move(items), *overflow_, browse_->sortControls(), BrowseScreen::kSortControlsHeight,
                            264);
    } else {
        overlays_->showMenu(std::move(items), *overflow_);
    }
}

void App::showTabs() {
    App *self = this;
    auto content = Box::create({.gap = 4, .padding = {16, 0, 16, 8}});
    auto rows = Box::create({.gap = 2});
    content->appendChild(rows);
    // Rebuilt whenever the tabs, the active one, or a tab's folder change.
    rows->bind([self, r = rows.get()] {
        struct Line {
            std::uint64_t id;
            std::string name;
            std::string path;
            bool remote;
        };
        std::vector<Line> lines;
        const auto &volumes = self->context_->volumes();
        for (const auto &tab : self->state_->tabs()) {
            const Route route = tab->trackCurrent();
            std::string path;
            for (const std::string &label : pathLabels(route, *self->context_, volumes)) {
                path += (path.empty() ? "" : " / ") + label;
            }
            lines.push_back({tab->id, routeLabel(route, *self->context_), path,
                             route.kind == Route::Kind::browse && route.location &&
                                     route.location->kind() == vfs::BackendKind::kSftp});
        }
        const std::uint64_t active = self->state_->activeId();
        untrack([&] {
            r->clearChildren();
            const Theme &t = currentTheme();
            for (const Line &line : lines) {
                const bool on = line.id == active;
                auto row = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 12,
                                        .padding = {12, 0, 4, 0}, .height = 56});
                row->setDebugName("TabRow");
                auto glyph = Icon::create(line.remote ? "lan" : "folder_open", 22, ink(t, t.text.body));
                row->appendChild(glyph);
                auto texts = Box::create({.gap = 2, .grow = 1, .shrink = 1, .basis = 0});
                auto name = Text::create(line.name, t.type.body, ink(t, t.text.primary));
                name->setMaxLines(1);
                name->setOverflow(gfx::TextOverflow::ellipsisMiddle);
                auto path = Text::create(line.path, monoText(t, 11, 500), ink(t, t.text.meta));
                path->setMaxLines(1);
                path->setOverflow(gfx::TextOverflow::ellipsisMiddle);
                for (Node *n : {static_cast<Node *>(name.get()), static_cast<Node *>(path.get())}) {
                    n->updateSemantics([](Semantics &s) { s.hidden = true; });
                }
                texts->appendChild(name);
                texts->appendChild(path);
                row->appendChild(texts);
                const std::uint64_t id = line.id;
                row->appendChild(IconButton::create({.icon = "close",
                                                     .label = "Close tab " + line.name,
                                                     .style = IconButtonStyle::plain,
                                                     .size = 44,
                                                     .onTap = [self, id] { self->state_->closeTab(id); }}));
                auto pressable = makePressable(*row, {.onTap =
                                                              [self, id] {
                                                                  self->state_->selectTab(id);
                                                                  if (BottomSheet *sheet = self->overlays_->topSheet()) {
                                                                      sheet->dismiss();
                                                                  }
                                                              },
                                                      .onLongPress =
                                                              [self, id, raw = row.get()] { self->showTabMenu(id, *raw); },
                                                      .haptic = Haptic::tick});
                row->setPaintHook([pressable, on](PaintContext &context) {
                    const Theme &th = currentTheme();
                    const gfx::RoundedRect shape{context.bounds(), gfx::CornerRadii::uniform(context.px(th.corner(16)))};
                    if (on) {
                        context.list.fill(shape, th.colors.accent.withAlpha(th.colors.selected));
                    }
                    if (pressable->pressed.peek()) {
                        context.list.fill(shape, th.colors.ink.withAlpha(th.colors.pressed * 0.6f));
                    }
                });
                row->bind([raw = row.get(), pressable] {
                    (void)pressable->pressed();
                    untrack([&] { raw->markPaintDirty(); });
                });
                row->updateSemantics([&](Semantics &s) {
                    s.role = Role::tab;
                    s.label = line.name + ", " + line.path;
                    s.flags = on ? semantics_flags::kSelected : 0;
                });
                r->appendChild(row);
            }
        });
    });
    rows->updateSemantics([](Semantics &s) { s.role = Role::tabList; });
    auto add = Button::create({.label = "New tab",
                               .variant = ButtonVariant::glass,
                               .icon = "add",
                               .onTap = [self] {
                                   const Route &route = self->state_->peekActive()->current();
                                   self->state_->addTab(route.kind == Route::Kind::browse ? route.location : std::nullopt);
                                   if (BottomSheet *sheet = self->overlays_->topSheet()) {
                                       sheet->dismiss();
                                   }
                               }});
    add->updateStyle([](Style &s) { s.margin = {0, 10, 0, 0}; });
    content->appendChild(add);
    overlays_->showSheet("Tabs", content);
}

void App::showTabMenu(std::uint64_t id, const Node &anchor) {
    AppState *state = state_.get();
    std::vector<MenuItem> items{
            {"close", "Close tab", [state, id] { state->closeTab(id); }},
    };
    if (state_->tabs.peek().size() > 1) {
        items.push_back({"tab_close_right", "Close other tabs", [state, id] { state->closeOtherTabs(id); }});
    }
    if (auto tab = state_->tab(id); tab && tab->current().kind == Route::Kind::browse) {
        const auto location = tab->current().location;
        items.push_back({"tab_duplicate", "Duplicate tab", [state, location] { state->addTab(location); }});
    }
    overlays_->showMenu(std::move(items), anchor);
}

NodePtr createApp(runtime::AppServices &services) {
    AppOptions options{.onboarding = true};
#if FILEPAN_ADIRSTAT
    options.screens = diskUsageScreenSpecs();
#endif
    return App::create(services, std::move(options))->root();
}

}  // namespace filepan::app
