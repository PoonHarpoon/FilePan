// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "filepan/gfx/display_list.hpp"
#include "filepan/ui/components/common.hpp"
#include "filepan/ui/node.hpp"
#include "filepan/ui/semantics.hpp"

// File list rows and grid tiles, built once per recycled VirtualList node and rebound with
// setData. Selection never relies on color alone: in selection mode every item shows a check
// box, filled and ticked when selected.
namespace filepan::ui::components {

enum class FileKind : std::uint8_t { folder, image, video, audio, document, pdf, archive, apk, code, other };

// The tint and Material Symbols icon of a kind. Folders take the theme's highlight; files get a
// fixed palette tuned to read on every preset's ground.
gfx::Color kindColor(FileKind kind, const Theme &theme);
const char *kindIcon(FileKind kind);

// What a row shows; the screen maps vfs entries to this.
struct FileItem {
    std::string name;
    std::string meta;             // Body face, sentence case: `2.4 MB · 12 Oct`.
    // The utilitarian row's columns: the size (or a folder's item count) and the date.
    std::string size;
    std::string date;
    std::string icon = "draft";   // Material Symbols ligature for the kind.
    gfx::Color kindColor{1, 1, 1, 1};
    gfx::ImageId thumbnail = 0;   // Replaces the icon when set.
    bool video = false;           // Marks the thumbnail with a small play glyph.
    // The thumbnail is an icon with its own shape and transparent surround (an APK's app icon),
    // not a picture: drawn whole, with no backdrop, frame or rounding of its own.
    bool iconThumbnail = false;
    bool selected = false;
    // Selection mode: unselected rows show an empty check box.
    bool selecting = false;
    // Briefly tinted like a selected row, without selecting it: the file a favorite led to.
    bool flash = false;
    // A hidden item that's shown (a dot file, or one the user hid): its tile at half strength and
    // its name at the meta line's ink.
    bool dimmed = false;
    // What a screen reader says instead of the visible name and meta line, e.g. "notes.md,
    // Document, 18 KB, modified 8 Oct 2026". Empty reads the visible text.
    std::string spokenLabel;

    bool operator==(const FileItem &) const = default;
};

struct FileRowCallbacks {
    std::function<void()> onTap;
    std::function<void()> onLongPress;
    // Accessibility actions beyond click and long click ("Rename", "Delete").
    std::vector<CustomAction> customActions;
    std::function<void(std::uint32_t id)> onCustomAction;
};

// A 60 dp row: a 40 dp icon tile (radius 10, an 11% wash of the kind color behind an outlined
// glyph, or the thumbnail, a video's with a small play mark at its center, an icon thumbnail
// filling the tile as it is), the name in body type
// with a middle ellipsis so the extension stays visible, and a quiet 12 sp meta line. In
// selection mode a check box trails the row; selected rows also get a 13% accent tint (radius
// 14), with no border. In semantics it's one list item with the selected state and its actions.
// The utilitarian style lays it out as one line instead: a bare kind glyph, the name, and the
// size and date in right-aligned mono columns, over a hairline.
class FileRow final : public Node {
public:
    static std::shared_ptr<FileRow> create(FileRowCallbacks callbacks = {});

    void setData(const FileItem &item);
    [[nodiscard]] const FileItem &data() const { return item_.peek(); }

protected:
    void paint(PaintContext &context) override;
    void paintOverlay(PaintContext &context) override;

private:
    Signal<FileItem> item_;
    std::shared_ptr<Pressable> pressable_;
    Node *trailing_ = nullptr;
};

// A grid cell: a square thumbnail (radius 14; a video's has a small play mark in its lower left
// corner) or kind-tinted tile, an icon thumbnail centered on that tile at about half its size,
// with the name below.
class GridTile final : public Node {
public:
    static std::shared_ptr<GridTile> create(FileRowCallbacks callbacks = {});

    void setData(const FileItem &item);
    [[nodiscard]] const FileItem &data() const { return item_.peek(); }

protected:
    void paintOverlay(PaintContext &context) override;

private:
    Signal<FileItem> item_;
    std::shared_ptr<Pressable> pressable_;
    std::shared_ptr<Node> tile_;
};

// The check box of an item in selection mode (24 dp, radius 6): accent and ticked when selected,
// an empty outlined box otherwise, into rect (px).
void paintSelectionStamp(gfx::DisplayList &list, const gfx::Rect &rect, bool selected, const Theme &theme,
                         float density);

}  // namespace filepan::ui::components
