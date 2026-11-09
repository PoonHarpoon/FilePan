// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/adirstat/disk_usage_screen.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>
#include <utility>

#include "filepan/app/app_state.hpp"
#include "filepan/app/favorites_sheet.hpp"
#include "filepan/app/format.hpp"
#include "filepan/app/screen_parts.hpp"
#include "filepan/size_format.hpp"
#include "filepan/ui/components/components.hpp"
#include "filepan/ui/gesture.hpp"

namespace filepan::app {

namespace {

using namespace ui;
using namespace ui::components;
using adirstat::Category;
using adirstat::kNoNode;
using adirstat::NodeId;
using adirstat::NodeKind;

constexpr std::int64_t kPollNs = 150'000'000;
constexpr std::int64_t kDoubleTapNs = 300'000'000;
constexpr float kIndent = 14;
// A folder lists at most this many of its children; the rest are summed in its own line.
constexpr std::size_t kMaxChildren = 500;
constexpr std::size_t kMaxExtensions = 40;
// The treemap: the gap between tiles and the frame a folder draws around its children, in dp.
constexpr float kTileGap = 3;
constexpr float kFramePadding = 3;
// Tiles at least this big (dp) carry their name and size.
constexpr float kLabelWidth = 72;
constexpr float kLabelHeight = 36;

Style fill() { return {.position = Position::absolute, .left = 0, .top = 0, .right = 0, .bottom = 0}; }

bool isDirectory(const adirstat::Node &node) {
    return node.kind == NodeKind::kDirectory || node.kind == NodeKind::kMountPoint;
}

std::string percentOf(std::uint64_t part, std::uint64_t whole) {
    if (whole == 0) {
        return "0%";
    }
    const double percent = 100.0 * static_cast<double>(part) / static_cast<double>(whole);
    char text[16];
    std::snprintf(text, sizeof text, percent < 10 ? "%.1f%%" : "%.0f%%", percent);
    return text;
}

// The app's file kinds, so a category wears the same color and icon as in the browser.
FileKind kindOf(Category category) {
    switch (category) {
        case Category::kImage:
            return FileKind::image;
        case Category::kVideo:
            return FileKind::video;
        case Category::kAudio:
            return FileKind::audio;
        case Category::kDocument:
            return FileKind::document;
        case Category::kArchive:
            return FileKind::archive;
        case Category::kPackage:
            return FileKind::apk;
        case Category::kCode:
            return FileKind::code;
        case Category::kOther:
            break;
    }
    return FileKind::other;
}

struct Hsl {
    float h = 0;  // Degrees.
    float s = 0;
    float l = 0;
};

Hsl toHsl(gfx::Color c) {
    const float high = std::max({c.r, c.g, c.b});
    const float low = std::min({c.r, c.g, c.b});
    Hsl out{.l = (high + low) / 2};
    const float range = high - low;
    if (range <= 1e-5f) {
        return out;
    }
    out.s = range / (1 - std::abs(2 * out.l - 1));
    if (high == c.r) {
        out.h = 60 * std::fmod((c.g - c.b) / range + 6, 6.0f);
    } else if (high == c.g) {
        out.h = 60 * ((c.b - c.r) / range + 2);
    } else {
        out.h = 60 * ((c.r - c.g) / range + 4);
    }
    return out;
}

gfx::Color fromHsl(Hsl c) {
    const float chroma = (1 - std::abs(2 * c.l - 1)) * c.s;
    const float h = std::fmod(c.h, 360.0f) / 60;
    const float x = chroma * (1 - std::abs(std::fmod(h, 2.0f) - 1));
    const float m = c.l - chroma / 2;
    float r = 0;
    float g = 0;
    float b = 0;
    switch (static_cast<int>(h)) {
        case 0: r = chroma, g = x; break;
        case 1: r = x, g = chroma; break;
        case 2: g = chroma, b = x; break;
        case 3: g = x, b = chroma; break;
        case 4: r = x, b = chroma; break;
        default: r = chroma, b = x; break;
    }
    return {r + m, g + m, b + m, 1};
}

// A file's tile. Each folder at the top of the map gets its own hue, offset from the theme's
// accent so that consecutive folders (the biggest, laid out side by side) sit far apart on the
// color wheel. Within a folder, how much of the map the file takes (on a log scale, a
// ten-thousandth to all of it) sets how strongly it stands out, and siblings cycle through three
// shades so tiles next to each other never match.
gfx::Color tileColor(const Theme &t, int group, double share, int sibling) {
    constexpr float kHueOffsets[] = {0, 180, 90, 270, 45, 225, 135, 315};
    constexpr float kShades[] = {0, 0.11f, -0.09f};
    constexpr float kHueNudges[] = {0, 8, -8};
    const std::size_t groupIndex = static_cast<std::size_t>(std::max(group, 0));
    const std::size_t shade = static_cast<std::size_t>(std::max(sibling, 0)) % 3;
    const float at = std::clamp(1.0f + static_cast<float>(std::log10(std::max(share, 1e-9))) / 4.0f, 0.0f, 1.0f);
    const Hsl accent = toHsl(t.colors.accent);
    // Past the eighth folder, the hues come round again a shade apart.
    const float lap = static_cast<float>(groupIndex / std::size(kHueOffsets)) * 0.08f;
    Hsl c{.h = std::fmod(accent.h + kHueOffsets[groupIndex % std::size(kHueOffsets)] + kHueNudges[shade] + 360, 360.0f),
          .s = std::clamp(accent.s, 0.45f, 0.7f)};
    // Bigger files sit further from the ground: lighter on a dark theme, deeper on a light one.
    c.l = t.colors.light ? 0.72f - 0.22f * at - kShades[shade] - lap : 0.34f + 0.2f * at + kShades[shade] + lap;
    c.l = std::clamp(c.l, 0.2f, 0.85f);
    return fromHsl(c);
}

// The scan root when the screen opens: the folder under it in the tab, if local, else the
// primary storage volume.
std::string startingRoot(AppContext &context) {
    const std::vector<Route> &stack = context.state.peekActive()->stack.peek();
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
        if (it->kind == Route::Kind::browse && it->location && it->location->isLocal()) {
            return it->location->path();
        }
        if (it->kind == Route::Kind::browse || it->kind == Route::Kind::home) {
            break;
        }
    }
    for (const runtime::StorageVolume &volume : context.volumes.peek()) {
        if (volume.primary && !volume.path.empty()) {
            return volume.path;
        }
    }
    return "/";
}

// A line of the list: an icon, the name over a meta line, a share bar and the size. Styled from
// the theme on every bind, so a theme change only needs the rows bound again.
class ItemRow final : public Node {
public:
    static std::shared_ptr<ItemRow> create() {
        auto row = std::make_shared<ItemRow>();
        row->construct();
        return row;
    }

