// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/browse_screen.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <functional>
#include <unordered_set>

#include "filepan/app/favorites_sheet.hpp"
#include "filepan/app/format.hpp"
#include "filepan/app/places.hpp"
#include "filepan/app/text_screen.hpp"
#include "filepan/app/viewer_screen.hpp"
#include "filepan/ui/components/recipes.hpp"
#include "filepan/vfs/mime.hpp"

namespace filepan::app {

namespace {

using namespace ui;
using namespace ui::components;

Style fill() {
    Style s;
    s.position = Position::absolute;
    s.left = 0;
    s.top = 0;
    s.right = 0;
    s.bottom = 0;
    return s;
}

constexpr float kPi = 3.14159265f;

gfx::Transform rotation(float angle) {
    return {std::cos(angle), std::sin(angle), -std::sin(angle), std::cos(angle), 0, 0};
}

std::uint64_t mix64(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

// The dock and band clear the list's end by this much above the navigation bar.
float bottomClearance() {
    const Theme &t = currentTheme();
    return t.spacing.dockInset + t.spacing.dock + (t.style.utilitarian ? t.spacing.statusLine + 8 : 24);
}

// The status line: "12 items · 2.1 GiB · 21 GiB free", or "3 selected · 12 MiB".
std::string statusLine(const std::vector<vfs::Entry> &entries, const std::vector<std::string> &selection,
                       const runtime::StorageVolume *volume) {
    std::uint64_t bytes = 0;
    bool files = false;
    for (const vfs::Entry &entry : entries) {
        if (!entry.isDirectoryLike() && entry.hasStat &&
            (selection.empty() || std::ranges::binary_search(selection, entry.name))) {
            bytes += entry.size;
            files = true;
        }
    }
    std::string text = selection.empty() ? formatCount(entries.size()) : std::to_string(selection.size()) + " selected";
    if (files) {
        text += " · " + formatSize(bytes);
    }
    if (selection.empty() && volume != nullptr && volume->totalBytes > 0) {
        text += " · " + formatSize(volume->freeBytes) + " free";
    }
    return text;
}

std::string errorMessage(const Error &error) {
    switch (error.code) {
    case EACCES:
    case EPERM:
        return "FilePan isn't allowed to read this folder.";
    case ENOENT:
        return error.message.empty() ? "This folder doesn't exist." : error.message + ".";
    case ENOTDIR:
        return "This isn't a folder.";
    case EROFS:
        return "This location is read-only.";
    default:
        break;
    }
    return std::string(std::strerror(error.code)) + ".";
}

// A row of three tiles: the grid view's list item.
class TileRow final : public Box {
public:
    std::shared_ptr<GridTile> tiles[BrowseScreen::kGridColumns];
};

}  // namespace

std::shared_ptr<BrowseScreen> BrowseScreen::create(AppContext &context) {
    auto node = std::make_shared<BrowseScreen>(context);
    node->construct();
    return node;
}

BrowseScreen::BrowseScreen(AppContext &context) : context_(context), loop_(context.services.loop) {
    setDebugName("BrowseScreen");
    model_ = std::make_shared<DirectoryModel>(context.services.loop, context.services.io, context.services.backends,
                                              context.collator);
}

BrowseScreen::~BrowseScreen() {
    setSpinning(false);
    if (refreshTimer_) {
        loop_.cancel(*refreshTimer_);
    }
    model_->stop();
}

std::shared_ptr<TabModel> BrowseScreen::tab() const { return context_.state.peekActive(); }

void BrowseScreen::construct() {
    setStyle(fill());
    const Theme &t = currentTheme();

    list_ = VirtualList::create();
    list_->setStyle(fill());
    list_->setEstimatedItemExtent(t.spacing.listRow);
    list_->updateSemantics([](Semantics &s) { s.role = Role::list; });
    list_->setItemType([this](std::size_t i) { return static_cast<std::uint32_t>(items_[i].type); });
    list_->setCreate([this](std::uint32_t type) -> NodePtr {
        if (type == Item::header) {
            return Box::create({.minHeight = 40});
        }
        const auto callbacks = [this](std::function<const Node *()> self) {
            return FileRowCallbacks{
                    .onTap =
                            [this, self] {
                                if (auto found = boundEntry_.find(self()); found != boundEntry_.end()) {
                                    activate(found->second);
                                }
                            },
                    .onLongPress =
                            [this, self] {
                                if (auto found = boundEntry_.find(self()); found != boundEntry_.end()) {
                                    longPress(found->second);
                                }
                            },
            };
        };
        if (type == Item::row) {
            auto holder = std::make_shared<const Node *>(nullptr);
            auto row = FileRow::create(callbacks([holder] { return *holder; }));
            *holder = row.get();
            return row;
        }
        auto tiles = std::make_shared<TileRow>();
        tiles->setStyle({.direction = Direction::row, .alignItems = Align::start, .padding = Edges::symmetric(10, 0)});
        for (auto &tile : tiles->tiles) {
            auto holder = std::make_shared<const Node *>(nullptr);
            tile = GridTile::create(callbacks([holder] { return *holder; }));
            *holder = tile.get();
            tile->updateStyle([](Style &s) {
                s.grow = 1;
                s.shrink = 1;
                s.basis = 0;
            });
            tiles->appendChild(tile);
        }
        return tiles;
    });
    list_->setBind([this](std::size_t index, Node &node) { bindItem(index, node); });
    list_->setKeys([this](std::size_t i) { return keyOf(i); },
            [this](std::uint64_t key) -> std::optional<std::size_t> {
                const auto found = itemOfKey_.find(key);
                return found == itemOfKey_.end() ? std::nullopt : std::optional(found->second);
            });
    list_->setPointerHandler([this](const PointerEvent &event) { onPointer(event); });
    // Pull to refresh: at the top, a downward drag pulls the indicator instead of stretching.
    list_->setPostScroll([this](float delta) -> float {
        if (!pointer_ || delta >= 0 || dragAnchor_ || feedback_) {
            return 0;
        }
        setPull(pull_.peek() - delta * 0.5f);
        return delta;
    });
    list_->setPreScroll([this](float delta) -> float {
        if (delta <= 0 || pull_.peek() <= 0 || !pointer_ || feedback_) {
            return 0;
        }
        const float consumed = std::min(delta, pull_.peek() * 2);
        setPull(std::max(0.0f, pull_.peek() - consumed * 0.5f));
        return consumed;
    });
    appendChild(list_);

    emptySlot_ = Box::create([] {
        Style s = fill();
        s.justify = Justify::center;
        s.alignItems = Align::center;
        return s;
    }());
    emptySlot_->setHitTestBehavior(HitTestBehavior::deferToChild);
    appendChild(emptySlot_);

    // The pull indicator: a panel with an arrow that turns as it's pulled, takes the accent once
    // releasing would refresh, and spins while the folder lists again.
    auto indicator = Glass::create({.justify = Justify::center, .alignItems = Align::center, .width = 40, .height = 40,
                                    .position = Position::absolute, .left = 0, .top = 0},
                                   {.radii = gfx::CornerRadii::uniform(20)});
    pullIcon_ = Icon::create("refresh", 22, ink(t, 0.7f));
    indicator->appendChild(pullIcon_);
    indicator->setHitTestBehavior(HitTestBehavior::none);
    indicator->updateSemantics([](Semantics &s) { s.hidden = true; });
    pullIndicator_ = indicator;
    appendChild(indicator);

    auto fade = BottomFade(136, 96);
    fade->bind([f = fade.get()] {
        const bool show = !theme().style.utilitarian;
        untrack([&] { f->updateStyle([&](Style &s) { s.display = show; }); });
    });
    appendChild(fade);

    // The utilitarian status line, right over the toolbar.
    statusLine_ = Box::create({.display = false, .justify = Justify::center, .padding = Edges::symmetric(12, 0),
                               .position = Position::absolute, .left = -1, .right = -1});
    statusLine_->setPaintHook([](PaintContext &context) {
        const Theme &th = currentTheme();
        recipes::glass(context.list, {context.bounds(), {}}, th, context.density);
    });
    statusText_ = Text::create({}, gfx::TextStyle{.family = gfx::FontFamily::mono, .size = 11.5f, .weight = 450,
                                                  .lineHeight = 1.3f});
    statusText_->setMaxLines(1);
    statusText_->setOverflow(gfx::TextOverflow::ellipsisEnd);
    statusLine_->appendChild(statusText_);
    statusLine_->updateSemantics([](Semantics &s) { s.flags |= semantics_flags::kLiveRegion; });
    appendChild(statusLine_);

    dock_ = BottomDock::create(
            {.idle = {{"add", "New", [this] { showNewMenu(*dock_); }},
                      {"search", "Filter this folder", [this] { tab()->setSearching(!tab()->searching.peek()); }},
                      // Filled while the folder shown is a favorite.
                      {"star", "Favorites", [this] { showFavorites(context_); }, false,
                       [this] {
                           const Route route = context_.state.active()->trackCurrent();
                           return route.location.has_value() && context_.state.isFavorite(*route.location);
                       }},
                      {"checklist", "Select all", [this] { selectAll(); }}},
             .selection = {{"attach_file", "Attach", [this] { context_.actions.pick(selectionTargets()); }, false, false,
                            [this] { return context_.actions.pickRequest().has_value(); }},
                           {"content_copy", "Copy", [this] { context_.actions.copy(selectionTargets()); }},
                           {"drive_file_move", "Move", [this] { context_.actions.cut(selectionTargets()); }},
                           {"delete", "Delete", [this] { context_.actions.remove(selectionTargets()); }, true},
                           // Attach takes its place while another app waits for files.
                           {"share", "Share", [this] { context_.actions.share(selectionTargets()); }, false, false,
                            [this] { return !context_.actions.pickRequest().has_value(); }},
                           {"more_vert", "More", [this] { showMoreMenu(*dock_); }}},
             .selecting = [this] { return !context_.state.active()->selection().empty(); },
             .selectionCount = [this] { return static_cast<int>(context_.state.active()->selection().size()); }});
    // The file actions' job band above the dock, and their clipboard stamp leading its idle row.
    if (auto band = context_.actions.createBand()) {
        appendChild(band);
    }
    if (auto stamp = context_.actions.createDockStamp([this] { return shown_; })) {
        dock_->idleRow().insertChild(0, stamp);
    }
    appendChild(dock_);

    BrowseScreen *self = this;
    this->build([&] {
        // Insets: the list clears the top bar, the dock and the band.
        bind([self] {
            (void)theme();
            const float density = environment().density();
            const float bottom = environment().safeArea().bottom / density;
            const float top = self->context_.barHeight();
            const float band = self->context_.actions.bandShown() ? (currentTheme().style.utilitarian ? 40.0f : 50.0f) : 0.0f;
            untrack([&] {
                const Edges padding{0, top + 6, 0, bottom + bottomClearance() + band};
                self->list_->updateStyle([&](Style &s) { s.padding = padding; });
                self->emptySlot_->updateStyle([&](Style &s) { s.padding = {16, top, 16, bottom + bottomClearance()}; });
            });
        });
        // The status line: shown in the utilitarian style, over the toolbar.
        bind([self] {
            const Theme &th = theme();
            const float density = environment().density();
            const float bottom = environment().safeArea().bottom / density;
            (void)self->model_->version();
            const std::vector<std::string> selection = self->context_.state.active()->selection();
            const std::vector<runtime::StorageVolume> volumes = self->context_.volumes();
            untrack([&] {
                const bool show = th.style.utilitarian;
                self->statusLine_->updateStyle([&](Style &s) {
                    s.display = show;
                    s.height = th.spacing.statusLine;
                    s.bottom = bottom + th.spacing.dock - 1;
                });
                if (!show) {
                    return;
                }
                const runtime::StorageVolume *volume =
                        self->shown_ && self->shown_->isLocal() ? volumeFor(self->shown_->path(), volumes) : nullptr;
                self->statusText_->setText(statusLine(self->model_->visible(), selection, volume));
                self->statusText_->setColor(textInk(th, th.text.label, 11.5f));
            });
        });
        // A finished job changed the folder: watched folders update by themselves, others list
        // again.
        bind([self, last = std::make_shared<std::uint64_t>(0)] {
            const std::uint64_t changes = self->context_.actions.changes();
            untrack([&] {
                if (changes == *last) {
                    return;
                }
                *last = changes;
                const auto folders = self->context_.actions.changedFolders();
                if (self->shown_ && !self->model_->watching() && std::ranges::find(folders, *self->shown_) != folders.end()) {
                    self->model_->refresh();
                }
            });
        });
        // The route: a new folder or tab lists again.
        bind([self] {
            auto active = self->context_.state.active();
            const Route route = active->trackCurrent();
            (void)self->context_.volumes();
            untrack([&] { self->followRoute(); });
        });
        // Options: sort, hidden files (dot files and what the user hid here), the filter.
        bind([self] {
            const Settings &s = self->context_.state.settings();
            auto active = self->context_.state.active();
            const Route route = active->trackCurrent();
            DirectoryModel::Options options{
                    .sort = s.sort,
                    .showHidden = s.showHidden,
                    .filter = active->filter(),
                    .hiddenNames = route.location ? hiddenNamesIn(s.hiddenItems, *route.location)
                                                  : std::vector<std::string>()};
            untrack([&] { self->model_->setOptions(std::move(options)); });
        });
        // Contents and view mode.
        bind([self] {
            (void)self->model_->version();
            const ViewMode view = self->context_.state.active()->view();
            untrack([&] {
                self->view_ = view;
                self->rebuildItems();
            });
        });
        bind([self] {
            (void)self->model_->status();
            (void)self->model_->version();
            (void)self->context_.services.allFilesAccess();
            untrack([&] {
                self->updateEmptyState();
                self->tryReveal();
                if (self->refreshing_ && !self->refreshTimer_ &&
                    self->model_->status.peek() != DirectoryModel::Status::loading) {
                    self->finishRefresh();
                }
            });
        });
        // Selection and counts: rebind what's on screen.
        bind([self] {
            (void)self->context_.state.active()->selection();
            (void)self->model_->countVersion();
            untrack([&] { self->rebindVisible(); });
        });
        // Mobile data mode on a server: pending counts stop; off, the rows on screen ask again.
        bind([self] {
            const bool saving = self->context_.dataSaving();
            untrack([&] {
                if (!self->shown_ || self->shown_->kind() != vfs::BackendKind::kSftp) {
                    return;
                }
                if (saving) {
                    self->model_->cancelCountsExcept({});
                } else {
                    self->rebindVisible();
                }
            });
        });
        // Requests for rows that scrolled away are dropped.
        bind([self] {
            (void)self->list_->offsetSignal()();
            untrack([&] { self->pruneRequests(); });
        });
        // The pull indicator follows the pull.
        bind([self] {
            const Theme &t = currentTheme();
            self->pullIcon_->setColor(self->armed_() ? legible(t.colors.accent, t.colors.ground) : ink(t, 0.7f));
        });
        bind([self] {
            const float pull = self->pull_();
            const float top = self->context_.barHeight();
            const float width = environment().windowSize().width / environment().density();
            untrack([&] {
                const float progress = std::clamp(pull / kPullThreshold, 0.0f, 1.0f);
                self->pullIndicator_->updateStyle([&](Style &s) {
                    s.left = width / 2 - 20;
                    s.top = top - 40 + std::min(pull, kPullThreshold * 1.4f);
                });
                self->pullIndicator_->setOpacity(pull <= 0.5f ? 0.0f : std::min(1.0f, progress * 1.2f));
                if (!self->ticking_) {
                    self->pullIcon_->setTransform(rotation(progress * kPi * 1.5f));
                }
            });
        });
    });
}

void BrowseScreen::rebindVisible() {
    if (list_->itemCount() > 0) {
        list_->notifyChanged(0, list_->itemCount());
    }
}

void BrowseScreen::followRoute() {
    auto active = tab();
    const Route &route = active->current();
    if (route.kind != Route::Kind::browse || !route.location) {
        return;
    }
    if (shown_ == route.location && shownTab_ == active->id) {
        return;
    }
    saveScroll();
    // A refresh of the folder left behind ends quietly.
    cancelRefresh();
    // Each tab keeps its own selection; a new folder in the same tab starts without one.
    shown_ = *route.location;
    shownTab_ = active->id;
    dragAnchor_.reset();
    restore_ = active->scrollFor(*shown_);
    ++salt_;
    model_->load(*shown_);
    list_->setItemCount(0);
    list_->scrollTo(0);
    rebuildItems();
}

void BrowseScreen::reveal(const vfs::Location &file) {
    const auto folder = file.parent();
    if (!folder) {
        return;
    }
    revealing_ = Reveal{*folder, std::string(file.name())};
    context_.state.navigate(*folder);
    tryReveal();
}

void BrowseScreen::tryReveal() {
    if (!revealing_ || shown_ != revealing_->folder || model_->status.peek() == DirectoryModel::Status::loading) {
        return;
    }
    const std::string name = revealing_->name;
    revealing_.reset();
    restore_.reset();
    if (!model_->indexOf(name)) {
        return;  // Not listed here (hidden, or gone): the folder is enough.
    }
    flash_ = name;
    std::weak_ptr<Node> weak = weak_from_this();
    // After this frame's layout, so the list knows its rows; a third of a screen from the top.
    context_.services.loop.post([this, weak, name, salt = salt_] {
        if (weak.expired() || salt != salt_) {
            return;
        }
        if (auto entry = model_->indexOf(name)) {
            if (auto item = itemOf(*entry)) {
                const float height = environment().windowSize.peek().height / environment().density.peek();
                list_->scrollTo(std::max(0.0f, list_->itemOffset(*item) - height / 3));
                list_->notifyChanged(*item);
            }
        }
    });
    context_.services.loop.postDelayed(
            [this, weak, name] {
                if (weak.expired() || flash_ != name) {
                    return;
                }
                flash_.reset();
                if (list_->itemCount() > 0) {
                    list_->notifyChanged(0, list_->itemCount());
                }
            },
            kRevealFlashMs * 1'000'000);
}

void BrowseScreen::saveScroll() {
    if (!shown_) {
        return;
    }
    auto previous = context_.state.tab(shownTab_);
    if (!previous) {
        return;
    }
    TabModel::ScrollPosition position{.offset = list_->offset()};
    const std::size_t anchor = list_->anchorIndex();
    if (anchor < items_.size() && items_[anchor].type != Item::header && items_[anchor].begin < model_->visible().size()) {
        position.anchor = model_->visible()[items_[anchor].begin].name;
    }
    previous->setScroll(*shown_, std::move(position));
}

void BrowseScreen::restoreScroll() {
    if (!restore_) {
        return;
    }
    // Once listed, back to the same offset: after this frame's layout, so the list knows how long
    // it is. Only if the entry that was at the top is still there.
    if (model_->status.peek() == DirectoryModel::Status::loading) {
        return;
    }
    const TabModel::ScrollPosition position = *restore_;
    restore_.reset();
    if (position.offset <= 0 || (!position.anchor.empty() && !model_->indexOf(position.anchor))) {
        return;
    }
    std::weak_ptr<Node> weak = weak_from_this();
    context_.services.loop.post([this, weak, offset = position.offset, salt = salt_] {
        if (!weak.expired() && salt == salt_) {
            list_->scrollTo(offset);
        }
    });
}

void BrowseScreen::rebuildItems() {
    items_.clear();
    const auto &visible = model_->visible();
    itemOfEntry_.assign(visible.size(), 0);
    const std::size_t folders = model_->folderCount();
    const bool sections = model_->options().sort.directoriesFirst;
    const auto addRange = [&](std::size_t begin, std::size_t end) {
        if (view_ == ViewMode::grid) {
            for (std::size_t i = begin; i < end; i += kGridColumns) {
                const std::size_t stop = std::min(end, i + kGridColumns);
                for (std::size_t e = i; e < stop; ++e) {
                    itemOfEntry_[e] = items_.size();
                }
                items_.push_back({.type = Item::tiles, .begin = i, .end = stop});
            }
        } else {
            for (std::size_t i = begin; i < end; ++i) {
                itemOfEntry_[i] = items_.size();
                items_.push_back({.type = Item::row, .begin = i, .end = i + 1});
            }
        }
    };
    if (!visible.empty()) {
        if (sections) {
            int section = 0;
            if (folders > 0) {
                items_.push_back({.type = Item::header, .section = ++section, .label = "Folders"});
                addRange(0, folders);
            }
            if (folders < visible.size()) {
                items_.push_back({.type = Item::header, .section = ++section, .label = "Files"});
                addRange(folders, visible.size());
            }
        } else {
            addRange(0, visible.size());
        }
    }
    // The anchor item keeps its place on screen across the change, found by its key.
    itemOfKey_.clear();
    for (std::size_t i = 0; i < items_.size(); ++i) {
        itemOfKey_.emplace(keyOf(i), i);
    }
    list_->setItemCount(items_.size());
    restoreScroll();
}

// Salted per listing, so a name in the folder just left never anchors the new one.
std::uint64_t BrowseScreen::keyOf(std::size_t index) const {
    const Item &item = items_[index];
    if (item.type == Item::header) {
        return mix64(static_cast<std::uint64_t>(item.section) ^ (salt_ << 8));
    }
    const std::string &name = model_->visible()[item.begin].name;
    return mix64(std::hash<std::string>{}(name) ^ (static_cast<std::uint64_t>(item.type) << 60) ^ mix64(salt_));
}

std::optional<std::size_t> BrowseScreen::itemOf(std::size_t entry) const {
    if (entry >= itemOfEntry_.size()) {
        return std::nullopt;
    }
    return itemOfEntry_[entry];
}

FileItem BrowseScreen::fileItem(std::size_t index) {
    const vfs::Entry &entry = model_->visible()[index];
    const Theme &t = currentTheme();
    const FileKind kind = kitKind(entry);
    std::optional<std::uint64_t> count;
    if (entry.isDirectoryLike()) {
        count = model_->itemCount(entry.name);
        // Listing each folder on a server costs data: mobile data mode skips it.
        if (!count && !(context_.dataSaving.peek() && shown_ && shown_->kind() == vfs::BackendKind::kSftp)) {
            model_->requestCount(entry.name);
        }
    }
    const auto &selection = tab()->selection.peek();
    FileItem item{.name = entry.name,
                  .meta = metaLine(entry, count),
                  .size = sizeColumn(entry, count),
                  .date = entry.hasStat ? formatShortDate(entry.modifiedTime) : std::string(),
                  .icon = kindIcon(kind),
                  .kindColor = kindColor(kind, t),
                  .selected = std::ranges::binary_search(selection, entry.name),
                  .selecting = !selection.empty(),
                  .flash = flash_ == entry.name,
                  .spokenLabel = spokenDescription(entry, count)};
    // Shown hidden items (dot files, and what the user hid) are dimmed; the user's say so.
    const bool userHidden = model_->userHidden(entry.name);
    item.dimmed = entry.hidden || userHidden;
    if (userHidden) {
        item.meta = "Hidden · " + item.meta;
        item.spokenLabel += ", hidden";
    }
    const vfs::FileKind fileKind = entry.mimeType().kind;
    const ThumbnailSource source = shown_ ? thumbnailSource(*shown_, entry) : ThumbnailSource::none;
    if (source == ThumbnailSource::local) {
        const std::string path = shown_->child(entry.name).path();
        const std::string key = path + "|" + std::to_string(entry.modifiedTime.seconds) + "|" + std::to_string(entry.size);
        item.thumbnail = context_.thumbnails.get(key);
        item.video = fileKind == vfs::FileKind::kVideo;
        item.iconThumbnail = isIconThumbnail(fileKind);
        if (item.thumbnail == 0) {
            const float density = environment().density.peek();
            std::weak_ptr<Node> weak = weak_from_this();
            context_.thumbnails.request(key, path, static_cast<int>(std::lround(128 * density)),
                                        ThumbnailPersistence::disk, [this, weak, name = entry.name](gfx::ImageId) {
                                            if (weak.expired()) {
                                                return;
                                            }
                                            if (auto entryIndex = model_->indexOf(name)) {
                                                if (auto at = itemOf(*entryIndex)) {
                                                    list_->notifyChanged(*at);
                                                }
                                            }
                                        });
        }
    }
    // Images on an SFTP server: kept in memory and temporary files only (see thumbnailSource).
    if (source == ThumbnailSource::remote) {
        const vfs::Location location = shown_->child(entry.name);
        const std::string key = thumbnailKey(location, entry);
        item.thumbnail = context_.thumbnails.get(key);
        if (item.thumbnail == 0) {
            const float density = environment().density.peek();
            std::weak_ptr<Node> weak = weak_from_this();
            context_.thumbnails.requestRemote(key, location, entry.size, static_cast<int>(std::lround(128 * density)),
                                              [this, weak, name = entry.name](gfx::ImageId) {
                                                  if (weak.expired()) {
                                                      return;
                                                  }
                                                  if (auto entryIndex = model_->indexOf(name)) {
                                                      if (auto at = itemOf(*entryIndex)) {
                                                          list_->notifyChanged(*at);
                                                      }
                                                  }
                                              });
        }
    }
    return item;
}

void BrowseScreen::bindItem(std::size_t index, Node &node) {
    if (index >= items_.size()) {
        return;
    }
    const Item &item = items_[index];
    switch (item.type) {
    case Item::header: {
        node.clearChildren();
        node.appendChild(SectionLabel(item.label));
        break;
    }
    case Item::row: {
        boundEntry_[&node] = item.begin;
        static_cast<FileRow &>(node).setData(fileItem(item.begin));
        break;
    }
    case Item::tiles: {
        auto &row = static_cast<TileRow &>(node);
        for (int k = 0; k < kGridColumns; ++k) {
            GridTile &tile = *row.tiles[k];
            const std::size_t entry = item.begin + static_cast<std::size_t>(k);
            if (entry < item.end) {
                boundEntry_[&tile] = entry;
                tile.setOpacity(1);
                tile.setHitTestBehavior(HitTestBehavior::deferToChild);
                tile.updateSemantics([](Semantics &s) { s.hidden = false; });
                tile.setData(fileItem(entry));
            } else {
                // Keeps its column but shows nothing.
                boundEntry_.erase(&tile);
                tile.setOpacity(0);
                tile.setHitTestBehavior(HitTestBehavior::none);
                tile.updateSemantics([](Semantics &s) { s.hidden = true; });
            }
        }
        break;
    }
    }
}

void BrowseScreen::pruneRequests() {
    const auto [first, last] = list_->boundRange();
    std::vector<std::string> names;
    std::unordered_set<std::string> keys;
    const auto &visible = model_->visible();
    for (std::size_t i = first; i < last && i < items_.size(); ++i) {
        for (std::size_t e = items_[i].begin; e < items_[i].end && e < visible.size(); ++e) {
            names.push_back(visible[e].name);
            if (shown_) {
                keys.insert(thumbnailKey(shown_->child(visible[e].name), visible[e]));
            }
        }
    }
    model_->cancelCountsExcept(names);
    context_.thumbnails.cancelExcept(keys);
}

void BrowseScreen::activate(std::size_t index) {
    const auto &visible = model_->visible();
    if (index >= visible.size() || !shown_) {
        return;
    }
    const vfs::Entry &entry = visible[index];
    if (!tab()->selection.peek().empty()) {
        context_.state.toggleSelected(entry.name);
        return;
    }
    const vfs::Location location = shown_->child(entry.name);
    if (entry.isDirectoryLike()) {
        context_.state.navigate(location);
        return;
    }
    // Another app waiting for files: a tap picks one. An archive opens as a folder to pick from,
    // unless the app asked for its type.
    if (const auto picking = untrack([&] { return context_.actions.pickRequest(); });
        picking && entry.targetType == vfs::EntryType::kFile &&
        (entry.mimeType().kind != vfs::FileKind::kArchive ||
         std::ranges::contains(picking->mimeTypes, entry.mimeType().type))) {
        context_.actions.pick({*shown_, {entry}});
        return;
    }
    if (entry.targetType == vfs::EntryType::kFile && entry.mimeType().kind == vfs::FileKind::kArchive) {
        const vfs::Location inside = vfs::Location::archive(location, "/");
        if (context_.services.backends.backendFor(inside) != nullptr) {
            context_.state.navigate(inside);
            return;
        }
    }
    // Local files and files on servers, which the viewer streams. Archive entries aren't cheaply
    // seekable: they're copied out as below.
    const bool viewable = location.isLocal() || (location.kind() == vfs::BackendKind::kSftp &&
                                                 context_.services.backends.backendFor(location) != nullptr);
    if (viewable && context_.openViewer && viewerKind(entry)) {
        // The viewer, with every image and video here to swipe through, in this order.
        std::vector<MediaItem> media;
        std::size_t at = 0;
        for (std::size_t i = 0; i < visible.size(); ++i) {
            if (const auto kind = viewerKind(visible[i])) {
                if (i == index) {
                    at = media.size();
                }
                const vfs::Location child = shown_->child(visible[i].name);
                media.push_back({child.path(), visible[i].name, std::string(visible[i].mimeType().type), *kind,
                                 *shown_, visible[i], child});
            }
        }
        context_.openViewer(std::move(media), at);
        return;
    }
    // Text files open in the editor, wherever they are: it reads them through their backend.
    if (context_.openText && isTextFile(entry) && context_.services.backends.backendFor(location) != nullptr) {
        context_.openText({*shown_, {entry}});
        return;
    }
    if (!location.isLocal()) {
        // Other apps can only open local files: the actions copy it out first.
        context_.actions.open({*shown_, {entry}});
        return;
    }
    context_.services.platform.openFile(location.path(), std::string(entry.mimeType().type));
}

void BrowseScreen::longPress(std::size_t index) {
    const auto &visible = model_->visible();
    if (index >= visible.size()) {
        return;
    }
    auto current = tab();
    std::vector<std::string> names = current->selection.peek();
    if (std::ranges::binary_search(names, visible[index].name)) {
        // A long press on what's already selected opens its menu.
        for (const auto &[node, entry] : boundEntry_) {
            if (entry == index) {
                showMoreMenu(*node, true);
                return;
            }
        }
        showMoreMenu(*dock_, true);
        return;
    }
    if (pointer_) {
        dragAnchor_ = index;
        dragBase_ = names;
    }
    names.push_back(visible[index].name);
    context_.state.setSelection(std::move(names));
}

void BrowseScreen::dragTo(std::size_t index) {
    if (!dragAnchor_) {
        return;
    }
    const auto &visible = model_->visible();
    const std::size_t from = std::min(*dragAnchor_, index);
    const std::size_t to = std::min(std::max(*dragAnchor_, index), visible.empty() ? 0 : visible.size() - 1);
    std::vector<std::string> names = dragBase_;
    for (std::size_t i = from; i <= to && i < visible.size(); ++i) {
        names.push_back(visible[i].name);
    }
    const std::size_t before = tab()->selection.peek().size();
    context_.state.setSelection(std::move(names));
    if (tab()->selection.peek().size() != before) {
        haptic(Haptic::tick);
    }
}

void BrowseScreen::endDrag() {
    dragAnchor_.reset();
    dragBase_.clear();
}

std::optional<std::size_t> BrowseScreen::entryAt(gfx::Vec2 window) const {
    const float density = environment().density.peek();
    const auto hit = [&](const Node &node) {
        const gfx::Transform t = nodeToWindow(node, density);
        const gfx::Rect frame = node.frame();
        return gfx::Rect{t.tx, t.ty, frame.width * density, frame.height * density}.contains(window);
    };
    const auto [first, last] = list_->boundRange();
    for (std::size_t i = first; i < last && i < items_.size(); ++i) {
        const Node *node = list_->nodeFor(i);
        if (node == nullptr || items_[i].type == Item::header || !hit(*node)) {
            continue;
        }
        if (items_[i].type == Item::row) {
            return items_[i].begin;
        }
        for (const auto &child : node->children()) {
            if (const auto found = boundEntry_.find(child.get()); found != boundEntry_.end() && hit(*child)) {
                return found->second;
            }
        }
        // Between tiles: the nearest one in the row.
        return items_[i].begin;
    }
    return std::nullopt;
}

void BrowseScreen::onPointer(const PointerEvent &event) {
    switch (event.action) {
    case PointerAction::down:
        if (!pointer_) {
            pointer_ = event.pointer;
        }
        break;
    case PointerAction::move:
        if (pointer_ != event.pointer || !dragAnchor_) {
            break;
        }
        {
            // Near the edges, the list scrolls so the range can grow past the screen.
            const float density = environment().density.peek();
            const float y = event.position.y / density;
            const float top = context_.barHeight.peek() + 24;
            const float bottom =
                    environment().windowSize.peek().height / density - environment().safeArea.peek().bottom / density -
                    bottomClearance();
            if (y < top) {
                list_->scrollBy(-std::min(24.0f, (top - y) * 0.5f));
            } else if (y > bottom) {
                list_->scrollBy(std::min(24.0f, (y - bottom) * 0.5f));
            }
            if (auto entry = entryAt(event.position)) {
                dragTo(*entry);
            }
        }
        break;
    case PointerAction::up:
    case PointerAction::cancel:
        if (pointer_ != event.pointer) {
            break;
        }
        pointer_.reset();
        endDrag();
        if (pull_.peek() > 0 && !feedback_) {
            if (armed_.peek() && event.action == PointerAction::up) {
                refresh(RefreshFeedback::shown);
            } else {
                armed_.set(false);
                pull_.animateTo(0, Spring{currentTheme().motion.springStiffness, 1});
            }
        }
        break;
    }
}

void BrowseScreen::setPull(float pull) {
    pull_.snapTo(pull);
    if (!armed_.peek() && pull >= kPullThreshold) {
        armed_.set(true);
        haptic(Haptic::tick);
    } else if (armed_.peek() && pull < kPullDisarm) {
        armed_.set(false);
    }
}

void BrowseScreen::refresh(RefreshFeedback feedback) {
    if (!shown_) {
        return;
    }
    if (feedback == RefreshFeedback::shown) {
        if (refreshing_ && feedback_) {
            return;  // Already showing one.
        }
        feedback_ = true;
        refreshStartNs_ = context_.services.loop.now();
        armed_.set(true);
        pull_.animateTo(kPullThreshold * 0.8f, Spring{currentTheme().motion.springStiffness, 1});
        setSpinning(!environment().reducedMotion.peek());
        context_.services.platform.announce("Refreshing");
    }
    refreshing_ = true;
    if (model_->status.peek() == DirectoryModel::Status::error) {
        model_->load(*shown_);
    } else {
        model_->refresh();
    }
}

void BrowseScreen::finishRefresh() {
    if (!feedback_) {
        refreshing_ = false;
        return;
    }
    // A fast listing keeps the indicator up for kMinRefreshMs, so it doesn't just blink.
    const std::int64_t elapsed = context_.services.loop.now() - refreshStartNs_;
    const std::int64_t remaining = kMinRefreshMs * 1'000'000 - elapsed;
    if (remaining <= 0) {
        endRefresh();
        return;
    }
    std::weak_ptr<Node> weak = weak_from_this();
    refreshTimer_ = context_.services.loop.postDelayed(
            [this, weak] {
                if (weak.expired()) {
                    return;
                }
                refreshTimer_.reset();
                endRefresh();
            },
            remaining);
}

void BrowseScreen::endRefresh() {
    const bool failed = model_->status.peek() == DirectoryModel::Status::error;
    const std::size_t count = model_->visible().size();
    const bool feedback = feedback_;
    cancelRefresh();
    if (feedback) {
        context_.notify(failed ? std::string("Couldn't refresh") : "Refreshed · " + formatCount(count));
    }
}

void BrowseScreen::cancelRefresh() {
    if (refreshTimer_) {
        context_.services.loop.cancel(*refreshTimer_);
        refreshTimer_.reset();
    }
    const bool shown = feedback_;
    refreshing_ = false;
    feedback_ = false;
    setSpinning(false);
    if (shown) {
        armed_.set(false);
        pull_.animateTo(0, Spring{currentTheme().motion.springStiffness, 1});
    }
}

void BrowseScreen::setSpinning(bool on) {
    if (on == ticking_) {
        return;
    }
    ticking_ = on;
    if (on) {
        spinBase_ = std::clamp(pull_.peek() / kPullThreshold, 0.0f, 1.0f) * kPi * 1.5f;
        spinStartNs_ = -1;
        frameClock().start(this);
    } else {
        frameClock().stop(this);
    }
}

bool BrowseScreen::tick(std::int64_t nowNs) {
    if (!ticking_) {
        return false;
    }
    if (spinStartNs_ < 0) {
        spinStartNs_ = nowNs;
    }
    // About 1.2 turns a second.
    const float seconds = static_cast<float>(nowNs - spinStartNs_) * 1e-9f;
    pullIcon_->setTransform(rotation(spinBase_ + seconds * kPi * 2.4f));
    return true;
}

Node *BrowseScreen::emptyState() const {
    return emptySlot_->children().empty() ? nullptr : emptySlot_->children().front().get();
}

void BrowseScreen::updateEmptyState() {
    const auto status = model_->status.peek();
    const bool empty = model_->visible().empty();
    std::string key;
    std::string title;
    std::string message;
    std::vector<std::pair<std::string, std::function<void()>>> buttons;
    if (status == DirectoryModel::Status::error && model_->error.peek()) {
        const Error &error = *model_->error.peek();
        key = "error" + std::to_string(error.code) + error.message;
        title = "Can't open";
        message = errorMessage(error);
        buttons.emplace_back("Retry", [this] { refresh(); });
        if (!context_.services.allFilesAccess.peek() && shown_ && shown_->isLocal()) {
            key += "+access";
            buttons.emplace_back("Grant access", [this] { context_.services.platform.requestAllFilesAccess(); });
        }
    } else if (status == DirectoryModel::Status::loaded && empty) {
        const auto &options = model_->options();
        if (!options.filter.empty()) {
            key = "nomatch" + options.filter;
            title = "No match";
            message = "Nothing here matches “" + options.filter + "”.";
        } else if (model_->totalCount() > 0) {
            key = "hidden";
            title = "Empty";
            message = "Only hidden files here.";
            buttons.emplace_back("Show hidden", [this] {
                context_.state.updateSettings([](Settings &s) { s.showHidden = true; });
            });
        } else {
            key = "empty";
            title = "Empty";
            message = "Nothing in this folder yet.";
        }
    }
    if (key == emptyKey_) {
        return;
    }
    emptyKey_ = key;
    emptySlot_->clearChildren();
    if (key.empty()) {
        return;
    }
    std::string path;
    if (shown_) {
        for (const Crumb &crumb : crumbsFor(*shown_, context_.volumes.peek())) {
            path += (path.empty() ? "" : " / ") + crumb.label;
        }
    }
    auto column = Box::create({.alignItems = Align::center, .gap = 16, .shrink = 1});
    column->appendChild(EmptyState({.title = title, .path = path, .message = message, .orbSize = 120}));
    if (!buttons.empty()) {
        auto row = Box::create({.direction = Direction::row, .justify = Justify::center, .gap = 12});
        bool first = true;
        for (auto &[label, onTap] : buttons) {
            row->appendChild(Button::create(
                    {.label = label, .variant = first ? ButtonVariant::primary : ButtonVariant::glass, .onTap = onTap}));
            first = false;
        }
        column->appendChild(row);
    }
    emptySlot_->appendChild(column);
}

std::vector<vfs::Entry> BrowseScreen::selectedEntries() const {
    std::vector<vfs::Entry> out;
    const auto &selection = context_.state.peekActive()->selection.peek();
    for (const vfs::Entry &entry : model_->visible()) {
        if (std::ranges::binary_search(selection, entry.name)) {
            out.push_back(entry);
        }
    }
    return out;
}

FileTargets BrowseScreen::selectionTargets() const {
    return {shown_.value_or(vfs::Location::local("/")), selectedEntries()};
}

void BrowseScreen::selectAll() {
    std::vector<std::string> names;
    for (const vfs::Entry &entry : model_->visible()) {
        names.push_back(entry.name);
    }
    context_.state.setSelection(std::move(names));
}

void BrowseScreen::showNewMenu(const Node &anchor) {
    if (!shown_) {
        return;
    }
    const vfs::Location here = *shown_;
    std::vector<MenuItem> items{
            {fileActionIcon(FileAction::newFolder), fileActionLabel(FileAction::newFolder),
             [this, here] { context_.actions.newFolder(here); }},
            {fileActionIcon(FileAction::newFile), fileActionLabel(FileAction::newFile),
             [this, here] { context_.actions.newFile(here); }},
    };
    if (context_.actions.canPaste()) {
        items.push_back({fileActionIcon(FileAction::paste), fileActionLabel(FileAction::paste),
                         [this, here] { context_.actions.paste(here); }});
    }
    context_.overlays.showMenu(std::move(items), anchor);
}

void BrowseScreen::setHidden(const FileTargets &targets, bool hidden) {
    const auto locations = targets.locations();
    if (locations.empty()) {
        return;
    }
    // Undo puts back exactly those this changed.
    std::vector<vfs::Location> changed;
    for (const vfs::Location &location : locations) {
        if (context_.state.isHiddenItem(location) != hidden) {
            changed.push_back(location);
        }
    }
    batch([&] {
        context_.state.setHidden(locations, hidden);
        context_.state.clearSelection();
    });
    const std::string what = locations.size() == 1 ? "“" + std::string(locations.front().name()) + "”"
                                                   : formatCount(locations.size());
    std::string text = (hidden ? "Hid " : "Unhid ") + what;
    if (hidden && context_.state.settings.peek().showHidden) {
        text += " · hidden files are shown";
    }
    AppState *state = &context_.state;
    context_.overlays.showSnackbar({.text = std::move(text),
                                    .action = "Undo",
                                    .onAction = [state, changed, hidden] { state->setHidden(changed, !hidden); }});
}

void BrowseScreen::showMoreMenu(const Node &anchor, bool all) {
    const FileTargets targets = selectionTargets();
    std::vector<MenuItem> items;
    const auto add = [&](FileAction action, bool destructive = false) {
        items.push_back({fileActionIcon(action), fileActionLabel(action),
                         [this, action, targets] { context_.actions.perform(action, targets); }, destructive});
    };
    const bool single = targets.entries.size() == 1;
    if (all) {
        // A row's own menu: what the dock offers too.
        add(FileAction::copy);
        add(FileAction::cut);
        add(FileAction::share);
    }
    if (single) {
        add(FileAction::rename);
    }
    const bool archives = !targets.entries.empty() && std::ranges::all_of(targets.entries, [](const vfs::Entry &e) {
        return !e.isDirectoryLike() && e.mimeType().kind == vfs::FileKind::kArchive;
    });
    if (archives) {
        add(FileAction::extract);
        add(FileAction::verify);
    }
    add(FileAction::compress);
    if (single && !targets.entries[0].isDirectoryLike()) {
        add(FileAction::openWith);
        if (context_.openText) {
            items.push_back({"article", "Open as text", [this, targets] { context_.openText(targets); }});
        }
    }
    if (!targets.entries.empty()) {
        // Files and folders alike, any number; remove only when every one already is.
        const bool favorites = allFavorites(context_, targets);
        items.push_back({.icon = "star",
                         .label = favorites ? "Remove from favorites" : "Add to favorites",
                         .onSelect = [this, targets, favorites] { setFavorites(context_, targets, !favorites); },
                         .filled = favorites});
        // Hide (or unhide, when every one is hidden already), with an undo.
        const auto locations = targets.locations();
        const bool hidden = std::ranges::all_of(locations, [&](const vfs::Location &l) {
            return untrack([&] { return context_.state.isHiddenItem(l); });
        });
        items.push_back({hidden ? "visibility" : "visibility_off", hidden ? "Unhide" : "Hide",
                         [this, targets, hidden] { setHidden(targets, !hidden); }});
    }
    add(FileAction::properties);
    if (all) {
        add(FileAction::remove, true);
    } else {
        items.push_back({"select_all", "Select all", [this] { selectAll(); }});
    }
    context_.overlays.showMenu(std::move(items), anchor);
}

namespace {

// What ascending and descending mean for a key, at the order row's end.
const char *orderHint(vfs::SortKey key, bool descending) {
    switch (key) {
    case vfs::SortKey::kSize:
        return descending ? "big first" : "small first";
    case vfs::SortKey::kModified:
        return descending ? "new first" : "old first";
    default:
        break;
    }
    return descending ? "Z → A" : "A → Z";
}

// A menu row with an icon, a label and something at its end; tapping it runs onTap.
std::shared_ptr<Box> menuLine(const char *icon, float height, std::function<void()> onTap) {
    auto row = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 14,
                            .padding = Edges::symmetric(16, 0), .height = height});
    row->appendChild(Icon::create(icon, 20));
    if (onTap) {
        auto pressable = makePressable(*row, {.onTap = std::move(onTap)});
        row->setPaintHook([pressable, r = row.get()](PaintContext &context) {
            const gfx::RoundedRect shape{{context.px(6), 0, context.size.width - context.px(12), context.size.height},
                                         gfx::CornerRadii::uniform(context.px(currentTheme().corner(14)))};
            if (pressable->pressed.peek()) {
                context.list.fill(shape, currentTheme().colors.ink.withAlpha(0.10f));
            }
            if (r->focused()) {
                context.list.stroke(shape, context.px(2), currentTheme().colors.accent);
            }
        });
        row->bind([r = row.get(), pressable] {
            (void)pressable->pressed();
            (void)r->trackFocused();
            untrack([&] { r->markPaintDirty(); });
        });
    }
    return row;
}

// A quiet caps label over a row of chips; a heading in semantics.
NodePtr menuLabel(const char *text) {
    const Theme &t = currentTheme();
    auto label = Box::create({.justify = Justify::end, .padding = {18, 0, 18, 6}, .height = 30});
    auto labelText = Text::create(text, t.type.section, ink(t, t.text.quiet));
    label->appendChild(labelText);
    label->updateSemantics([text = std::string(text)](Semantics &s) {
        s.role = Role::heading;
        s.label = text;
    });
    labelText->updateSemantics([](Semantics &s) { s.hidden = true; });
    return label;
}

// One of a row of choices (sort keys, view modes): accent-tinted while on() holds, a checkable
// button in semantics. on is tracked.
NodePtr menuChip(const char *debugName, std::string text, std::string icon, std::string spoken,
                 std::function<bool()> on, std::function<void()> onTap) {
    const Theme &t = currentTheme();
    auto chip = Box::create({.direction = Direction::row, .justify = Justify::center, .alignItems = Align::center,
                             .gap = 6, .grow = 1, .shrink = 1, .basis = 0, .height = 34});
    chip->setDebugName(debugName);
    std::shared_ptr<Icon> glyph;
    if (!icon.empty()) {
        glyph = Icon::create(icon, 17);
        chip->appendChild(glyph);
    }
    auto label = Text::create(std::move(text), withSize(t.type.body, 12.5f));
    label->setMaxLines(1);
    label->updateSemantics([](Semantics &s) { s.hidden = true; });
    chip->appendChild(label);
    auto pressable = makePressable(*chip, {.onTap = std::move(onTap), .haptic = Haptic::tick});
    chip->setPaintHook([on, pressable, c = chip.get()](PaintContext &context) {
        const Theme &th = currentTheme();
        const gfx::RoundedRect shape{context.bounds(), gfx::CornerRadii::uniform(context.px(th.corner(10)))};
        if (untrack(on)) {
            context.list.fill(shape, th.colors.accent.withAlpha(0.20f));
            context.list.stroke(shape, std::max(1.0f, context.density * 0.75f), th.colors.accent.withAlpha(0.5f));
        } else {
            context.list.fill(shape, th.colors.ink.withAlpha(0.05f));
        }
        if (pressable->pressed.peek()) {
            context.list.fill(shape, th.colors.ink.withAlpha(0.08f));
        }
        if (c->focused()) {
            context.list.stroke(shape, context.px(2), th.colors.accent);
        }
    });
    chip->bind([on, pressable, c = chip.get(), x = label.get(), g = glyph.get(), spoken = std::move(spoken)] {
        const bool selected = on();
        const Theme &th = theme();
        (void)pressable->pressed();
        (void)c->trackFocused();
        untrack([&] {
            const gfx::Color color = selected ? ink(th, th.text.primary) : ink(th, 0.78f);
            x->setColor(color);
            if (g != nullptr) {
                g->setColor(color);
            }
            c->markPaintDirty();
            c->updateSemantics([&](Semantics &s) {
                s.role = Role::button;
                s.label = spoken;
            });
            setSemanticsFlag(*c, semantics_flags::kCheckable, true);
            setSemanticsFlag(*c, semantics_flags::kChecked, selected);
        });
    });
    return chip;
}

}  // namespace

