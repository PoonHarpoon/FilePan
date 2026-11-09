// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/components/marks.hpp"

#include <algorithm>
#include <cmath>

#include "filepan/ui/components/recipes.hpp"
#include "filepan/ui/components/surfaces.hpp"

namespace filepan::ui::components {

namespace {

constexpr float kBandHeight = 40;

gfx::Color toneFill(const Theme &theme, StampTone tone) {
    const ColorTokens &c = theme.colors;
    switch (tone) {
    case StampTone::accent:
        return c.accent;
    case StampTone::secondary:
        return c.secondary;
    case StampTone::ink:
        return c.ink;
    case StampTone::destructive:
        return c.destructive;
    }
    return c.accent;
}

gfx::Color toneInk(const Theme &theme, StampTone tone) {
    const ColorTokens &c = theme.colors;
    switch (tone) {
    case StampTone::accent:
        return c.onAccent;
    case StampTone::secondary:
        return c.onSecondary;
    case StampTone::ink:
        return c.ground;
    case StampTone::destructive:
        return c.onDestructive;
    }
    return c.onAccent;
}

Style overlayStyle() {
    Style style;
    style.position = Position::absolute;
    style.left = 0;
    style.top = 0;
    style.right = 0;
    style.bottom = 0;
    return style;
}

std::shared_ptr<Text> tagText(const Prop<std::string> &text) {
    auto label = Text::create({}, stampStyle(currentTheme()));
    label->setMaxLines(1);
    label->bind([l = label.get(), text] { l->setText(text.get()); });
    return label;
}

}  // namespace

NodePtr Stamp(Prop<std::string> text, StampTone tone, std::string icon) {
    auto node = Box::create({.direction = Direction::row,
                             .alignItems = Align::center,
                             .gap = 4,
                             .padding = Edges::symmetric(6, 0),
                             .height = 20});
    node->setDebugName("Stamp");
    std::shared_ptr<Icon> glyph;
    if (!icon.empty()) {
        glyph = Icon::create(std::move(icon), 14);
        node->appendChild(glyph);
    }
    auto label = tagText(text);
    node->appendChild(label);
    node->setPaintHook([tone](PaintContext &context) {
        const Theme &theme = currentTheme();
        context.list.fill(gfx::RoundedRect{context.bounds(), gfx::CornerRadii::uniform(context.px(theme.radii.tag))},
                          toneFill(theme, tone));
    });
    node->bind([n = node.get(), l = label.get(), g = glyph.get(), tone, text] {
        const Theme &th = theme();
        const std::string value = text.get();
        untrack([&] {
            l->setColor(toneInk(th, tone));
            if (g != nullptr) {
                g->setColor(toneInk(th, tone));
            }
            n->markPaintDirty();
            n->updateSemantics([&](Semantics &s) { s.label = value; });
            l->updateSemantics([](Semantics &s) { s.hidden = true; });
        });
    });
    return node;
}

NodePtr OutlineTag(Prop<std::string> text) {
    auto node = Box::create({.direction = Direction::row, .alignItems = Align::center, .padding = Edges::symmetric(6, 0), .height = 20});
    node->setDebugName("OutlineTag");
    auto label = tagText(text);
    node->appendChild(label);
    node->setPaintHook([](PaintContext &context) {
        const Theme &theme = currentTheme();
        context.list.stroke(gfx::RoundedRect{context.bounds(), gfx::CornerRadii::uniform(context.px(theme.radii.tag))},
                            std::max(1.0f, context.density * 0.75f), ink(theme, 0.5f));
    });
    node->bind([l = label.get()] {
        const Theme &th = theme();
        untrack([&] { l->setColor(textInk(th, th.text.label, 10)); });
    });
    return node;
}

NodePtr GlassChip(GlassChipProps props) {
    auto node = Box::create({.direction = Direction::row,
                             .alignItems = Align::center,
                             .gap = 6,
                             .padding = Edges::symmetric(12, 0),
                             .height = 32});
    node->setDebugName("GlassChip");
    std::shared_ptr<Icon> glyph;
    if (!props.icon.empty()) {
        glyph = Icon::create(props.icon, 16);
        node->appendChild(glyph);
    }
    auto label = Text::create({}, currentTheme().type.label);
    label->setMaxLines(1);
    label->updateSemantics([](Semantics &s) { s.hidden = true; });
    node->appendChild(label);
    std::shared_ptr<Pressable> pressable;
    if (props.onTap) {
        pressable = makePressable(*node, {.onTap = props.onTap});
    }
    Prop<bool> selected = props.selected;
    const bool dot = props.dot;
    if (dot) {
        node->updateStyle([](Style &s) { s.padding.left = 22; });
    }
    node->setPaintHook([selected, dot, pressable](PaintContext &context) {
        const Theme &theme = currentTheme();
        const gfx::RoundedRect shape{context.bounds(), gfx::CornerRadii::uniform(theme.cornerPx(1e6f, context.density))};
        const bool on = untrack([&] { return selected.get(); });
        recipes::glass(context.list, shape, theme, context.density);
        if (on) {
            context.list.fill(shape, theme.colors.accent.withAlpha(0.16f));
            context.list.stroke(shape, std::max(1.0f, context.density * 0.75f), theme.colors.accent.withAlpha(0.7f));
        }
        if (dot) {
            const float r = context.px(3.5f);
            const gfx::RoundedRect d{{context.px(12) - r, context.size.height / 2 - r, 2 * r, 2 * r},
                                     gfx::CornerRadii::uniform(r)};
            context.list.fill(d, theme.colors.accent);
        }
        if (pressable && pressable->pressed.peek()) {
            recipes::pressedOverlay(context.list, shape);
        }
    });
    Prop<std::string> text = props.text;
    node->bind([n = node.get(), l = label.get(), g = glyph.get(), text, selected, pressable] {
        const Theme &th = theme();
        const std::string value = text.get();
        const bool on = selected.get();
        if (pressable) {
            (void)pressable->pressed();
        }
        untrack([&] {
            l->setText(value);
            const gfx::Color color = on ? legible(th.colors.accent, th.colors.ground) : textInk(th, th.text.body, 11);
            l->setColor(color);
            if (g != nullptr) {
                g->setColor(color);
            }
            n->markPaintDirty();
            n->updateSemantics([&](Semantics &s) {
                s.label = value;
                if (pressable) {
                    s.role = Role::button;
                }
            });
            setSemanticsFlag(*n, semantics_flags::kSelected, on);
        });
    });
    return node;
}

NodePtr BarcodeTag(std::string id) {
    auto node = Box::create({.direction = Direction::row,
                             .alignItems = Align::center,
                             .gap = 8,
                             .padding = Edges::symmetric(8, 0),
                             .height = 28});
    node->setDebugName("BarcodeTag");
    node->appendChild(Barcode(id, 44, 16));
    auto label = tagText(id);
    node->appendChild(label);
    node->setPaintHook([](PaintContext &context) {
        const Theme &theme = currentTheme();
        const gfx::RoundedRect shape{context.bounds(), gfx::CornerRadii::uniform(context.px(theme.radii.tag))};
        context.list.fill(shape, theme.colors.ink.withAlpha(0.04f));
        context.list.stroke(shape, std::max(1.0f, context.density * 0.75f), ink(theme, 0.4f));
    });
    node->updateSemantics([&](Semantics &s) { s.label = id; });
    label->updateSemantics([](Semantics &s) { s.hidden = true; });
    node->bind([l = label.get()] {
        const Theme &th = theme();
        untrack([&] { l->setColor(textInk(th, th.text.body, 10)); });
    });
    return node;
}

NodePtr CornerTicks(float inset, float length) {
    auto node = Box::create(overlayStyle());
    node->setDebugName("CornerTicks");
    node->setHitTestBehavior(HitTestBehavior::none);
    node->updateSemantics([](Semantics &s) { s.hidden = true; });
    node->setPaintHook([inset, length](PaintContext &context) {
        if (currentTheme().style.marks) {
            recipes::cornerTicks(context.list, context.bounds(), ink(currentTheme(), 0.48f), context.density, inset,
                                 length);
        }
    });
    return node;
}

NodePtr HazardStripes(float width, float height) {
    auto node = Box::create({.width = width, .height = height});
    node->setDebugName("HazardStripes");
    node->updateSemantics([](Semantics &s) { s.hidden = true; });
    node->setPaintHook([](PaintContext &context) {
        const Theme &theme = currentTheme();
        if (!theme.style.marks) {
            return;
        }
        recipes::hazardStripes(context.list, {context.bounds(), gfx::CornerRadii::uniform(context.px(2))},
                               ink(theme, 0.85f), context.density);
    });
    return node;
}

NodePtr Barcode(std::string seed, float width, float height) {
    auto node = Box::create({.width = width, .height = height});
    node->setDebugName("Barcode");
    node->updateSemantics([](Semantics &s) { s.hidden = true; });
    node->setPaintHook([seed = std::move(seed)](PaintContext &context) {
        if (currentTheme().style.marks) {
            recipes::barcode(context.list, context.bounds(), ink(currentTheme(), 0.8f), seed, context.density);
        }
    });
    return node;
}

NodePtr SectionLabel(std::string label) {
    auto node = Box::create({.justify = Justify::end, .padding = {20, 0, 20, 6}, .height = 36});
    node->setDebugName("SectionLabel");
    auto text = Text::create(label, currentTheme().type.section);
    text->setMaxLines(1);
    text->updateSemantics([](Semantics &s) { s.hidden = true; });
    node->appendChild(text);
    node->updateSemantics([&](Semantics &s) {
        s.role = Role::heading;
        s.label = label;
    });
    node->bind([t = text.get()] {
        const Theme &th = theme();
        untrack([&] {
            t->setTextStyle(th.type.section);
            t->setColor(ink(th, inkAlpha(th.text.quiet)));
        });
    });
    return node;
}

// --- DataBand ---

std::shared_ptr<DataBand> DataBand::create(DataBandProps props) {
    auto node = std::make_shared<DataBand>();
    DataBand *raw = node.get();
    node->setDebugName("DataBand");
    node->setStyle({.direction = Direction::row,
                    .alignItems = Align::center,
                    .gap = 10,
                    .padding = Edges::symmetric(16, 0),
                    .height = kBandHeight});
    node->progress_ = props.progress;
    const Theme &initial = currentTheme();
    auto glyph = Icon::create({}, 18);
    gfx::TextStyle body = initial.type.meta;
    body.weight = 550;
    auto text = Text::create({}, body);
    text->setMaxLines(1);
    text->setStyle({.grow = 1, .shrink = 1, .basis = 0});
    gfx::TextStyle mono = withSize(initial.type.label, 11.5f);
    mono.uppercase = false;
    mono.letterSpacing = 0;
    auto rate = Text::create({}, mono);
    rate->setMaxLines(1);
    for (Node *child : {static_cast<Node *>(glyph.get()), static_cast<Node *>(text.get()),
                        static_cast<Node *>(rate.get())}) {
        child->updateSemantics([](Semantics &s) { s.hidden = true; });
    }
    node->appendChild(glyph);
    node->appendChild(text);
    node->appendChild(rate);
    if (props.onTap) {
        node->pressable_ = makePressable(*node, {.onTap = std::move(props.onTap)});
    }
    std::string label = props.label;
    Prop<std::string> textProp = props.text;
    Prop<std::string> rateProp = props.rate;
    Prop<std::string> iconProp = props.icon;
    node->bind([raw, g = glyph.get(), t = text.get(), r = rate.get(), label, textProp, rateProp, iconProp] {
        const std::string value = textProp.get();
        const std::string speed = rateProp.get();
        const std::string icon = iconProp.get();
        const float progress = raw->progress_.get();
        const Theme &th = theme();
        if (raw->pressable_) {
            (void)raw->pressable_->pressed();
        }
        (void)raw->trackFocused();
        untrack([&] {
            g->setIcon(icon);
            g->setColor(legible(th.colors.accent, th.colors.ground));
            t->setText(value);
            t->setColor(ink(th, th.text.primary));
            r->setText(speed);
            r->setColor(ink(th, 0.70f));
            r->updateStyle([&](Style &s) { s.display = !speed.empty(); });
            raw->markPaintDirty();
            raw->updateSemantics([&](Semantics &s) {
                s.role = raw->pressable_ ? Role::button : Role::progress;
                std::string spoken = label.empty() ? value : label + ", " + value;
                if (!speed.empty()) {
                    spoken += ", " + speed;
                }
                s.label = spoken;
                s.value = progress >= 0 ? std::to_string(static_cast<int>(std::lround(progress * 100))) + "%" : "";
                s.flags |= semantics_flags::kLiveRegion;
            });
        });
    });
    return node;
}

void DataBand::paint(PaintContext &context) {
    const Theme &theme = currentTheme();
    const ColorTokens &c = theme.colors;
    const gfx::Rect bounds = context.bounds();
    const gfx::RoundedRect shape{bounds, gfx::CornerRadii::uniform(context.px(theme.radii.band))};
    recipes::glass(context.list, shape, theme, context.density);
    const float progress = untrack([this] { return progress_.get(); });
    if (progress >= 0) {
        const float right = std::round(bounds.width * std::clamp(progress, 0.0f, 1.0f));
        const float edge = context.px(2);
        context.list.pushClip(shape);
        context.list.fill(gfx::Rect{0, 0, right, bounds.height}, c.accent.withAlpha(0.20f));
        if (right > 0) {
            context.list.fill(gfx::Rect{std::max(0.0f, right - edge), 0, std::min(edge, right), bounds.height}, c.accent);
        }
        context.list.pop();
    }
    if (pressable_ && pressable_->pressed.peek()) {
        context.list.fill(shape, c.ink.withAlpha(c.pressed * 0.6f));
    }
}

void DataBand::paintOverlay(PaintContext &context) {
    if (focused()) {
        const Theme &theme = currentTheme();
        recipes::focusRing(context.list, {context.bounds(), gfx::CornerRadii::uniform(context.px(theme.radii.band))},
                           theme, context.density);
    }
}

}  // namespace filepan::ui::components