    void construct() {
        setStyle({.direction = Direction::row, .alignItems = Align::center, .gap = 12, .padding = {16, 0, 16, 0}});
        indent = Box::create({.width = 0});
        icon = Icon::create("folder", 22);
        auto texts = Box::create({.gap = 2, .grow = 1, .shrink = 1, .basis = 0});
        name = Text::create("");
        name->setMaxLines(1);
        name->setOverflow(gfx::TextOverflow::ellipsisMiddle);
        meta = Text::create("");
        meta->setMaxLines(1);
        texts->appendChild(name);
        texts->appendChild(meta);
        bar = Box::create({.width = 44, .height = 4});
        bar->setPaintHook([this](PaintContext &c) {
            const Theme &th = currentTheme();
            const gfx::Rect b = c.bounds();
            const gfx::CornerRadii radii = gfx::CornerRadii::uniform(b.height / 2);
            c.list.fill(gfx::RoundedRect{b, radii}, th.colors.ink.withAlpha(th.colors.surfaceRaised));
            if (share > 0) {
                const float w = std::max(b.height, b.width * std::min(1.0f, share));
                c.list.fill(gfx::RoundedRect{{b.x, b.y, w, b.height}, radii}, barColor);
            }
        });
        size = Text::create("");
        size->setAlign(gfx::TextAlign::end);
        // A fixed column: end-aligned text would otherwise take all the width it's offered.
        size->updateStyle([](Style &s) { s.width = 72; });
        for (Node *n : {static_cast<Node *>(name.get()), static_cast<Node *>(meta.get()),
                        static_cast<Node *>(size.get())}) {
            n->updateSemantics([](Semantics &s) { s.hidden = true; });
        }
        appendChild(indent);
        appendChild(icon);
        appendChild(texts);
        appendChild(bar);
        appendChild(size);
        setPaintHook([this](PaintContext &c) {
            const Theme &th = currentTheme();
            const gfx::RoundedRect shape{c.bounds(), c.px(gfx::CornerRadii::uniform(th.radii.listItem))};
            if (selected || focused) {
                c.list.fill(shape, selected ? th.colors.accent.withAlpha(th.colors.selected)
                                            : th.colors.ink.withAlpha(th.colors.surface));
            }
            if (pressable && pressable->pressed.peek()) {
                c.list.fill(shape, th.colors.ink.withAlpha(th.colors.pressed));
            }
        });
    }

    // Restyles from the theme when it changed since the last bind.
    void applyTheme(const Theme &t, std::uint64_t generation) {
        if (generation == themed) {
            return;
        }
        themed = generation;
        name->setTextStyle(t.type.body);
        name->setColor(t.colors.ink);
        meta->setTextStyle(t.type.meta);
        meta->setColor(textInk(t, t.text.meta, t.type.meta.size));
        gfx::TextStyle sizeStyle = monoText(t, 12, 500);
        size->setTextStyle(sizeStyle);
        size->setColor(textInk(t, t.text.label, 12));
        markPaintDirty();
    }

    void setShare(float value, gfx::Color color) {
        if (value != share || !(color == barColor)) {
            share = value;
            barColor = color;
            bar->markPaintDirty();
        }
    }

    void setMarks(bool isSelected, bool isFocused) {
        if (isSelected != selected || isFocused != focused) {
            selected = isSelected;
            focused = isFocused;
            markPaintDirty();
        }
    }

    std::shared_ptr<Box> indent;
    std::shared_ptr<Icon> icon;
    std::shared_ptr<Text> name;
    std::shared_ptr<Text> meta;
    std::shared_ptr<Box> bar;
    std::shared_ptr<Text> size;
    std::shared_ptr<Pressable> pressable;
    float share = 0;
    gfx::Color barColor;
    bool selected = false;
    bool focused = false;
    std::uint64_t themed = 0;
};

std::string iconFor(const adirstat::Node &node, Category category) {
    switch (node.kind) {
        case NodeKind::kDirectory:
            return node.error != 0 ? "folder_off" : kindIcon(FileKind::folder);
        case NodeKind::kMountPoint:
            return "storage";
        case NodeKind::kSymlink:
            return "link";
        case NodeKind::kOther:
            return "settings_input_component";
        case NodeKind::kFile:
            break;
    }
    return kindIcon(kindOf(category));
}

}  // namespace

// --- TreemapView ---

TreemapView::TreemapView(DiskUsageScreen &screen) : screen_(screen) {
    setDebugName("Treemap");
    auto tap = std::make_shared<TapRecognizer>();
    tap->onTapDown = [this](gfx::Vec2 local) { down_ = local; };
    tap->onTap = [this] { screen_.tapTreemap(down_.x, down_.y); };
    addGestureRecognizer(tap);
    auto longPress = std::make_shared<LongPressRecognizer>();
    longPress->onLongPressStart = [this](gfx::Vec2 local) { screen_.longPressTreemap(local.x, local.y); };
    addGestureRecognizer(longPress);
    updateSemantics([](Semantics &s) {
        s.role = Role::image;
        s.label = "Treemap";
    });
    // The outlines on a layer of their own, so moving the selection doesn't redraw the tiles.
    outlines_ = Box::create({.position = Position::absolute, .left = 0, .top = 0, .right = 0, .bottom = 0});
    outlines_->setPaintHook([this](PaintContext &context) { paintOutlines(context); });
    appendChild(outlines_);
}

