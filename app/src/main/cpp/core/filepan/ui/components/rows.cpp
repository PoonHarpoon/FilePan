// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/components/rows.hpp"

#include <algorithm>
#include <cmath>

#include "filepan/ui/components/recipes.hpp"
#include "filepan/ui/components/surfaces.hpp"

namespace filepan::ui::components {

namespace {

constexpr float kStamp = 24;
// A shown hidden item's icon tile or thumbnail.
constexpr float kDimmedTile = 0.5f;
// An icon thumbnail's side in a grid tile, in percent of the tile's.
constexpr float kTileIconPercent = 52;

void line(gfx::DisplayList &list, gfx::Vec2 a, gfx::Vec2 b, float width, gfx::Color color) {
    const gfx::Vec2 d = b - a;
    const float length = std::sqrt(d.x * d.x + d.y * d.y);
    const float c = d.x / length;
    const float s = d.y / length;
    list.pushTransform(gfx::Transform::translation(a) * gfx::Transform{c, s, -s, c, 0, 0});
    list.fill(gfx::RoundedRect{{-width / 2, -width / 2, length + width, width}, gfx::CornerRadii::uniform(width / 2)},
              color);
    list.pop();
}

// The kind tile behind a row's or tile's icon: a flat wash of the kind color.
void kindTile(gfx::DisplayList &list, const gfx::RoundedRect &shape, gfx::Color kind) {
    list.fill(shape, kind.withAlpha(0.11f));
}

// The outlined glyph: the kind color softened toward the ink.
gfx::Color kindInk(const Theme &theme, gfx::Color kind) {
    return legible(mix(kind.withAlpha(1), theme.colors.ink, 0.28f), theme.colors.ground, 3);
}

// The mark on a video's thumbnail: a small dark disc with a filled play glyph, quiet enough to
// leave the picture readable. Hidden until the item is a video with a thumbnail.
std::shared_ptr<Box> playBadge(float disc, float glyph, Style placement) {
    placement.display = false;
    placement.justify = Justify::center;
    placement.alignItems = Align::center;
    placement.width = disc;
    placement.height = disc;
    auto badge = Box::create(placement, {.background = gfx::Color{0, 0, 0, 0.42f},
                                         .radii = gfx::CornerRadii::uniform(disc / 2)});
    badge->appendChild(Icon::create("play_arrow", glyph, gfx::Color{1, 1, 1, 0.92f}, true));
    badge->updateSemantics([](Semantics &s) { s.hidden = true; });
    return badge;
}

void applySemantics(Node &node, const FileItem &item, const FileRowCallbacks &callbacks) {
    // A spoken label replaces the merged text, which the caller hides (hideVisibleText).
    node.updateSemantics([&](Semantics &s) {
        s.role = Role::listItem;
        s.mergeDescendants = true;
        s.label = item.spokenLabel;
        s.flags = static_cast<std::uint16_t>(item.selecting || item.selected ? semantics_flags::kCheckable : 0) |
                  static_cast<std::uint16_t>(item.selected ? semantics_flags::kSelected | semantics_flags::kChecked : 0);
        s.customActions = callbacks.customActions;
        if (callbacks.onCustomAction) {
            s.onAction = [handler = callbacks.onCustomAction](Action action, const ActionArgs &args) {
                if (action == Action::custom) {
                    handler(args.customId);
                    return true;
                }
                return false;
            };
        }
    });
}

}  // namespace

gfx::Color kindColor(FileKind kind, const Theme &theme) {
    switch (kind) {
    case FileKind::folder:
        return theme.colors.highlight;
    case FileKind::image:
        return gfx::Color::fromRgb(0x4ADE80);
    case FileKind::video:
        return gfx::Color::fromRgb(0xB78BFA);
    case FileKind::audio:
        return gfx::Color::fromRgb(0xFFB547);
    case FileKind::document:
        return gfx::Color::fromRgb(0x60A5FA);
    case FileKind::pdf:
        return gfx::Color::fromRgb(0xFF6B81);
    case FileKind::archive:
        return gfx::Color::fromRgb(0xF5C04A);
    case FileKind::apk:
        return gfx::Color::fromRgb(0x34D399);
    case FileKind::code:
        return gfx::Color::fromRgb(0x5EEAD4);
    case FileKind::other:
        break;
    }
    return gfx::Color::fromRgb(0xB8B4C8);
}

const char *kindIcon(FileKind kind) {
    switch (kind) {
    case FileKind::folder:
        return "folder";
    case FileKind::image:
        return "image";
    case FileKind::video:
        return "movie";
    case FileKind::audio:
        return "music_note";
    case FileKind::document:
        return "description";
    case FileKind::pdf:
        return "picture_as_pdf";
    case FileKind::archive:
        return "folder_zip";
    case FileKind::apk:
        return "android";
    case FileKind::code:
        return "code";
    case FileKind::other:
        break;
    }
    return "draft";
}

void paintSelectionStamp(gfx::DisplayList &list, const gfx::Rect &rect, bool selected, const Theme &theme,
                         float density) {
    const gfx::RoundedRect shape{rect, gfx::CornerRadii::uniform(theme.corner(6) * density)};
    if (selected) {
        list.fill(shape, theme.colors.accent);
        const float u = rect.width / 22;
        const gfx::Vec2 o = rect.origin();
        const float w = 2.6f * u;
        line(list, o + gfx::Vec2{6 * u, 11.5f * u}, o + gfx::Vec2{9.6f * u, 15 * u}, w, theme.colors.onAccent);
        line(list, o + gfx::Vec2{9.6f * u, 15 * u}, o + gfx::Vec2{16.2f * u, 7.8f * u}, w, theme.colors.onAccent);
    } else {
        list.fill(shape, gfx::Color{0, 0, 0, theme.colors.light ? theme.colors.wellAlpha : 0.25f});
        list.stroke(shape, 2 * density, theme.colors.ink.withAlpha(0.40f));
    }
}

// --- FileRow ---

std::shared_ptr<FileRow> FileRow::create(FileRowCallbacks callbacks) {
    auto node = std::make_shared<FileRow>();
    FileRow *raw = node.get();
    node->setDebugName("FileRow");
    const Theme &initial = currentTheme();

    auto tile = Box::create({.justify = Justify::center, .alignItems = Align::center});
    auto icon = Icon::create({}, 22);
    auto thumbnail = Image::create();
    thumbnail->setStyle({.display = false});
    constexpr float kRowBadge = 18;
    auto badge = playBadge(kRowBadge, 13, {.position = Position::absolute});
    tile->appendChild(icon);
    tile->appendChild(thumbnail);
    tile->appendChild(badge);
    tile->setPaintHook([raw](PaintContext &context) {
        const Theme &theme = currentTheme();
        const FileItem &item = raw->item_.peek();
        const gfx::RoundedRect shape{context.bounds(), gfx::CornerRadii::uniform(context.px(theme.radii.iconTile))};
        if (item.thumbnail == 0) {
            // The utilitarian row shows the bare glyph.
            if (!theme.style.utilitarian) {
                kindTile(context.list, shape, item.kindColor);
            }
        } else if (!item.iconThumbnail) {
            // Behind a picture's edges, so a translucent one doesn't show the row through.
            context.list.fill(shape, gfx::Color{0, 0, 0, 0.3f});
        }
    });
    tile->setOverlayHook([raw](PaintContext &context) {
        const FileItem &item = raw->item_.peek();
        if (item.thumbnail != 0 && !item.iconThumbnail) {
            const gfx::RoundedRect shape{context.bounds(),
                                         gfx::CornerRadii::uniform(context.px(currentTheme().radii.iconTile))};
            context.list.stroke(shape, std::max(1.0f, context.density * 0.75f),
                                currentTheme().colors.film.withAlpha(0.12f));
        }
    });
    node->appendChild(tile);

    auto texts = Box::create({.justify = Justify::center, .gap = 3, .grow = 1, .shrink = 1, .basis = 0});
    auto name = Text::create({}, initial.type.body);
    name->setMaxLines(1);
    name->setOverflow(gfx::TextOverflow::ellipsisMiddle);
    auto meta = Text::create({}, initial.type.meta);
    meta->setMaxLines(1);
    meta->setOverflow(gfx::TextOverflow::ellipsisEnd);
    texts->appendChild(name);
    texts->appendChild(meta);
    node->appendChild(texts);

    // The utilitarian columns.
    const gfx::TextStyle column{.family = gfx::FontFamily::mono, .size = 11.5f, .weight = 450, .lineHeight = 1.3f};
    auto size = Text::create({}, column);
    size->setMaxLines(1);
    size->setAlign(gfx::TextAlign::end);
    size->setStyle({.display = false, .width = 64});
    auto date = Text::create({}, column);
    date->setMaxLines(1);
    date->setAlign(gfx::TextAlign::end);
    date->setStyle({.display = false, .width = 76});
    for (Node *n : {static_cast<Node *>(size.get()), static_cast<Node *>(date.get())}) {
        n->updateSemantics([](Semantics &s) { s.hidden = true; });
    }
    node->appendChild(size);
    node->appendChild(date);

    auto trailing = Box::create({.display = false, .width = kStamp, .height = kStamp});
    trailing->updateSemantics([](Semantics &s) { s.hidden = true; });
    node->trailing_ = trailing.get();
    node->appendChild(trailing);

    node->pressable_ = makePressable(*node, {.onTap = callbacks.onTap,
                                             .onLongPress = callbacks.onLongPress,
                                             .haptic = Haptic::tap});
    // The metrics follow the theme, so a preset switch relays the rows already made.
    node->bind([raw, tile = tile.get(), icon = icon.get(), thumbnail = thumbnail.get(), badge = badge.get(),
                name = name.get()] {
        const Theme &th = theme();
        untrack([&] {
            const Spacing &sp = th.spacing;
            const bool dense = th.style.utilitarian;
            raw->setStyle({.direction = Direction::row,
                           .alignItems = Align::center,
                           .gap = sp.iconGap,
                           .padding = Edges::symmetric(sp.rowInset, 0),
                           .height = sp.listRow});
            tile->updateStyle([&](Style &s) {
                s.width = sp.iconTile;
                s.height = sp.iconTile;
            });
            icon->setSize(dense ? 20 : 22);
            thumbnail->updateStyle([&](Style &s) {
                s.width = sp.iconTile;
                s.height = sp.iconTile;
            });
            const float disc = dense ? 14 : kRowBadge;
            badge->updateStyle([&](Style &s) {
                s.left = (sp.iconTile - disc) / 2;
                s.top = (sp.iconTile - disc) / 2;
                s.width = disc;
                s.height = disc;
            });
            name->setTextStyle(th.type.body);
        });
    });
    node->bind([raw, tile = tile.get(), icon = icon.get(), thumbnail = thumbnail.get(), badge = badge.get(),
                name = name.get(), meta = meta.get(), size = size.get(), date = date.get(),
                trailing = trailing.get(), callbacks] {
        const FileItem &item = raw->item_();
        const Theme &th = theme();
        (void)raw->pressable_->pressed();
        (void)raw->trackFocused();
        untrack([&] {
            name->setText(item.name);
            name->setColor(ink(th, item.dimmed ? th.text.meta : th.text.primary));
            meta->setText(item.meta);
            meta->setColor(textInk(th, th.text.meta, th.type.meta.size));
            size->setText(item.size);
            size->setColor(textInk(th, th.text.label, 11.5f));
            date->setText(item.date);
            date->setColor(textInk(th, th.text.meta, 11.5f));
            // Utilitarian rows show the columns; a row with none (a favorite's path) keeps its meta line.
            const bool columns = th.style.utilitarian && !(item.size.empty() && item.date.empty());
            meta->updateStyle([&](Style &s) { s.display = !columns; });
            size->updateStyle([&](Style &s) { s.display = columns; });
            date->updateStyle([&](Style &s) { s.display = columns; });
            icon->setIcon(item.icon);
            icon->setColor(kindInk(th, item.kindColor));
            icon->setFilled(th.style.utilitarian && item.icon == kindIcon(FileKind::folder));
            icon->updateStyle([&](Style &s) { s.display = item.thumbnail == 0; });
            thumbnail->setImage(item.thumbnail);
            thumbnail->setRadii(item.iconThumbnail ? gfx::CornerRadii{} : gfx::CornerRadii::uniform(th.radii.iconTile));
            thumbnail->updateStyle([&](Style &s) { s.display = item.thumbnail != 0; });
            badge->updateStyle([&](Style &s) { s.display = item.video && item.thumbnail != 0; });
            trailing->updateStyle([&](Style &s) { s.display = item.selected || item.selecting; });
            tile->setOpacity(item.dimmed ? kDimmedTile : 1.0f);
            tile->markPaintDirty();
            raw->markPaintDirty();
            const bool spoken = !item.spokenLabel.empty();
            name->updateSemantics([&](Semantics &s) { s.hidden = spoken; });
            meta->updateSemantics([&](Semantics &s) { s.hidden = spoken; });
            applySemantics(*raw, item, callbacks);
        });
    });
    return node;
}

void FileRow::setData(const FileItem &item) { item_.set(item); }

namespace {

// Where a row's selection and press tint go: inset and rounded, or the whole utilitarian row.
gfx::RoundedRect rowHighlight(const PaintContext &context, const Theme &theme) {
    const gfx::Rect bounds = context.bounds();
    if (theme.style.utilitarian) {
        return {bounds, {}};
    }
    return {{context.px(8), context.px(2), bounds.width - context.px(16), bounds.height - context.px(4)},
            gfx::CornerRadii::uniform(context.px(theme.radii.listItem))};
}

}  // namespace

void FileRow::paint(PaintContext &context) {
    const Theme &theme = currentTheme();
    const FileItem &item = item_.peek();
    const gfx::RoundedRect highlight = rowHighlight(context, theme);
    if (item.selected || item.flash) {
        context.list.fill(highlight, theme.colors.accent.withAlpha(theme.colors.selected));
    }
    if (pressable_->pressed.peek()) {
        context.list.fill(highlight, theme.colors.ink.withAlpha(theme.colors.pressed * 0.6f));
    }
    if (theme.style.utilitarian) {
        // A hairline under the row, from where the name starts.
        const gfx::Rect bounds = context.bounds();
        const float t = std::max(1.0f, std::round(context.density * 0.5f));
        const float left = context.px(theme.spacing.rowInset + theme.spacing.iconTile + theme.spacing.iconGap);
        context.list.fill(gfx::Rect{left, bounds.height - t, bounds.width - left, t},
                          theme.colors.ink.withAlpha(theme.colors.divider));
    }
}

void FileRow::paintOverlay(PaintContext &context) {
    const Theme &theme = currentTheme();
    const FileItem &item = item_.peek();
    if (item.selected || item.selecting) {
        const gfx::Rect trailing = trailing_->frame();
        paintSelectionStamp(context.list,
                            {trailing.x * context.density, trailing.y * context.density, context.px(kStamp),
                             context.px(kStamp)},
                            item.selected, theme, context.density);
    }
    if (focused()) {
        recipes::focusRing(context.list, rowHighlight(context, theme), theme, context.density);
    }
}

// --- GridTile ---

std::shared_ptr<GridTile> GridTile::create(FileRowCallbacks callbacks) {
    auto node = std::make_shared<GridTile>();
    GridTile *raw = node.get();
    node->setDebugName("GridTile");
    const Theme &initial = currentTheme();
    node->setStyle({.alignItems = Align::stretch, .gap = 8, .padding = Edges::all(6)});
    auto tile = Box::create({.justify = Justify::center, .alignItems = Align::center, .aspectRatio = 1});
    tile->setClip(true, gfx::CornerRadii::uniform(initial.radii.tile));
    auto icon = Icon::create({}, 40);
    auto thumbnail = Image::create();
    thumbnail->setStyle({.display = false, .position = Position::absolute, .left = 0, .top = 0, .right = 0, .bottom = 0});
    // In the corner, so the picture stays whole.
    auto badge = playBadge(24, 17, {.position = Position::absolute, .left = 8, .bottom = 8});
    tile->appendChild(icon);
    tile->appendChild(thumbnail);
    tile->appendChild(badge);
    tile->setPaintHook([raw](PaintContext &context) {
        const Theme &theme = currentTheme();
        const FileItem &item = raw->item_.peek();
        const gfx::RoundedRect shape{context.bounds(), gfx::CornerRadii::uniform(context.px(theme.radii.tile))};
        if (item.thumbnail == 0 || item.iconThumbnail) {
            kindTile(context.list, shape, item.kindColor);
        }
    });
    node->tile_ = tile;
    node->appendChild(tile);
    auto name = Text::create({}, initial.type.bodySmall);
    name->setMaxLines(1);
    name->setOverflow(gfx::TextOverflow::ellipsisMiddle);
    name->setAlign(gfx::TextAlign::center);
    node->appendChild(name);
    node->pressable_ = makePressable(*node, {.onTap = callbacks.onTap, .onLongPress = callbacks.onLongPress});
    node->bind([raw, tile = tile.get(), icon = icon.get(), thumbnail = thumbnail.get(), badge = badge.get(),
                name = name.get(), callbacks] {
        const FileItem &item = raw->item_();
        const Theme &th = theme();
        (void)raw->pressable_->pressed();
        (void)raw->trackFocused();
        untrack([&] {
            name->setText(item.name);
            name->setColor(ink(th, item.selected ? 1.0f : item.dimmed ? th.text.meta : th.text.body));
            tile->setClip(true, gfx::CornerRadii::uniform(th.radii.tile));
            icon->setIcon(item.icon);
            icon->setColor(kindInk(th, item.kindColor));
            icon->setFilled(false);
            icon->updateStyle([&](Style &s) { s.display = item.thumbnail == 0; });
            thumbnail->setImage(item.thumbnail);
            thumbnail->updateStyle([&](Style &s) {
                s.display = item.thumbnail != 0;
                // A picture fills the tile (which clips it round); an icon sits in its middle.
                s.position = item.iconThumbnail ? Position::relative : Position::absolute;
                s.width = item.iconThumbnail ? Length::percent(kTileIconPercent) : Length::automatic();
                s.height = item.iconThumbnail ? Length::percent(kTileIconPercent) : Length::automatic();
            });
            tile->setOpacity(item.dimmed ? kDimmedTile : 1.0f);
            badge->updateStyle([&](Style &s) { s.display = item.video && item.thumbnail != 0; });
            tile->markPaintDirty();
            raw->markPaintDirty();
            const bool shrink = raw->pressable_->pressed.peek() && !th.style.utilitarian;
            raw->setTransform(shrink ? gfx::Transform::scale(0.97f, 0.97f) : gfx::Transform{});
            name->updateSemantics([&](Semantics &s) { s.hidden = !item.spokenLabel.empty(); });
            applySemantics(*raw, item, callbacks);
        });
    });
    return node;
}

void GridTile::setData(const FileItem &item) { item_.set(item); }

void GridTile::paintOverlay(PaintContext &context) {
    const Theme &theme = currentTheme();
    const FileItem &item = item_.peek();
    const gfx::Rect t = tile_->frame();
    const gfx::Rect tile{t.x * context.density, t.y * context.density, t.width * context.density,
                         t.height * context.density};
    const gfx::RoundedRect shape{tile, gfx::CornerRadii::uniform(context.px(theme.radii.tile))};
    if (item.selected || item.flash) {
        context.list.fill(shape, theme.colors.accent.withAlpha(theme.colors.selected * 2));
    }
    if (theme.style.utilitarian && pressable_->pressed.peek()) {
        context.list.fill(shape, theme.colors.ink.withAlpha(theme.colors.pressed));
    }
    if (item.selected || item.selecting) {
        paintSelectionStamp(context.list,
                            {tile.right() - context.px(kStamp + 8), tile.y + context.px(8), context.px(kStamp),
                             context.px(kStamp)},
                            item.selected, theme, context.density);
    }
    if (focused()) {
        recipes::focusRing(context.list, shape, theme, context.density);
    }
}

}  // namespace filepan::ui::components