NodePtr BrowseScreen::sortControls() {
    AppState *state = &context_.state;
    const Theme &t = currentTheme();
    auto column = Box::create({});
    column->setDebugName("SortControls");

    column->appendChild(menuLabel("Sort by"));
    auto chips = Box::create({.direction = Direction::row, .gap = 4, .padding = {12, 0, 12, 6}, .height = 40});
    static constexpr const char *kKeys[] = {"Name", "Size", "Date", "Type"};
    for (std::size_t i = 0; i < std::size(kKeys); ++i) {
        const auto key = static_cast<vfs::SortKey>(i);
        chips->appendChild(menuChip(
                "SortChip", kKeys[i], {}, std::string("Sort by ") + kKeys[i],
                [state, key] { return state->settings().sort.key == key; },
                [state, key] {
                    state->updateSettings([&](Settings &s) {
                        if (s.sort.key == key) {
                            s.sort.descending = !s.sort.descending;
                        } else {
                            s.sort.key = key;
                            // Sizes and dates read best biggest and newest first.
                            s.sort.descending = key == vfs::SortKey::kSize || key == vfs::SortKey::kModified;
                        }
                    });
                }));
    }
    column->appendChild(chips);

    auto order = menuLine("arrow_upward", 46, [state] {
        state->updateSettings([](Settings &s) { s.sort.descending = !s.sort.descending; });
    });
    auto orderText = Text::create({}, t.type.body);
    orderText->setStyle({.grow = 1, .shrink = 1, .basis = 0});
    orderText->setMaxLines(1);
    gfx::TextStyle mono = withSize(t.type.label, 12);
    mono.uppercase = false;
    mono.letterSpacing = 0;
    auto hint = Text::create({}, mono);
    hint->setMaxLines(1);
    for (Node *n : {static_cast<Node *>(orderText.get()), static_cast<Node *>(hint.get())}) {
        n->updateSemantics([](Semantics &s) { s.hidden = true; });
    }
    order->appendChild(orderText);
    order->appendChild(hint);
    order->bind([state, o = order.get(), x = orderText.get(), hi = hint.get()] {
        const auto sort = state->settings().sort;
        const Theme &th = theme();
        untrack([&] {
            static_cast<Icon &>(*o->children()[0]).setIcon(sort.descending ? "arrow_downward" : "arrow_upward");
            static_cast<Icon &>(*o->children()[0]).setColor(ink(th, 0.78f));
            x->setText(sort.descending ? "Descending" : "Ascending");
            x->setColor(ink(th, th.text.primary));
            hi->setText(orderHint(sort.key, sort.descending));
            hi->setColor(ink(th, th.text.meta));
            o->updateSemantics([&](Semantics &s) {
                s.role = Role::button;
                s.label = std::string("Order: ") + (sort.descending ? "Descending" : "Ascending") + ", " +
                          orderHint(sort.key, sort.descending);
            });
        });
    });
    column->appendChild(order);

    auto folders = menuLine("folder_copy", 48, nullptr);
    auto foldersText = Text::create("Folders first", t.type.body);
    foldersText->setStyle({.grow = 1, .shrink = 1, .basis = 0});
    foldersText->updateSemantics([](Semantics &s) { s.hidden = true; });
    folders->appendChild(foldersText);
    folders->appendChild(Switch::create({.checked = [state] { return state->settings().sort.directoriesFirst; },
                                         .onChange = [state](bool on) {
                                             state->updateSettings([&](Settings &s) { s.sort.directoriesFirst = on; });
                                         },
                                         .label = "Folders first"}));
    folders->bind([f = folders.get(), x = foldersText.get()] {
        const Theme &th = theme();
        untrack([&] {
            static_cast<Icon &>(*f->children()[0]).setColor(ink(th, 0.78f));
            x->setColor(ink(th, th.text.primary));
        });
    });
    column->appendChild(folders);

    // The view, for this tab and as the default for new ones.
    column->appendChild(menuLabel("View"));
    auto views = Box::create({.direction = Direction::row, .gap = 4, .padding = {12, 0, 12, 6}, .height = 40});
    BrowseScreen *self = this;
    for (const ViewMode mode : {ViewMode::list, ViewMode::grid}) {
        const bool grid = mode == ViewMode::grid;
        views->appendChild(menuChip(
                "ViewChip", grid ? "Grid" : "List", grid ? "grid_view" : "view_list", grid ? "Grid view" : "List view",
                [state, mode] { return state->active()->view() == mode; },
                [self, state, mode] {
                    batch([&] {
                        self->tab()->view.set(mode);
                        state->updateSettings([&](Settings &s) { s.view = mode; });
                    });
                }));
    }
    column->appendChild(views);

    auto rule = Box::create({.height = 13});
    rule->updateSemantics([](Semantics &s) { s.hidden = true; });
    rule->setPaintHook([](PaintContext &context) {
        const gfx::Rect b = context.bounds();
        context.list.fill(gfx::Rect{context.px(14), std::round(b.height / 2), b.width - context.px(28),
                                    std::max(1.0f, context.density * 0.5f)},
                          currentTheme().colors.ink.withAlpha(0.08f));
    });
    column->appendChild(rule);
    return column;
}

}  // namespace filepan::app