void TreemapView::paint(PaintContext &context) {
    const Theme &t = currentTheme();
    const gfx::RoundedRect card{context.bounds(), context.px(gfx::CornerRadii::uniform(t.radii.panel))};
    context.list.fill(card, t.colors.ink.withAlpha(t.colors.surface));
    const auto &tiles = screen_.tiles();
    const adirstat::Tree *tree = screen_.tree();
    if (tree == nullptr || tiles.empty()) {
        return;
    }
    context.list.pushClip(card);
    const float gap = context.px(kTileGap) / 2;
    const float maxRadius = context.px(t.radii.tile);
    const double whole = static_cast<double>((*tree)[screen_.zoomRoot()].size);
    const std::optional<Category> highlight = screen_.highlight();
    // How many of each folder's tiles are drawn so far: a tile's place among its siblings.
    std::unordered_map<NodeId, int> siblings;
    gfx::TextStyle nameStyle = t.type.meta;
    nameStyle.size *= context.density * context.fontScale;
    gfx::TextStyle sizeStyle = monoText(t, 11, 500);
    sizeStyle.size *= context.density * context.fontScale;
    for (const adirstat::Tile &tile : tiles) {
        if (tile.depth == 0) {
            continue;
        }
        const gfx::Rect r{tile.rect.x + gap, tile.rect.y + gap, tile.rect.width - 2 * gap,
                          tile.rect.height - 2 * gap};
        if (r.width <= 0 || r.height <= 0) {
            continue;
        }
        const float radius = std::min(maxRadius, std::min(r.width, r.height) / 3);
        const gfx::RoundedRect shape{r, gfx::CornerRadii::uniform(radius)};
        const adirstat::Node &node = (*tree)[tile.node];
        if (!tile.leaf) {
            // A folder: a frame in its group's hue, a little stronger each level down, so the
            // regions of the map read apart at a glance.
            const float strength = std::min(0.45f, 0.16f + 0.08f * static_cast<float>(tile.depth));
            const gfx::Color hue = tileColor(t, screen_.groupOf(tile.node), 1, 0);
            context.list.fill(shape, hue.withAlpha(strength));
            continue;
        }
        const int sibling = siblings[node.parent]++;
        gfx::Color color;
        const bool file = node.kind == NodeKind::kFile;
        if (highlight && (!file || static_cast<Category>(node.category) != *highlight)) {
            color = t.colors.ink.withAlpha(t.colors.surface);
        } else if (highlight) {
            // The highlighted type wears its own color, as in the list below.
            color = kindColor(kindOf(*highlight), t);
        } else if (file) {
            const double share = whole > 0 ? static_cast<double>(node.size) / whole : 0;
            color = tileColor(t, screen_.groupOf(tile.node), share, sibling);
        } else {
            // Folded folders and what isn't a file.
            color = t.colors.ink.withAlpha(t.colors.surfaceRaised);
        }
        context.list.fill(shape, color);
        // A darker rim, so tiles of one shade still part where they meet.
        const float rim = context.px(1);
        const gfx::Rect inner{r.x + rim / 2, r.y + rim / 2, r.width - rim, r.height - rim};
        context.list.stroke(gfx::RoundedRect{inner, gfx::CornerRadii::uniform(std::max(0.0f, radius - rim / 2))}, rim,
                            darken(color, 0.35f).withAlpha(color.a));
        if (context.text == nullptr || r.width < context.px(kLabelWidth) || r.height < context.px(kLabelHeight)) {
            continue;
        }
        const gfx::Color background = ui::composite(color, t.colors.ground);
        const gfx::Color label = pickInk(background, {0, 0, 0, 0.87f}, {1, 1, 1, 0.95f});
        gfx::TextLayoutOptions options;
        options.maxLines = 1;
        options.maxWidth = r.width - context.px(16);
        options.overflow = gfx::TextOverflow::ellipsisMiddle;
        const float x = r.x + context.px(8);
        auto name = std::make_shared<const gfx::ShapedText>(
                context.text->shape(std::string(tree->name(tile.node)), nameStyle, options));
        context.list.text(name, {x, r.y + context.px(6)}, label);
        auto size = std::make_shared<const gfx::ShapedText>(
                context.text->shape(formatSize(node.size), sizeStyle, options));
        context.list.text(size, {x, r.y + context.px(8) + name->size.height}, label.withAlpha(label.a * 0.75f));
    }
    context.list.pop();
}

void TreemapView::paintOutlines(PaintContext &context) {
    const Theme &t = currentTheme();
    const auto &tiles = screen_.tiles();
    const adirstat::Tree *tree = screen_.tree();
    if (tree == nullptr || tiles.empty()) {
        return;
    }
    const float gap = context.px(kTileGap) / 2;
    const float maxRadius = context.px(t.radii.tile);
    const auto outline = [&](NodeId id, float width, gfx::Color color) {
        // The node's own tile, or the tile of the nearest ancestor that has one.
        for (NodeId at = id; at != kNoNode; at = (*tree)[at].parent) {
            for (const adirstat::Tile &tile : tiles) {
                if (tile.node == at) {
                    const float inset = gap + width / 2;
                    const gfx::Rect r{tile.rect.x + inset, tile.rect.y + inset,
                                      std::max(0.0f, tile.rect.width - 2 * inset),
                                      std::max(0.0f, tile.rect.height - 2 * inset)};
                    const float radius = std::min(maxRadius, std::min(r.width, r.height) / 3);
                    context.list.stroke(gfx::RoundedRect{r, gfx::CornerRadii::uniform(radius)}, width, color);
                    return;
                }
            }
            if (at == screen_.zoomRoot()) {
                return;
            }
        }
    };
    for (const NodeId id : screen_.selection()) {
        outline(id, context.px(2.5f), t.colors.accent);
    }
    if (screen_.focus() != kNoNode) {
        outline(screen_.focus(), context.px(2), t.colors.ink);
    }
}

void TreemapView::onSizeChanged() { screen_.treemapResized(); }

// --- DiskUsageScreen ---

std::shared_ptr<DiskUsageScreen> DiskUsageScreen::create(AppContext &context, std::string root,
                                                         DiskUsageOptions options) {
    auto screen = std::make_shared<DiskUsageScreen>(context, std::move(root), options);
    screen->construct();
    screen->rescan();
    return screen;
}

DiskUsageScreen::DiskUsageScreen(AppContext &context, std::string root, DiskUsageOptions options)
    : context_(context), services_(context.services), root_(std::move(root)), options_(options) {
    setDebugName("DiskUsageScreen");
}

DiskUsageScreen::~DiskUsageScreen() {
    cancel_.cancel();
    if (pollTimer_ != 0) {
        services_.loop.cancel(pollTimer_);
    }
}

