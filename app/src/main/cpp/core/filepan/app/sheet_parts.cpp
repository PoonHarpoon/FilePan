// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/sheet_parts.hpp"

#include <algorithm>

#include "filepan/app/format.hpp"
#include "filepan/ui/components/recipes.hpp"
#include "filepan/ui/scroll.hpp"

namespace filepan::app::parts {

using namespace ui;
using namespace ui::components;

std::shared_ptr<Box> sheetColumn(float gap) { return Box::create({.gap = gap, .padding = {24, 0, 24, 8}}); }

NodePtr scrolling(NodePtr column) {
    auto scroll = ScrollNode::create();
    scroll->setStyle({.shrink = 1});
    scroll->setFitContent(true);
    scroll->appendChild(std::move(column));
    return scroll;
}

NodePtr section(std::string label) {
    auto node = SectionLabel(std::move(label));
    node->updateStyle([](Style &s) { s.padding = {0, 10, 0, 2}; });
    return node;
}

std::shared_ptr<Text> bodyText(std::string text) {
    auto node = Text::create(std::move(text), currentTheme().type.body);
    node->bind([n = node.get()] {
        const Theme &t = theme();
        untrack([&] { n->setColor(ink(t, t.text.body)); });
    });
    return node;
}

gfx::TextStyle plainMono() {
    gfx::TextStyle style = withSize(currentTheme().type.label, 12);
    style.uppercase = false;
    style.letterSpacing = 0.02f;
    return style;
}

std::shared_ptr<Text> monoText(std::string text, int maxLines, bool caps) {
    auto node = Text::create(std::move(text), caps ? currentTheme().type.label : plainMono());
    if (maxLines > 0) {
        node->setMaxLines(maxLines);
        node->setOverflow(gfx::TextOverflow::ellipsisMiddle);
    }
    node->bind([n = node.get()] {
        const Theme &t = theme();
        untrack([&] { n->setColor(textInk(t, t.text.meta, 11)); });
    });
    return node;
}

NodePtr infoLine(InfoLine line) {
    const Theme &initial = currentTheme();
    auto row = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 12, .padding = {0, 6, 0, 6},
                            .minHeight = 48});
    auto texts = Box::create({.gap = 3, .grow = 1, .shrink = 1, .basis = 0});
    auto label = Text::create(line.label, initial.type.label);
    label->setMaxLines(1);
    auto value = Text::create({}, line.mono ? plainMono() : initial.type.body);
    texts->appendChild(label);
    texts->appendChild(value);
    row->appendChild(texts);
    std::shared_ptr<Icon> icon;
    if (line.onTap) {
        icon = Icon::create(line.icon, 20);
        row->appendChild(icon);
    }
    const bool mono = line.mono;
    Prop<std::string> text = line.value;
    row->bind([label = label.get(), value = value.get(), icon = icon.get(), text, mono] {
        const Theme &t = theme();
        const std::string v = text.get();
        untrack([&] {
            value->setText(v);
            label->setColor(textInk(t, t.text.label, 11));
            value->setColor(mono ? ink(t, t.text.body) : ink(t, t.text.primary));
            if (icon != nullptr) {
                icon->setColor(legible(t.colors.accent, t.colors.ground));
            }
        });
    });
    if (line.onTap) {
        // Done once: the icon turns into a check, since messages wait while the sheet is up.
        auto pressable = makePressable(*row, {.onTap = [onTap = std::move(line.onTap), icon = icon.get()] {
                                                  onTap();
                                                  icon->setIcon("check");
                                              }});
        row->setPaintHook([pressable](PaintContext &context) {
            if (pressable->pressed.peek()) {
                const gfx::Rect b = context.bounds();
                const gfx::RoundedRect shape{{-context.px(8), 0, b.width + context.px(16), b.height},
                                             gfx::CornerRadii::uniform(context.px(currentTheme().radii.listItem))};
                context.list.fill(shape, ink(currentTheme(), currentTheme().colors.pressed));
            }
        });
        row->bind([row = row.get(), pressable, name = line.label, text] {
            (void)pressable->pressed();
            const std::string v = text.get();
            untrack([&] {
                row->markPaintDirty();
                row->updateSemantics([&](Semantics &s) {
                    s.role = Role::button;
                    s.label = name + ", " + v;
                    s.mergeDescendants = true;
                });
            });
        });
    } else {
        row->updateSemantics([](Semantics &s) { s.mergeDescendants = true; });
    }
    return row;
}

