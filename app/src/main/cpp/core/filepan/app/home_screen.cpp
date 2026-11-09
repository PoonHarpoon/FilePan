// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/home_screen.hpp"

#include <algorithm>
#include <cmath>

#include "filepan/app/document_trees.hpp"
#include "filepan/app/favorites_sheet.hpp"
#include "filepan/app/format.hpp"
#include "filepan/app/places.hpp"
#include "filepan/app/sftp_screens.hpp"

namespace filepan::app {

namespace {

using namespace ui;
using namespace ui::components;

NodePtr section(const char *label) {
    auto s = SectionLabel(label);
    s->updateStyle([](Style &st) { st.padding = {4, 14, 0, 2}; });
    return s;
}

std::string spaceLine(const runtime::StorageVolume &volume) {
    if (volume.totalBytes == 0) {
        return volume.state == "mounted_ro" ? "Read only" : std::string();
    }
    return formatSize(volume.freeBytes) + " free of " + formatSize(volume.totalBytes);
}

NodePtr volumeCard(AppContext &context, const runtime::StorageVolume &volume) {
    const Theme &t = currentTheme();
    auto card = Glass::create({.direction = Direction::row, .alignItems = Align::center, .gap = 18,
                               .padding = Edges::all(20)},
                              {.radii = gfx::CornerRadii::uniform(t.radii.panel),
                               .cornerTicks = volume.primary});
    const float used = volume.totalBytes == 0
                               ? 0.0f
                               : static_cast<float>(volume.totalBytes - std::min(volume.freeBytes, volume.totalBytes)) /
                                         static_cast<float>(volume.totalBytes);
    auto meter = MeterRing::create({.value = used, .label = volumeLabel(volume) + " used", .size = 104});
    meter->updateSemantics([](Semantics &s) { s.hidden = true; });
    card->appendChild(meter);
    auto texts = Box::create({.alignItems = Align::start, .gap = 6, .grow = 1, .shrink = 1, .basis = 0});
    if (volume.primary) {
        texts->appendChild(Stamp("Primary"));
    } else if (volume.removable) {
        texts->appendChild(Stamp("Removable", StampTone::secondary));
    }
    const std::string label = volumeLabel(volume);
    auto title = Text::create(label, t.type.title, t.colors.ink);
    title->setMaxLines(1);
    texts->appendChild(title);
    const std::string space = spaceLine(volume);
    if (!space.empty()) {
        texts->appendChild(Text::create(space, t.type.label, textInk(t, t.text.label, 11)));
    }
    auto path = Text::create(volume.description.empty() ? volume.path : volume.description, t.type.label,
                             textInk(t, t.text.meta, 11));
    path->setMaxLines(1);
    path->setOverflow(gfx::TextOverflow::ellipsisMiddle);
    texts->appendChild(path);
    for (const auto &child : texts->children()) {
        child->updateSemantics([](Semantics &s) { s.hidden = true; });
    }
    card->appendChild(texts);
    auto chevron = Icon::create("chevron_right", 24, ink(t, 0.6f));
    card->appendChild(chevron);

    AppState *state = &context.state;
    const std::string mount = volume.path;
    auto pressable = makePressable(*card, {.onTap = [state, mount] { state->navigate(vfs::Location::local(mount)); }});
    Glass *raw = card.get();
    card->setOverlayHook([raw, pressable](PaintContext &context) {
        if (pressable->pressed.peek()) {
            const Theme &th = currentTheme();
            context.list.fill(gfx::RoundedRect{context.bounds(), gfx::CornerRadii::uniform(context.px(th.radii.panel))},
                              th.colors.ink.withAlpha(0.06f));
        }
        (void)raw;
    });
    card->bind([raw, pressable] {
        (void)pressable->pressed();
        untrack([&] { raw->markPaintDirty(); });
    });
    card->updateSemantics([&](Semantics &s) {
        s.role = Role::button;
        s.label = label + ", " + std::to_string(static_cast<int>(std::lround(used * 100))) + "% used" +
                  (space.empty() ? std::string() : ", " + space);
    });
    return card;
}

// The utilitarian volume: a full-width row with the name, a thin usage bar, and the space in mono.
NodePtr volumeRow(AppContext &context, const runtime::StorageVolume &volume) {
    const Theme &t = currentTheme();
    auto row = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 14,
                            .padding = {14, 10, 12, 10}, .margin = {-16, 0, -16, 0}, .minHeight = 56});
    const float used = volume.totalBytes == 0
                               ? 0.0f
                               : static_cast<float>(volume.totalBytes - std::min(volume.freeBytes, volume.totalBytes)) /
                                         static_cast<float>(volume.totalBytes);
    row->appendChild(Icon::create(volume.removable ? "sd_card" : "storage", 20, ink(t, 0.75f)));
    auto texts = Box::create({.gap = 5, .grow = 1, .shrink = 1, .basis = 0});
    const std::string label = volumeLabel(volume);
    auto top = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 8});
    auto title = Text::create(label, t.type.body, ink(t, t.text.primary));
    title->setMaxLines(1);
    title->setStyle({.shrink = 1});
    top->appendChild(title);
    if (volume.removable) {
        top->appendChild(Text::create("removable", t.type.meta, textInk(t, t.text.meta, t.type.meta.size)));
    }
    texts->appendChild(top);
    auto bar = Box::create({.height = 4});
    bar->setPaintHook([used](PaintContext &context) {
        const Theme &th = currentTheme();
        const gfx::Rect b = context.bounds();
        context.list.fill(b, th.colors.ink.withAlpha(th.colors.divider));
        context.list.fill(gfx::Rect{b.x, b.y, std::round(b.width * std::clamp(used, 0.0f, 1.0f)), b.height},
                          used > 0.9f ? th.colors.destructive : th.colors.accent);
    });
    texts->appendChild(bar);
    std::string space = spaceLine(volume);
    if (volume.totalBytes > 0) {
        space = formatSizeOf(volume.totalBytes - std::min(volume.freeBytes, volume.totalBytes), volume.totalBytes) +
                " used · " + formatSize(volume.freeBytes) + " free";
    }
    if (!space.empty()) {
        texts->appendChild(Text::create(space, gfx::TextStyle{.family = gfx::FontFamily::mono, .size = 11.5f,
                                                               .weight = 450, .lineHeight = 1.3f},
                                        textInk(t, t.text.label, 11.5f)));
    }
    for (const auto &child : texts->children()) {
        child->updateSemantics([](Semantics &s) { s.hidden = true; });
    }
    row->appendChild(texts);
    row->appendChild(Icon::create("chevron_right", 20, ink(t, 0.5f)));

    AppState *state = &context.state;
    const std::string mount = volume.path;
    auto pressable = makePressable(*row, {.onTap = [state, mount] { state->navigate(vfs::Location::local(mount)); }});
    Box *raw = row.get();
    row->setPaintHook([pressable](PaintContext &context) {
        const Theme &th = currentTheme();
        const gfx::Rect b = context.bounds();
        if (pressable->pressed.peek()) {
            context.list.fill(b, th.colors.ink.withAlpha(th.colors.pressed * 0.6f));
        }
        const float hair = std::max(1.0f, std::round(context.density * 0.5f));
        context.list.fill(gfx::Rect{b.x, b.bottom() - hair, b.width, hair}, th.colors.ink.withAlpha(th.colors.divider));
    });
    row->bind([raw, pressable] {
        (void)pressable->pressed();
        untrack([&] { raw->markPaintDirty(); });
    });
    row->updateSemantics([&](Semantics &s) {
        s.role = Role::button;
        s.label = label + ", " + std::to_string(static_cast<int>(std::lround(used * 100))) + "% used" +
                  (space.empty() ? std::string() : ", " + space);
    });
    return row;
}