void DiskUsageScreen::construct() {
    setStyle(fill());

    header_ = Box::create({.gap = 6, .padding = {16, 0, 16, 12}});
    auto pathRow = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 8});
    pathText_ = Text::create("");
    pathText_->setMaxLines(1);
    pathText_->setOverflow(gfx::TextOverflow::ellipsisStart);
    pathText_->updateStyle([](Style &s) {
        s.grow = 1;
        s.shrink = 1;
        s.basis = 0;
    });
    pathRow->appendChild(pathText_);
    pathRow->appendChild(IconButton::create({.icon = "refresh",
                                             .label = "Scan again",
                                             .style = IconButtonStyle::plain,
                                             .size = 40,
                                             .onTap = [this] { rescan(); }}));
    header_->appendChild(pathRow);

    statusText_ = Text::create("");
    statusText_->setMaxLines(1);
    statusText_->updateSemantics([](Semantics &s) { s.role = Role::heading; });
    header_->appendChild(statusText_);
    detailText_ = Text::create("");
    detailText_->setMaxLines(1);
    detailText_->setOverflow(gfx::TextOverflow::ellipsisStart);
    header_->appendChild(detailText_);
    cancelButton_ = Button::create({.label = "Stop", .variant = ButtonVariant::glass, .icon = "stop", .onTap = [this] {
                                        cancel_.cancel();
                                    }});
    cancelButton_->updateStyle([](Style &s) {
        s.alignSelf = Align::start;
        s.margin = {0, 8, 0, 0};
    });
    header_->appendChild(cancelButton_);

    // Where the map is zoomed to, with the way back up.
    zoomRow_ = Box::create(
            {.direction = Direction::row, .alignItems = Align::center, .gap = 4, .margin = {0, 10, 0, 0}});
    zoomText_ = Text::create("");
    zoomText_->setMaxLines(1);
    zoomText_->setOverflow(gfx::TextOverflow::ellipsisStart);
    zoomText_->updateStyle([](Style &s) {
        s.grow = 1;
        s.shrink = 1;
        s.basis = 0;
    });
    zoomRow_->appendChild(zoomText_);
    zoomOutButton_ = IconButton::create({.icon = "zoom_out_map",
                                         .label = "Zoom out",
                                         .style = IconButtonStyle::plain,
                                         .size = 40,
                                         .onTap = [this] { zoomOut(); }});
    zoomRow_->appendChild(zoomOutButton_);
    header_->appendChild(zoomRow_);

    treemap_ = std::make_shared<TreemapView>(*this);
    treemap_->setStyle({.aspectRatio = 1.2f});
    header_->appendChild(treemap_);

    tabs_ = SegmentedControl::create({.options = {"Folders", "File types"},
                                      .selected = [this] { return std::size_t{view_.get() == View::types ? 1u : 0u}; },
                                      .onSelect = [this](std::size_t i) { setView(i == 0 ? View::folders : View::types); },
                                      .label = "List"});
    tabs_->updateStyle([](Style &s) { s.margin = {0, 8, 0, 4}; });
    header_->appendChild(tabs_);

    list_ = VirtualList::create();
    list_->setStyle(fill());
    list_->setHeader(header_);
    list_->updateSemantics([](Semantics &s) { s.role = Role::list; });
    list_->setCreate([this](std::uint32_t) -> NodePtr {
        auto row = ItemRow::create();
        Node *raw = row.get();
        row->pressable = makePressable(*row, {.onTap =
                                                      [this, raw] {
                                                          if (auto it = boundItem_.find(raw); it != boundItem_.end()) {
                                                              tapItem(it->second);
                                                          }
                                                      },
                                              .onLongPress =
                                                      [this, raw] {
                                                          if (auto it = boundItem_.find(raw); it != boundItem_.end()) {
                                                              longPressItem(it->second);
                                                          }
                                                      }});
        ItemRow *self = row.get();
        row->build([&] {
            row->bind([self] {
                (void)self->pressable->pressed();
                untrack([&] { self->markPaintDirty(); });
            });
        });
        return row;
    });
    list_->setBind([this](std::size_t index, Node &node) { bindItem(index, node); });
    appendChild(list_);

    // Back to the top, once the treemap has scrolled away.
    topButton_ = GlassPanel({.position = Position::absolute, .right = 16, .bottom = 16}, 24);
    topButton_->setDebugName("DiskUsageTop");
    topButton_->appendChild(IconButton::create({.icon = "vertical_align_top",
                                                .label = "Back to top",
                                                .style = IconButtonStyle::plain,
                                                .onTap = [this] { scrollToTop(); }}));
    appendChild(topButton_);

    selectionBar_ = GlassPanel({.direction = Direction::row,
                                .alignItems = Align::center,
                                .gap = 4,
                                .padding = {16, 6, 6, 6},
                                .position = Position::absolute,
                                .left = 12,
                                .right = 12,
                                .bottom = 12});
    selectionBar_->setDebugName("DiskUsageSelection");
    selectionText_ = Text::create("");
    selectionText_->setMaxLines(1);
    selectionText_->updateStyle([](Style &s) {
        s.grow = 1;
        s.shrink = 1;
        s.basis = 0;
    });
    selectionBar_->appendChild(selectionText_);
    openButton_ = IconButton::create(
            {.icon = "open_in_new", .label = "Open", .style = IconButtonStyle::plain, .onTap = [this] { openSelection(); }});
    revealButton_ = IconButton::create({.icon = "folder_open",
                                        .label = "Show in folder",
                                        .style = IconButtonStyle::plain,
                                        .onTap = [this] { revealSelection(); }});
    selectionBar_->appendChild(openButton_);
    selectionBar_->appendChild(revealButton_);
    selectionBar_->appendChild(IconButton::create({.icon = "delete",
                                                   .label = "Delete",
                                                   .style = IconButtonStyle::plain,
                                                   .destructive = true,
                                                   .onTap = [this] { deleteSelection(); }}));
    selectionBar_->appendChild(IconButton::create({.icon = "close",
                                                   .label = "Clear selection",
                                                   .style = IconButtonStyle::plain,
                                                   .onTap = [this] { clearSelection(); }}));
    appendChild(selectionBar_);

    DiskUsageScreen *self = this;
    this->build([&] {
        bind([self] {
            const Theme &t = theme();
            untrack([&] { self->applyTheme(t); });
        });
        // Insets: the list clears the top bar, and the floating bars the navigation bar.
        bind([self] {
            const float density = environment().density();
            const float bottom = environment().safeArea().bottom / density;
            const float top = self->context_.barHeight();
            untrack([&] {
                self->safeBottom_ = bottom;
                self->list_->updateStyle([&](Style &s) { s.padding = {0, top + 4, 0, bottom + 88}; });
                self->selectionBar_->updateStyle([&](Style &s) { s.bottom = bottom + 12; });
                self->placeTopButton();
            });
        });
        bind([self] {
            const float offset = self->list_->offsetSignal()();
            untrack([&] {
                self->context_.barScroll.set(offset);
                self->placeTopButton();
            });
        });
    });
    refreshHeader();
    refreshSelectionBar();
}

