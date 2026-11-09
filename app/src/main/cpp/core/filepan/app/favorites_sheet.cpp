// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/favorites_sheet.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "filepan/app/format.hpp"
#include "filepan/app/places.hpp"
#include "filepan/app/screen_parts.hpp"
#include "filepan/app/sheet_parts.hpp"
#include "filepan/ui/gesture.hpp"

namespace filepan::app {

namespace {

using namespace ui;
using namespace ui::components;

std::string inQuotes(std::string_view name) { return "“" + std::string(name) + "”"; }

enum ActionId : std::uint32_t { kRename = 1, kRemove, kMoveUp, kMoveDown };

void dismissTopSheet(AppContext &context) {
    if (BottomSheet *sheet = context.overlays.topSheet()) {
        sheet->dismiss();
    }
}

// What the sheet shares between its binds and callbacks.
struct FavoritesSheet {
    AppContext *context = nullptr;
    // The folder the sheet was opened on, if it was opened while browsing.
    std::optional<vfs::Location> here;
    Signal<bool> editing{false};
    // The row being dragged by its handle, and how far, dp.
    std::optional<std::size_t> dragging;
    float dragOffset = 0;
};

NodePtr favoriteRow(const std::shared_ptr<FavoritesSheet> &sheet, std::size_t index, const Favorite &favorite,
                    std::size_t count, bool editing) {
    AppContext &context = *sheet->context;
    const Theme &t = currentTheme();
    const bool missing = favoriteMissing(context, favorite);
    const std::string path = favoritePath(context, favorite);
    auto row = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 12,
                            .padding = {editing ? 0.0f : 12.0f, 0, 4, 0}, .height = kFavoriteRowHeight});
    row->setDebugName("FavoriteRow");
    Node *raw = row.get();

    if (editing) {
        // The handle: a drag along it moves the row; it settles in the slot it's let go over.
        auto handle = Box::create({.justify = Justify::center, .alignItems = Align::center, .width = 44,
                                   .height = kFavoriteRowHeight});
        handle->setDebugName("DragHandle");
        handle->appendChild(Icon::create("drag_indicator", 22, ink(t, 0.5f)));
        handle->updateSemantics([](Semantics &s) { s.hidden = true; });
        auto drag = std::make_shared<DragRecognizer>(DragAxis::vertical);
        drag->onStart = [sheet, index, raw](gfx::Vec2) {
            sheet->dragging = index;
            sheet->dragOffset = 0;
            raw->markPaintDirty();
        };
        drag->onUpdate = [sheet, raw, index, count](gfx::Vec2 delta) {
            const float step = kFavoriteRowHeight + 2;
            const float lowest = -static_cast<float>(index) * step;
            const float highest = static_cast<float>(count - 1 - index) * step;
            sheet->dragOffset =
                    std::clamp(sheet->dragOffset + delta.y / environment().density.peek(), lowest, highest);
            raw->setTransform(gfx::Transform::translation({0, sheet->dragOffset}));
        };
        const auto finish = [sheet, raw, index, count] {
            const float step = kFavoriteRowHeight + 2;
            const auto moved = static_cast<long>(std::lround(sheet->dragOffset / step));
            const auto target = static_cast<std::size_t>(
                    std::clamp(static_cast<long>(index) + moved, 0L, static_cast<long>(count) - 1));
            sheet->dragging.reset();
            sheet->dragOffset = 0;
            raw->setTransform({});
            raw->markPaintDirty();
            if (target != index) {
                haptic(Haptic::tick);
                sheet->context->state.moveFavorite(index, target);
            }
        };
        drag->onEnd = [finish](gfx::Vec2) { finish(); };
        drag->onCancel = finish;
        handle->addGestureRecognizer(drag);
        row->appendChild(handle);
    } else {
        // Files in the secondary color, folders in the highlight, as the prototype has them;
        // faded when the target is gone.
        const gfx::Color kind = favorite.kind == Favorite::Kind::file ? t.colors.secondary : t.colors.highlight;
        row->appendChild(Icon::create(missing ? "link_off" : favoriteIcon(favorite), 22,
                                      missing ? ink(t, 0.5f) : legible(kind, t.colors.ground, 3)));
    }

    auto texts = Box::create({.gap = 2, .grow = 1, .shrink = 1, .basis = 0});
    auto name = Text::create(favorite.name, t.type.body, ink(t, missing ? t.text.meta : t.text.primary));
    name->setMaxLines(1);
    name->setOverflow(gfx::TextOverflow::ellipsisMiddle);
    auto line = Text::create(missing ? "Unavailable · " + path : path, monoText(t, 11, 500), ink(t, t.text.meta));
    line->setMaxLines(1);
    line->setOverflow(gfx::TextOverflow::ellipsisStart);
    for (Node *n : {static_cast<Node *>(name.get()), static_cast<Node *>(line.get())}) {
        n->updateSemantics([](Semantics &s) { s.hidden = true; });
    }
    texts->appendChild(name);
    texts->appendChild(line);
    row->appendChild(texts);