FileItem fileItemFor(const vfs::Entry &entry, std::optional<std::uint64_t> itemCount) {
    const FileKind kind = kitKind(entry);
    return {.name = entry.name,
            .meta = metaLine(entry, itemCount),
            .icon = kindIcon(kind),
            .kindColor = kindColor(kind, currentTheme()),
            .spokenLabel = spokenDescription(entry, itemCount)};
}

NodePtr fileRow(const vfs::Entry &entry, std::string meta, std::string stamp) {
    auto line = Box::create({.direction = Direction::row, .alignItems = Align::center, .margin = {-16, 0, -16, 0}});
    auto row = FileRow::create();
    FileItem item = fileItemFor(entry);
    if (!meta.empty()) {
        item.meta = std::move(meta);
    }
    row->setData(item);
    row->setHitTestBehavior(HitTestBehavior::none);
    row->setFocusable(false);
    row->updateStyle([](Style &s) {
        s.grow = 1;
        s.shrink = 1;
        s.basis = 0;
    });
    line->appendChild(row);
    if (!stamp.empty()) {
        auto tag = Stamp(stamp, StampTone::secondary);
        tag->updateStyle([](Style &s) { s.margin = {0, 0, 16, 0}; });
        line->appendChild(tag);
    }
    return line;
}

NodePtr progressTrack(Prop<float> value) {
    auto node = Box::create({.height = 6});
    node->setDebugName("ProgressTrack");
    node->setPaintHook([value](PaintContext &context) {
        const Theme &theme = currentTheme();
        const gfx::Rect b = context.bounds();
        const gfx::RoundedRect track{b, gfx::CornerRadii::uniform(b.height / 2)};
        recipes::sunken(context.list, track, theme, context.density);
        const float v = untrack([&] { return value.get(); });
        if (v < 0) {
            if (!theme.style.marks) {
                // Still counting: the track tinted, with nothing to measure yet.
                context.list.fill(track, theme.colors.accent.withAlpha(0.30f));
                return;
            }
            context.list.pushClip(track);
            recipes::hazardStripes(context.list, track, theme.colors.accent.withAlpha(0.55f), context.density);
            context.list.pop();
            return;
        }
        const float width = std::max(b.height, b.width * std::clamp(v, 0.0f, 1.0f));
        const gfx::RoundedRect fill{{b.x, b.y, width, b.height}, gfx::CornerRadii::uniform(b.height / 2)};
        context.list.fill(fill, theme.colors.accent);
    });
    node->bind([n = node.get(), value] {
        const float v = value.get();
        (void)theme();
        untrack([&] {
            n->markPaintDirty();
            n->updateSemantics([&](Semantics &s) {
                s.role = Role::progress;
                s.value = v < 0 ? std::string() : std::to_string(static_cast<int>(v * 100 + 0.5f)) + "%";
            });
        });
    });
    return node;
}

NodePtr buttonRow(std::vector<NodePtr> buttons) {
    auto row = Box::create({.direction = Direction::row, .gap = 10, .padding = {0, 8, 0, 0}});
    for (auto &button : buttons) {
        button->updateStyle([](Style &s) {
            s.grow = 1;
            s.shrink = 1;
            s.basis = 0;
        });
        row->appendChild(std::move(button));
    }
    return row;
}

}  // namespace filepan::app::parts