void DiskUsageScreen::applyTheme(const Theme &t) {
    ++themeGeneration_;
    pathText_->setTextStyle(monoText(t, 12, 500));
    pathText_->setColor(ink(t, t.text.label));
    gfx::TextStyle total = t.type.display;
    total.uppercase = false;
    statusText_->setTextStyle(total);
    statusText_->setColor(t.colors.ink);
    detailText_->setTextStyle(t.type.label);
    detailText_->setColor(textInk(t, t.text.label, t.type.label.size));
    zoomText_->setTextStyle(t.type.label);
    zoomText_->setColor(textInk(t, t.text.body, t.type.label.size));
    selectionText_->setTextStyle(t.type.body);
    selectionText_->setColor(t.colors.ink);
    list_->setFixedItemExtent(t.spacing.listRow);
    list_->notifyReset();
    treemap_->markPaintDirty();
    treemap_->outlinesChanged();
}

bool DiskUsageScreen::scrolledDown() const { return list_->offsetSignal().peek() > 1; }

void DiskUsageScreen::scrollToTop() { list_->scrollTo(0, true); }

void DiskUsageScreen::placeTopButton() {
    // Shown once the treemap is out of sight; above the selection bar when that's up.
    const bool past = list_->offsetSignal().peek() > header_->frame().height * 0.8f && header_->frame().height > 0;
    const bool selecting = !effectiveSelection().empty();
    topButton_->updateStyle([&](Style &s) {
        s.display = past;
        s.bottom = safeBottom_ + (selecting ? 80.0f : 16.0f);
    });
}

bool DiskUsageScreen::back() {
    if (!selection_.empty()) {
        clearSelection();
        return true;
    }
    if (view_.peek() == View::types && (highlight_ || openCategory_)) {
        openCategory_.reset();
        setHighlight(std::nullopt);
        rebuildItems();
        return true;
    }
    if (scrolledDown()) {
        scrollToTop();
        return true;
    }
    if (tree_ != nullptr && zoom_ != tree_->root()) {
        zoomOut();
        // Keep the map in view to see it zoom out, rather than the focused row.
        scrollToTop();
        return true;
    }
    return false;
}

void DiskUsageScreen::rescan() {
    cancel_.cancel();
    cancel_ = CancellationToken();
    progress_ = std::make_shared<adirstat::ScanProgress>();
    phase_ = Phase::scanning;
    failure_.clear();
    tree_.reset();
    tiles_.clear();
    tileOf_.clear();
    treemap_->markPaintDirty();
    zoom_ = 0;
    focus_ = kNoNode;
    selection_.clear();
    expanded_.clear();
    highlight_.reset();
    openCategory_.reset();
    stats_ = {};
    items_.clear();
    list_->setItemCount(0);

    const std::uint64_t generation = ++scanGeneration_;
    auto weak = std::weak_ptr(std::static_pointer_cast<DiskUsageScreen>(shared_from_this()));
    services_.io.submit(
            [root = root_, cancel = cancel_, progress = progress_] {
                auto result = adirstat::scan(root, {}, cancel, progress.get());
                if (result) {
                    adirstat::assignCategories(**result);
                }
                return result;
            },
            [weak, generation](Result<std::unique_ptr<adirstat::Tree>> result) {
                auto self = weak.lock();
                if (self && generation == self->scanGeneration_) {
                    self->finishScan(std::move(result));
                }
            });
    if (pollTimer_ != 0) {
        services_.loop.cancel(pollTimer_);
    }
    poll();
    refreshHeader();
    refreshSelectionBar();
}

void DiskUsageScreen::poll() {
    pollTimer_ = 0;
    if (phase_ != Phase::scanning) {
        return;
    }
    refreshHeader();
    auto weak = std::weak_ptr(std::static_pointer_cast<DiskUsageScreen>(shared_from_this()));
    pollTimer_ = services_.loop.postDelayed(
            [weak] {
                if (auto self = weak.lock()) {
                    self->poll();
                }
            },
            kPollNs);
}

void DiskUsageScreen::finishScan(Result<std::unique_ptr<adirstat::Tree>> result) {
    if (!result) {
        phase_ = Phase::failed;
        failure_ = result.error().isCancelled() ? "Stopped" : std::string(std::strerror(result.error().code));
        refreshHeader();
        return;
    }
    phase_ = Phase::done;
    tree_ = std::move(*result);
    zoom_ = tree_->root();
    stats_ = adirstat::computeTypeStats(*tree_, zoom_);
    rebuildItems();
    relayout();
    refreshHeader();
}

void DiskUsageScreen::refreshHeader() {
    const bool scanning = phase_ == Phase::scanning;
    cancelButton_->updateStyle([&](Style &s) { s.display = scanning; });
    const bool done = phase_ == Phase::done && tree_ != nullptr;
    zoomRow_->updateStyle([&](Style &s) { s.display = done; });
    treemap_->updateStyle([&](Style &s) { s.display = done; });
    tabs_->updateStyle([&](Style &s) { s.display = done; });
    pathText_->setText(locationPath(context_, vfs::Location::local(root_)));
    if (scanning) {
        const std::uint64_t files = progress_ ? progress_->files.load(std::memory_order_relaxed) : 0;
        const std::uint64_t bytes = progress_ ? progress_->bytes.load(std::memory_order_relaxed) : 0;
        statusText_->setText("Scanning… " + formatSize(bytes));
        detailText_->setText(formatCount(files) + (progress_ ? " · " + progress_->current() : std::string()));
        return;
    }
    if (!done) {
        statusText_->setText(failure_ == "Stopped" ? "Scan stopped" : "Couldn't scan this folder");
        detailText_->setText(failure_);
        return;
    }
    const adirstat::Node &root = (*tree_)[tree_->root()];
    statusText_->setText(formatSize(root.size));
    std::string detail = formatCount(root.files) + " · " + std::to_string(root.dirs) + " folders";
    // Only worth saying when it's far off: sparse files, or many small ones.
    if (options_.showAllocated && (root.allocated > root.size + root.size / 10 || root.allocated < root.size - root.size / 10)) {
        detail += " · " + formatSize(root.allocated) + " on disk";
    }
    if (root.errors > 0) {
        detail += " · " + std::to_string(root.errors) + " unreadable";
    }
    detailText_->setText(detail);
    // The way down from the scan root, as a trail: "DCIM / Camera".
    std::string trail;
    for (NodeId at = zoom_; at != tree_->root(); at = (*tree_)[at].parent) {
        trail = std::string(tree_->name(at)) + (trail.empty() ? "" : " / " + trail);
    }
    const adirstat::Node &zoom = (*tree_)[zoom_];
    zoomText_->setText((trail.empty() ? std::string("Showing everything") : trail) + " · " + formatSize(zoom.size));
    zoomOutButton_->updateStyle([&](Style &s) { s.display = zoom_ != tree_->root(); });
}