    AppContext *ctx = &context;
    const vfs::Location location = favorite.location;
    if (editing) {
        row->appendChild(IconButton::create({.icon = "edit",
                                             .label = "Rename " + favorite.name,
                                             .style = IconButtonStyle::plain,
                                             .size = 44,
                                             .onTap = [ctx, index] { showRenameFavorite(*ctx, index); }}));
        row->appendChild(IconButton::create({.icon = "close",
                                             .label = "Remove " + favorite.name,
                                             .style = IconButtonStyle::plain,
                                             .size = 44,
                                             .destructive = true,
                                             .onTap = [ctx, index] { removeFavoriteAt(*ctx, index); }}));
    } else {
        row->appendChild(Icon::create("chevron_right", 20, ink(t, 0.4f)));
    }

    std::shared_ptr<Pressable> pressable;
    if (!editing) {
        pressable = makePressable(*row, {.onTap = [ctx, location] {
                                             const auto &favorites = ctx->state.settings.peek().favorites;
                                             if (auto at = findFavorite(favorites, location)) {
                                                 const Favorite chosen = favorites[*at];
                                                 dismissTopSheet(*ctx);
                                                 openFavorite(*ctx, chosen);
                                             }
                                         },
                                         .haptic = Haptic::tick});
    }
    row->setPaintHook([pressable, sheet, index](PaintContext &paint) {
        const Theme &th = currentTheme();
        const gfx::RoundedRect shape{paint.bounds(), gfx::CornerRadii::uniform(paint.px(th.corner(16)))};
        if (sheet->dragging == index) {
            paint.list.fill(shape, th.colors.ink.withAlpha(0.08f));
        }
        if (pressable && pressable->pressed.peek()) {
            paint.list.fill(shape, th.colors.ink.withAlpha(th.colors.pressed * 0.6f));
        }
    });
    if (pressable) {
        row->bind([raw, pressable] {
            (void)pressable->pressed();
            untrack([&] { raw->markPaintDirty(); });
        });
    }

    std::vector<CustomAction> actions{{kRename, "Rename"}, {kRemove, "Remove from favorites"}};
    if (index > 0) {
        actions.push_back({kMoveUp, "Move up"});
    }
    if (index + 1 < count) {
        actions.push_back({kMoveDown, "Move down"});
    }
    row->updateSemantics([&](Semantics &s) {
        s.role = editing ? Role::listItem : Role::button;
        s.label = favorite.name + ", " + favoriteKind(favorite) + ", " + path + (missing ? ", unavailable" : "");
        s.customActions = actions;
        s.onAction = [ctx, index](Action action, const ActionArgs &args) {
            if (action != Action::custom) {
                return false;
            }
            switch (args.customId) {
            case kRename:
                showRenameFavorite(*ctx, index);
                return true;
            case kRemove:
                removeFavoriteAt(*ctx, index);
                return true;
            case kMoveUp:
                ctx->state.moveFavorite(index, index - 1);
                return true;
            case kMoveDown:
                ctx->state.moveFavorite(index, index + 1);
                return true;
            default:
                return false;
            }
        };
    });
    return row;
}

}  // namespace

std::string favoriteKind(const Favorite &favorite) {
    const bool remote = favorite.location.kind() == vfs::BackendKind::kSftp;
    if (favorite.kind == Favorite::Kind::file) {
        return remote ? "SFTP file" : "file";
    }
    return remote ? "SFTP folder" : "folder";
}

const char *favoriteIcon(const Favorite &favorite) {
    if (favorite.kind == Favorite::Kind::file) {
        return "description";
    }
    return favorite.location.kind() == vfs::BackendKind::kSftp ? "lan" : "folder";
}

std::string favoritePath(const AppContext &context, const Favorite &favorite) {
    return locationPath(context, favorite.location);
}

std::string locationPath(const AppContext &context, const vfs::Location &location) {
    std::vector<Crumb> crumbs = crumbsFor(location, context.volumes.peek());
    std::string prefix;
    if (location.kind() == vfs::BackendKind::kSftp && !crumbs.empty()) {
        // The server's own name, not its id.
        for (const auto &server : context.state.settings.peek().servers) {
            if (server.id == location.serverId()) {
                crumbs.front().label = server.name.empty() ? server.server.host : server.name;
            }
        }
        prefix = "SFTP · ";
    }
    std::string path;
    for (const Crumb &crumb : crumbs) {
        path += (path.empty() ? "" : " / ") + crumb.label;
    }
    return prefix + path;
}