NodePtr makePermissionCard(AppContext &context) {
    const Theme &t = currentTheme();
    auto card = Glass::create({.alignItems = Align::start, .gap = 12, .padding = Edges::all(20)},
                              {.radii = gfx::CornerRadii::uniform(t.radii.panel),
                               .cornerTicks = true});
    card->appendChild(Stamp("Access needed", StampTone::secondary, "lock"));
    auto title = Text::create("Allow all files", t.type.title, t.colors.ink);
    title->updateSemantics([](Semantics &s) { s.role = Role::heading; });
    card->appendChild(title);
    auto body = Text::create("A file manager has to see every folder on your storage, not just media. Android "
                             "asks for that as all files access; FilePan uses it only to show and change the "
                             "files you choose.",
                             t.type.bodySmall, ink(t, t.text.body));
    card->appendChild(body);
    runtime::Platform *platform = &context.services.platform;
    card->appendChild(Button::create({.label = "Grant access",
                                      .variant = ButtonVariant::primary,
                                      .icon = "folder_open",
                                      .onTap = [platform] { platform->requestAllFilesAccess(); },
                                      .accessibilityLabel = "Grant all files access"}));
    return card;
}

}  // namespace

std::shared_ptr<HomeScreen> HomeScreen::create(AppContext &context) {
    auto node = std::make_shared<HomeScreen>(context);
    node->construct();
    return node;
}