void DiskUsageScreen::refreshSelectionBar() {
    const std::vector<NodeId> chosen = effectiveSelection();
    selectionBar_->updateStyle([&](Style &s) { s.display = !chosen.empty(); });
    placeTopButton();
    if (chosen.empty() || tree_ == nullptr) {
        return;
    }
    std::uint64_t bytes = 0;
    for (const NodeId id : chosen) {
        bytes += (*tree_)[id].size;
    }
    selectionText_->setText(std::to_string(chosen.size()) + " selected · " + formatSize(bytes));
    const bool one = chosen.size() == 1;
    openButton_->updateStyle([&](Style &s) { s.display = one && (*tree_)[chosen.front()].kind == NodeKind::kFile; });
    revealButton_->updateStyle([&](Style &s) { s.display = one; });
}

void DiskUsageScreen::setView(View view) {
    if (view == view_.peek()) {
        return;
    }
    view_.set(view);
    if (view == View::folders) {
        setHighlight(std::nullopt);
    }
    rebuildItems();
}

void DiskUsageScreen::rebuildItems() {
    // Each folder at the top of the map takes a hue; the list's bars match.
    groups_.clear();
    if (tree_ != nullptr) {
        int index = 0;
        for (const NodeId child : tree_->children(zoom_)) {
            groups_[child] = index++;
        }
    }
    items_.clear();
    if (tree_ != nullptr) {
        if (view_.peek() == View::folders) {
            // Depth first through the open folders, from the zoom root's children.
            struct Pending {
                NodeId node;
                int depth;
            };
            std::vector<Pending> stack;
            const auto push = [&](NodeId dir, int depth) {
                std::vector<NodeId> children = tree_->children(dir);
                if (children.size() > kMaxChildren) {
                    children.resize(kMaxChildren);
                }
                for (auto it = children.rbegin(); it != children.rend(); ++it) {
                    stack.push_back({*it, depth});
                }
            };
            push(zoom_, 0);
            while (!stack.empty()) {
                const Pending p = stack.back();
                stack.pop_back();
                items_.push_back({.kind = Item::Kind::node, .node = p.node, .depth = p.depth});
                if (expanded_.contains(p.node)) {
                    push(p.node, p.depth + 1);
                }
            }
        } else {
            std::vector<std::size_t> order;
            for (std::size_t i = 0; i < adirstat::kCategoryCount; ++i) {
                if (stats_.categories[i].files > 0) {
                    order.push_back(i);
                }
            }
            std::ranges::sort(order, [&](std::size_t a, std::size_t b) {
                return stats_.categories[a].size > stats_.categories[b].size;
            });
            for (const std::size_t i : order) {
                const auto category = static_cast<Category>(i);
                items_.push_back({.kind = Item::Kind::category, .category = category});
                if (openCategory_ != category) {
                    continue;
                }
                std::size_t shown = 0;
                for (std::size_t e = 0; e < stats_.extensions.size() && shown < kMaxExtensions; ++e) {
                    if (stats_.extensions[e].category == category) {
                        items_.push_back(
                                {.kind = Item::Kind::extension, .depth = 1, .category = category, .extension = e});
                        ++shown;
                    }
                }
            }
        }
    }
    list_->setItemCount(items_.size());
}

void DiskUsageScreen::bindItem(std::size_t index, Node &node) {
    auto &row = static_cast<ItemRow &>(node);
    boundItem_[&node] = index;
    const Theme &t = currentTheme();
    row.applyTheme(t, themeGeneration_);
    row.updateStyle([&](Style &s) { s.height = t.spacing.listRow; });
    const Item &item = items_[index];
    std::string label;
    if (item.kind == Item::Kind::node) {
        const adirstat::Node &n = (*tree_)[item.node];
        const auto category = static_cast<Category>(n.category);
        const bool dir = isDirectory(n);
        const std::uint64_t parentSize = (*tree_)[n.parent].size;
        const std::uint64_t wholeSize = (*tree_)[zoom_].size;
        row.indent->updateStyle([&](Style &s) { s.width = kIndent * static_cast<float>(item.depth); });
        const bool open = expanded_.contains(item.node);
        row.icon->setIcon(dir && n.childCount > 0 ? (open ? "expand_more" : "chevron_right") : iconFor(n, category));
        row.icon->setColor(dir                           ? t.colors.highlight
                           : n.kind == NodeKind::kFile ? kindColor(kindOf(category), t)
                                                       : ink(t, t.text.meta));
        row.name->setText(std::string(tree_->name(item.node)));
        std::string meta = percentOf(n.size, parentSize);
        if (dir) {
            meta += " · " + formatCount(n.files);
            if (n.error != 0) {
                meta += " · unreadable";
            } else if (n.kind == NodeKind::kMountPoint) {
                meta += " · other storage, not scanned";
            }
        } else if (n.flags & adirstat::kHardLinkDuplicate) {
            meta += " · hard link, counted once";
        }
        if (n.newest > 0) {
            meta += " · " + formatShortDate({.seconds = n.newest, .nanoseconds = 0});
        }
        row.meta->setText(meta);
        row.size->setText(formatSize(n.size));
        const double ofMap = wholeSize == 0 ? 0 : static_cast<double>(n.size) / static_cast<double>(wholeSize);
        row.setShare(parentSize == 0 ? 0 : static_cast<float>(static_cast<double>(n.size) / parentSize),
                     tileColor(t, groupOf(item.node), ofMap, 0));
        row.setMarks(selection_.contains(item.node), focus_ == item.node);
        label = std::string(tree_->name(item.node)) + ", " + formatSize(n.size) + ", " + meta;
        row.updateSemantics([&](Semantics &s) {
            setSemanticsFlag(row, semantics_flags::kExpandable, dir && n.childCount > 0);
            setSemanticsFlag(row, semantics_flags::kExpanded, open);
            setSemanticsFlag(row, semantics_flags::kSelected, selection_.contains(item.node));
            s.role = Role::listItem;
            s.label = label;
        });
        return;
    }
    const adirstat::TypeTotals &totals = item.kind == Item::Kind::category
                                                 ? stats_.categories[static_cast<std::size_t>(item.category)]
                                                 : stats_.extensions[item.extension].totals;
    const gfx::Color color = kindColor(kindOf(item.category), t);
    row.indent->updateStyle([&](Style &s) { s.width = kIndent * static_cast<float>(item.depth); });
    std::string name;
    if (item.kind == Item::Kind::category) {
        row.icon->setIcon(openCategory_ == item.category ? "expand_more" : kindIcon(kindOf(item.category)));
        row.icon->setFilled(true);
        name = std::string(adirstat::categoryName(item.category));
    } else {
        const std::string &extension = stats_.extensions[item.extension].extension;
        row.icon->setIcon("label");
        row.icon->setFilled(false);
        name = extension.empty() ? std::string("No extension") : "." + extension;
    }
    row.icon->setColor(color);
    row.name->setText(name);
    const std::string meta = percentOf(totals.size, stats_.all.size) + " · " + formatCount(totals.files);
    row.meta->setText(meta);
    row.size->setText(formatSize(totals.size));
    row.setShare(stats_.all.size == 0 ? 0 : static_cast<float>(static_cast<double>(totals.size) / stats_.all.size),
                 color);
    row.setMarks(false, item.kind == Item::Kind::category && highlight_ == item.category);
    label = name + ", " + formatSize(totals.size) + ", " + meta;
    row.updateSemantics([&](Semantics &s) {
        s.role = Role::listItem;
        s.label = label;
    });
}