bool favoriteMissing(const AppContext &context, const Favorite &favorite) {
    return std::ranges::find(context.missingFavorites.peek(), favorite.location) != context.missingFavorites.peek().end();
}

void openFavorite(AppContext &context, const Favorite &favorite) {
    if (favoriteMissing(context, favorite)) {
        AppState *state = &context.state;
        const vfs::Location location = favorite.location;
        context.overlays.showSnackbar({.text = inQuotes(favorite.name) + " isn't there any more",
                                       .action = "Remove",
                                       .onAction = [state, location] { state->removeFavorite(location); }});
        return;
    }
    if (favorite.kind == Favorite::Kind::file && context.reveal) {
        context.reveal(favorite.location);
        return;
    }
    context.state.navigate(favorite.location);
}

void openHome(AppContext &context) {
    if (const Favorite *favorite = homeFavorite(context.state.settings.peek())) {
        openFavorite(context, Favorite(*favorite));
        return;
    }
    context.state.navigateHome();
}

void removeFavoriteAt(AppContext &context, std::size_t index) {
    const auto &favorites = context.state.settings.peek().favorites;
    if (index >= favorites.size()) {
        return;
    }
    const Favorite removed = favorites[index];
    context.state.removeFavorite(removed.location);
    AppState *state = &context.state;
    context.overlays.showSnackbar({.text = "Removed " + inQuotes(removed.name) + " from favorites",
                                   .action = "Undo",
                                   .onAction = [state, removed, index] { state->addFavorite(removed, index); }});
}

void showRenameFavorite(AppContext &context, std::size_t index) {
    const auto &favorites = context.state.settings.peek().favorites;
    if (index >= favorites.size()) {
        return;
    }
    const vfs::Location location = favorites[index].location;
    auto name = std::make_shared<std::string>(favorites[index].name);
    AppContext *ctx = &context;
    const auto submit = [ctx, location, name] {
        if (name->empty()) {
            return;
        }
        if (auto at = findFavorite(ctx->state.settings.peek().favorites, location)) {
            ctx->state.renameFavorite(*at, *name);
        }
        dismissTopSheet(*ctx);
    };
    auto column = parts::sheetColumn(10);
    column->setDebugName("RenameFavoriteSheet");
    auto error = std::make_shared<Signal<std::string>>();
    auto field = TextField::create({.label = "Name in favorites",
                                    .text = *name,
                                    .icon = "star",
                                    .helper = "Only here; the file keeps its own name",
                                    .error = [error] { return (*error)(); },
                                    .config = {.action = ImeAction::done},
                                    .onChange = [name, error](const std::string &text) {
                                        *name = text;
                                        error->set(text.empty() ? "Enter a name" : "");
                                    },
                                    .onSubmit = submit});
    column->appendChild(field);
    column->appendChild(parts::buttonRow(
            {Button::create({.label = "Cancel", .variant = ButtonVariant::glass, .onTap = [ctx] { dismissTopSheet(*ctx); }}),
             Button::create({.label = "Rename",
                             .variant = ButtonVariant::primary,
                             .enabled = [error] { return (*error)().empty(); },
                             .onTap = submit})}));
    TextField *raw = field.get();
    context.overlays.showSheet("Rename favorite", parts::scrolling(column), true, [raw] { raw->editor().clearFocus(); });
    raw->editor().requestFocus();
}

void toggleFavoriteFolder(AppContext &context, const vfs::Location &folder) {
    const std::string name = locationLabel(folder, context.volumes.peek());
    if (context.state.isFavorite(folder)) {
        context.state.removeFavorite(folder);
        context.notify("Removed " + inQuotes(name) + " from favorites");
    } else {
        context.state.addFavorite({name, folder, Favorite::Kind::folder});
        context.notify("Added " + inQuotes(name) + " to favorites");
    }
}

bool allFavorites(const AppContext &context, const FileTargets &targets) {
    const auto &favorites = context.state.settings.peek().favorites;
    return !targets.entries.empty() && std::ranges::all_of(targets.entries, [&](const vfs::Entry &entry) {
        return findFavorite(favorites, targets.directory.child(entry.name)).has_value();
    });
}