void HomeScreen::construct() {
    setDebugName("HomeScreen");
    setStyle({.position = Position::absolute, .left = 0, .top = 0, .right = 0, .bottom = 0});
    scroll_ = ScrollNode::create();
    scroll_->setStyle({.position = Position::absolute, .left = 0, .top = 0, .right = 0, .bottom = 0});
    column_ = Box::create({.gap = 10, .padding = Edges::symmetric(16, 0)});
    scroll_->appendChild(column_);
    appendChild(scroll_);
    HomeScreen *self = this;
    this->build([&] {
        bind([self] {
            const float density = environment().density();
            const float bottom = environment().safeArea().bottom / density;
            const float top = self->context_.barHeight();
            untrack([&] { self->column_->updateStyle([&](Style &s) { s.padding = {16, top + 4, 16, bottom + 32}; }); });
        });
        bind([self] {
            (void)self->context_.volumes();
            (void)self->context_.services.allFilesAccess();
            (void)self->context_.state.settings().favorites;
            (void)self->context_.missingFavorites();
            (void)self->context_.state.settings().servers;
            (void)self->context_.state.settings().documentTrees;
            (void)self->context_.state.settings().trees;
            (void)theme();
            untrack([&] { self->rebuild(); });
        });
    });
}

void HomeScreen::rebuild() {
    column_->clearChildren();
    volumes_.clear();
    favorites_.clear();
    permission_ = nullptr;
    servers_ = nullptr;
    const Theme &t = currentTheme();
    column_->updateStyle([&](Style &s) { s.gap = t.style.utilitarian ? 0 : 10; });

    if (!context_.services.allFilesAccess.peek()) {
        auto card = makePermissionCard(context_);
        permission_ = card.get();
        column_->appendChild(card);
    }

    column_->appendChild(section("Storage"));
    for (const auto &volume : context_.volumes.peek()) {
        if (volume.path.empty()) {
            continue;
        }
        auto card = t.style.utilitarian ? volumeRow(context_, volume) : volumeCard(context_, volume);
        volumes_.push_back(card.get());
        column_->appendChild(card);
    }
    if (documentTreesAvailable(context_)) {
        // Experimental: folders granted through the system's picker.
        auto treeRows = Box::create({.margin = {-16, 0, -16, 0}});
        AppContext *context = &context_;
        for (const DocumentTree &tree : context_.state.settings.peek().trees) {
            const std::string id = tree.id;
            auto row = FileRow::create({.onTap = [context, id] { context->state.navigate(vfs::Location::document(id)); },
                                        .onLongPress = [context, id] { removeDocumentTree(*context, id); },
                                        .customActions = {{1, "Remove storage"}},
                                        .onCustomAction = [context, id](std::uint32_t) {
                                            removeDocumentTree(*context, id);
                                        }});
            row->setData({.name = id,
                          .meta = "System storage · long-press to remove",
                          .icon = "folder_shared",
                          .kindColor = t.colors.highlight,
                          .spokenLabel = id + ", system storage"});
            treeRows->appendChild(row);
        }
        auto add = FileRow::create({.onTap = [context] { addDocumentTree(*context); }});
        add->setData({.name = "Add storage…",
                      .meta = "A folder from the system's picker · experimental",
                      .icon = "create_new_folder",
                      .kindColor = t.colors.highlight,
                      .spokenLabel = "Add storage, a folder from the system's picker, experimental"});
        treeRows->appendChild(add);
        column_->appendChild(treeRows);
    }

    column_->appendChild(section("Favorites"));
    const auto &favorites = context_.state.settings.peek().favorites;
    auto rows = Box::create({.margin = {-16, 0, -16, 0}});
    for (std::size_t i = 0; i < favorites.size(); ++i) {
        const Favorite &favorite = favorites[i];
        const std::string path = favoritePath(context_, favorite);
        const bool missing = favoriteMissing(context_, favorite);
        AppContext *context = &context_;
        auto row = FileRow::create({.onTap = [context, favorite] { openFavorite(*context, favorite); },
                                    .onLongPress = [context, i] { removeFavoriteAt(*context, i); },
                                    .customActions = {{1, "Remove from favorites"}},
                                    .onCustomAction = [context, i](std::uint32_t) { removeFavoriteAt(*context, i); }});
        const bool file = favorite.kind == Favorite::Kind::file;
        row->setData({.name = favorite.name,
                      .meta = missing ? "Unavailable · " + path : path,
                      .icon = missing ? "link_off" : favoriteIcon(favorite),
                      .kindColor = missing ? ink(t, 0.5f) : file ? t.colors.secondary : t.colors.highlight,
                      .spokenLabel = favorite.name + ", favorite " + favoriteKind(favorite) + ", " + path +
                                     (missing ? ", unavailable" : "")});
        favorites_.push_back(row.get());
        rows->appendChild(row);
    }
    if (favorites.empty()) {
        auto hint = Text::create("Star a folder from the dock, or a file from its menu. Long-press one to remove it.",
                                 t.type.bodySmall, ink(t, t.text.label));
        hint->updateStyle([](Style &s) { s.margin = {20, 2, 16, 4}; });
        rows->appendChild(hint);
    }
    column_->appendChild(rows);

    column_->appendChild(section("Servers"));
    auto serverRows = Box::create({.margin = {-16, 0, -16, 0}});
    const auto &saved = context_.state.settings.peek().servers;
    serverRows_.clear();
    for (const auto &server : saved) {
        AppContext *context = &context_;
        const std::string id = server.id;
        const std::string name = server.name.empty() ? server.server.host : server.name;
        auto row = FileRow::create({.onTap = [context, id] { openServer(*context, id); }});
        const std::string line = endpointLine(server.server);
        row->setData({.name = name,
                      .meta = line,
                      .icon = "dns",
                      .kindColor = t.colors.tertiary,
                      .spokenLabel = name + ", SFTP server, " + line});
        serverRows_.push_back(row.get());
        serverRows->appendChild(row);
    }
    const std::size_t count = saved.size();
    auto servers = FileRow::create({.onTap = [this] { openServers(); }});
    const std::string meta = count == 0 ? std::string("None saved yet") : "Add, edit, delete";
    servers->setData({.name = count == 0 ? "SFTP servers" : "Manage servers",
                      .meta = meta,
                      .icon = count == 0 ? "dns" : "settings_ethernet",
                      .kindColor = t.colors.tertiary,
                      .spokenLabel = "SFTP servers, " + meta});
    servers_ = servers.get();
    serverRows->appendChild(servers);
    column_->appendChild(serverRows);
}

void HomeScreen::openServers() {
    if (context_.screens.find(kServersScreen) != nullptr) {
        context_.state.openScreen(kServersScreen);
    } else {
        context_.notify("Not yet");
    }
}

}  // namespace filepan::app