void DiskUsageScreen::treemapResized() {
    if (tree_ != nullptr) {
        relayout();
    }
}

void DiskUsageScreen::relayout() {
    tiles_.clear();
    tileOf_.clear();
    const float density = environment().density();
    const gfx::Size size = treemap_->sizePx(density);
    const int width = static_cast<int>(std::floor(size.width));
    const int height = static_cast<int>(std::floor(size.height));
    if (tree_ == nullptr || width <= 0 || height <= 0) {
        treemap_->tilesChanged();
        return;
    }
    tiles_ = adirstat::layoutTreemap(*tree_, zoom_, {0, 0, static_cast<float>(width), static_cast<float>(height)},
                                     {.minTileSize = std::max(4.0f, density * 2.5f),
                                      .padding = density * kFramePadding});
    for (std::size_t i = 0; i < tiles_.size(); ++i) {
        tileOf_[tiles_[i].node] = i;
    }
    treemap_->tilesChanged();
}

int DiskUsageScreen::groupOf(NodeId id) const {
    // The node's ancestor at the top of the map: the zoom root's child it falls under.
    for (NodeId at = id; at != kNoNode && at != zoom_; at = (*tree_)[at].parent) {
        if ((*tree_)[at].parent == zoom_) {
            const auto it = groups_.find(at);
            return it == groups_.end() ? 0 : it->second;
        }
    }
    return 0;
}

void DiskUsageScreen::zoomTo(NodeId dir) {
    if (tree_ == nullptr || !isDirectory((*tree_)[dir]) || dir == zoom_) {
        return;
    }
    zoom_ = dir;
    if (focus_ != kNoNode && !tree_->isAncestorOf(zoom_, focus_)) {
        focus_ = kNoNode;
    }
    stats_ = adirstat::computeTypeStats(*tree_, zoom_);
    rebuildItems();
    relayout();
    refreshHeader();
}

void DiskUsageScreen::zoomOut() {
    if (tree_ != nullptr && zoom_ != tree_->root()) {
        const NodeId from = zoom_;
        zoomTo((*tree_)[zoom_].parent);
        setFocus(from);
    }
}

void DiskUsageScreen::setFocus(NodeId id) {
    if (tree_ == nullptr) {
        return;
    }
    focus_ = id;
    if (id != kNoNode && view_.peek() == View::folders) {
        // Open the folders down to it, so its row is there to scroll to.
        bool changed = false;
        for (NodeId at = (*tree_)[id].parent; at != kNoNode && at != zoom_; at = (*tree_)[at].parent) {
            changed |= expanded_.insert(at).second;
        }
        if (changed) {
            rebuildItems();
        } else {
            list_->notifyReset();
        }
        for (std::size_t i = 0; i < items_.size(); ++i) {
            if (items_[i].kind == Item::Kind::node && items_[i].node == id) {
                const auto [first, last] = list_->boundRange();
                if (i <= first || i + 1 >= last) {
                    list_->scrollToItem(i, true);
                }
                break;
            }
        }
    } else {
        list_->notifyReset();
    }
    treemap_->outlinesChanged();
}

void DiskUsageScreen::toggleExpanded(NodeId dir) {
    if (!expanded_.erase(dir)) {
        expanded_.insert(dir);
    }
    rebuildItems();
}

void DiskUsageScreen::toggleSelected(NodeId id) {
    if (tree_ == nullptr || id == tree_->root()) {
        return;
    }
    if (!selection_.erase(id)) {
        selection_.insert(id);
    }
    list_->notifyReset();
    treemap_->outlinesChanged();
    refreshSelectionBar();
}

void DiskUsageScreen::clearSelection() {
    selection_.clear();
    list_->notifyReset();
    treemap_->outlinesChanged();
    refreshSelectionBar();
}

void DiskUsageScreen::setHighlight(std::optional<Category> category) {
    if (category == highlight_) {
        return;
    }
    highlight_ = category;
    if (tree_ != nullptr && !tiles_.empty()) {
        treemap_->markPaintDirty();
    }
    list_->notifyReset();
}

std::vector<NodeId> DiskUsageScreen::effectiveSelection() const {
    std::vector<NodeId> out;
    if (tree_ == nullptr) {
        return out;
    }
    for (const NodeId id : selection_) {
        if ((*tree_)[id].flags & adirstat::kRemoved) {
            continue;
        }
        bool covered = false;
        for (const NodeId other : selection_) {
            if (other != id && tree_->isAncestorOf(other, id)) {
                covered = true;
                break;
            }
        }
        if (!covered) {
            out.push_back(id);
        }
    }
    return out;
}

vfs::Location DiskUsageScreen::locationOf(NodeId id) const { return vfs::Location::local(tree_->pathOf(id)); }

vfs::Entry DiskUsageScreen::entryOf(NodeId id) const {
    const adirstat::Node &node = (*tree_)[id];
    vfs::Entry entry;
    entry.name = std::string(tree_->name(id));
    switch (node.kind) {
        case NodeKind::kFile:
            entry.type = vfs::EntryType::kFile;
            break;
        case NodeKind::kDirectory:
        case NodeKind::kMountPoint:
            entry.type = vfs::EntryType::kDirectory;
            break;
        case NodeKind::kSymlink:
            entry.type = vfs::EntryType::kSymlink;
            break;
        case NodeKind::kOther:
            entry.type = vfs::EntryType::kOther;
            break;
    }
    entry.targetType = entry.type == vfs::EntryType::kSymlink ? vfs::EntryType::kOther : entry.type;
    entry.hidden = vfs::Entry::isHiddenName(entry.name);
    entry.hasStat = true;
    entry.size = node.ownSize;
    entry.modifiedTime = {.seconds = node.modified, .nanoseconds = 0};
    return entry;
}