void setFavorites(AppContext &context, const FileTargets &targets, bool on) {
    if (targets.entries.empty()) {
        return;
    }
    ui::batch([&] {
        for (const vfs::Entry &entry : targets.entries) {
            const vfs::Location location = targets.directory.child(entry.name);
            if (on) {
                context.state.addFavorite({entry.name, location,
                                           entry.isDirectoryLike() ? Favorite::Kind::folder : Favorite::Kind::file});
            } else {
                context.state.removeFavorite(location);
            }
        }
        context.state.clearSelection();
    });
    const std::string what = targets.entries.size() == 1 ? inQuotes(targets.entries.front().name)
                                                         : formatCount(targets.entries.size());
    context.notify(on ? "Added " + what + " to favorites" : "Removed " + what + " from favorites");
}

void showFavorites(AppContext &context) {
    auto sheet = std::make_shared<FavoritesSheet>();
    sheet->context = &context;
    if (const Route &route = context.state.peekActive()->current(); route.kind == Route::Kind::browse) {
        sheet->here = route.location;
    }
    const Theme &t = currentTheme();
    AppContext *ctx = &context;

    // The header's edit toggle, only while there's something to edit.
    auto toggle = Box::create({.direction = Direction::row});
    auto edit = IconButton::create({.icon = "edit",
                                    .label = "Edit favorites",
                                    .style = IconButtonStyle::plain,
                                    .size = 44,
                                    .onTap = [sheet] { sheet->editing.set(true); }});
    auto done = IconButton::create({.icon = "check",
                                    .label = "Done editing",
                                    .style = IconButtonStyle::plain,
                                    .size = 44,
                                    .onTap = [sheet] { sheet->editing.set(false); }});
    toggle->appendChild(edit);
    toggle->appendChild(done);
    toggle->bind([sheet, ctx, toggle = toggle.get(), edit = edit.get(), done = done.get()] {
        const bool any = !ctx->state.settings().favorites.empty();
        const bool editing = sheet->editing();
        untrack([&] {
            if (!any && editing) {
                sheet->editing.set(false);
            }
            toggle->updateStyle([&](Style &s) { s.display = any; });
            edit->updateStyle([&](Style &s) { s.display = !editing; });
            done->updateStyle([&](Style &s) { s.display = editing; });
        });
    });

    auto content = Box::create({.gap = 4, .padding = {16, 0, 16, 8}, .shrink = 1});
    content->setDebugName("FavoritesSheet");
    auto rows = Box::create({.gap = 2});
    rows->updateSemantics([](Semantics &s) { s.role = Role::list; });
    content->appendChild(parts::scrolling(rows));
    auto empty = Text::create("No favorites yet. Star a folder here, or a file from its menu.", t.type.bodySmall,
                              ink(t, t.text.label));
    empty->setStyle({.margin = {8, 4, 8, 12}});
    content->appendChild(empty);
    rows->bind([sheet, ctx, r = rows.get(), e = empty.get()] {
        const std::vector<Favorite> favorites = ctx->state.settings().favorites;
        const bool editing = sheet->editing();
        (void)ctx->missingFavorites();
        (void)ctx->volumes();
        untrack([&] {
            r->clearChildren();
            for (std::size_t i = 0; i < favorites.size(); ++i) {
                r->appendChild(favoriteRow(sheet, i, favorites[i], favorites.size(), editing));
            }
            e->updateStyle([&](Style &s) { s.display = favorites.empty(); });
        });
    });

    // The current folder: add it, or take it off.
    if (sheet->here) {
        const vfs::Location here = *sheet->here;
        const std::string name = locationLabel(here, context.volumes.peek());
        auto add = Button::create({.label = "Add " + inQuotes(name),
                                   .variant = ButtonVariant::primary,
                                   .icon = "star",
                                   .onTap = [ctx, here] { toggleFavoriteFolder(*ctx, here); },
                                   .accessibilityLabel = "Add " + name + " to favorites"});
        auto remove = Button::create({.label = "Remove " + inQuotes(name),
                                      .variant = ButtonVariant::glass,
                                      .icon = "star",
                                      .onTap = [ctx, here] { toggleFavoriteFolder(*ctx, here); },
                                      .accessibilityLabel = "Remove " + name + " from favorites"});
        for (Node *b : {static_cast<Node *>(add.get()), static_cast<Node *>(remove.get())}) {
            b->updateStyle([](Style &s) { s.margin = {0, 10, 0, 0}; });
        }
        content->bind([sheet, ctx, here, a = add.get(), rm = remove.get()] {
            const bool on = ctx->state.isFavorite(here);
            const bool editing = sheet->editing();
            untrack([&] {
                a->updateStyle([&](Style &s) { s.display = !on && !editing; });
                rm->updateStyle([&](Style &s) { s.display = on && !editing; });
            });
        });
        content->appendChild(add);
        content->appendChild(remove);
    }
    context.overlays.showSheet("Favorites", content, true, {}, toggle);
}

}  // namespace filepan::app