void DiskUsageScreen::deleteSelection() {
    const std::vector<NodeId> chosen = effectiveSelection();
    if (chosen.empty()) {
        return;
    }
    std::map<NodeId, std::vector<vfs::Entry>> groups;
    for (const NodeId id : chosen) {
        groups[(*tree_)[id].parent].push_back(entryOf(id));
    }
    std::vector<FileTargets> targets;
    for (auto &[parent, entries] : groups) {
        targets.push_back({.directory = locationOf(parent), .entries = std::move(entries)});
    }
    auto weak = std::weak_ptr(std::static_pointer_cast<DiskUsageScreen>(shared_from_this()));
    const std::uint64_t generation = scanGeneration_;
    context_.actions.removeFrom(targets, [weak, generation, chosen] {
        if (auto self = weak.lock(); self && generation == self->scanGeneration_) {
            self->afterDelete(chosen);
        }
    });
}

void DiskUsageScreen::afterDelete(std::vector<NodeId> removed) {
    // Some may have failed or been skipped: take out only what's really gone.
    std::vector<std::string> paths;
    for (const NodeId id : removed) {
        paths.push_back(tree_->pathOf(id));
    }
    auto weak = std::weak_ptr(std::static_pointer_cast<DiskUsageScreen>(shared_from_this()));
    const std::uint64_t generation = scanGeneration_;
    services_.io.submit(
            [paths] {
                std::vector<bool> gone;
                for (const std::string &path : paths) {
                    struct stat st {};
                    gone.push_back(::lstat(path.c_str(), &st) != 0 && errno == ENOENT);
                }
                return gone;
            },
            [weak, generation, removed](std::vector<bool> gone) {
                auto self = weak.lock();
                if (!self || generation != self->scanGeneration_ || self->tree_ == nullptr) {
                    return;
                }
                for (std::size_t i = 0; i < removed.size(); ++i) {
                    if (gone[i]) {
                        self->tree_->remove(removed[i]);
                        std::erase_if(self->selection_,
                                      [&](NodeId id) { return self->tree_->isAncestorOf(removed[i], id); });
                        if (self->focus_ != kNoNode && self->tree_->isAncestorOf(removed[i], self->focus_)) {
                            self->focus_ = kNoNode;
                        }
                        if (self->tree_->isAncestorOf(removed[i], self->zoom_)) {
                            self->zoom_ = (*self->tree_)[removed[i]].parent;
                        }
                    }
                }
                self->stats_ = adirstat::computeTypeStats(*self->tree_, self->zoom_);
                self->rebuildItems();
                self->relayout();
                self->refreshHeader();
                self->refreshSelectionBar();
            });
}

void DiskUsageScreen::openSelection() {
    const std::vector<NodeId> chosen = effectiveSelection();
    if (chosen.size() != 1 || (*tree_)[chosen.front()].kind != NodeKind::kFile) {
        return;
    }
    const NodeId id = chosen.front();
    context_.actions.open({.directory = locationOf((*tree_)[id].parent), .entries = {entryOf(id)}});
}

void DiskUsageScreen::revealSelection() {
    const std::vector<NodeId> chosen = effectiveSelection();
    if (chosen.size() == 1 && context_.reveal) {
        context_.reveal(locationOf(chosen.front()));
    }
}

void DiskUsageScreen::tapTreemap(float x, float y) {
    const auto hit = adirstat::leafAt(tiles_, x, y);
    if (!hit) {
        return;
    }
    const std::int64_t now = services_.loop.now();
    const NodeId id = tiles_[*hit].node;
    if (!selection_.empty()) {
        toggleSelected(id);
        return;
    }
    // A second tap on the same tile soon after zooms in.
    if (now - lastTapNs_ < kDoubleTapNs && std::abs(x - lastTap_.x) < 24 && std::abs(y - lastTap_.y) < 24) {
        lastTapNs_ = 0;
        doubleTapTreemap(x, y);
        return;
    }
    lastTapNs_ = now;
    lastTap_ = {x, y};
    // Tapping the focused tile again moves up a level.
    if (focus_ != kNoNode && tree_->isAncestorOf(focus_, id) && focus_ != zoom_) {
        const NodeId parent = (*tree_)[focus_].parent;
        setFocus(parent == zoom_ ? id : parent);
        return;
    }
    setFocus(id);
}

void DiskUsageScreen::doubleTapTreemap(float x, float y) {
    const auto hit = adirstat::leafAt(tiles_, x, y);
    if (!hit) {
        return;
    }
    NodeId id = tiles_[*hit].node;
    if (!isDirectory((*tree_)[id])) {
        id = (*tree_)[id].parent;
    }
    zoomTo(id);
}

void DiskUsageScreen::longPressTreemap(float x, float y) {
    if (const auto hit = adirstat::leafAt(tiles_, x, y)) {
        haptic(Haptic::longPress);
        toggleSelected(focus_ != kNoNode && tree_->isAncestorOf(focus_, tiles_[*hit].node) ? focus_
                                                                                             : tiles_[*hit].node);
    }
}

void DiskUsageScreen::tapItem(std::size_t index) {
    if (index >= items_.size()) {
        return;
    }
    const Item item = items_[index];
    switch (item.kind) {
        case Item::Kind::node: {
            if (!selection_.empty()) {
                toggleSelected(item.node);
                return;
            }
            const adirstat::Node &node = (*tree_)[item.node];
            if (isDirectory(node) && node.childCount > 0 && (focus_ == item.node || !expanded_.contains(item.node))) {
                toggleExpanded(item.node);
            }
            setFocus(item.node);
            return;
        }
        case Item::Kind::category:
            openCategory_ = openCategory_ == item.category ? std::nullopt : std::optional(item.category);
            setHighlight(item.category == highlight_ && !openCategory_ ? std::nullopt : std::optional(item.category));
            rebuildItems();
            return;
        case Item::Kind::extension:
            setHighlight(item.category);
            return;
    }
}

void DiskUsageScreen::longPressItem(std::size_t index) {
    if (index < items_.size() && items_[index].kind == Item::Kind::node) {
        haptic(Haptic::longPress);
        toggleSelected(items_[index].node);
    }
}

std::vector<ScreenSpec> diskUsageScreenSpecs(DiskUsageOptions options) {
    return {{.id = kDiskUsageScreen,
             .title = "Disk usage",
             .icon = "donut_small",
             .build = [options](AppContext &context) -> NodePtr {
                 return DiskUsageScreen::create(context, startingRoot(context), options);
             },
             .back = [](Node &screen) { return static_cast<DiskUsageScreen &>(screen).back(); },
             .inMenu = true,
             .enabled = [](const Settings &settings) { return settings.diskUsage; }}};
}

}  // namespace filepan::app
