// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/components/buttons.hpp"

#include "filepan/ui/components/recipes.hpp"

namespace filepan::ui::components {

namespace {

constexpr float kButtonHeight = 44;
constexpr float kOutlineRadius = 6;

// Disabled: CSS saturate(0) brightness(.6).
gfx::Color disabledColor(gfx::Color c) { return grayscale(c, 0.6f); }

gfx::Color labelColor(const Theme &theme, ButtonVariant variant) {
    const ColorTokens &c = theme.colors;
    switch (variant) {
    case ButtonVariant::primary:
        return c.onAccent;
    case ButtonVariant::danger:
        return c.onDestructive;
    case ButtonVariant::glass:
        return c.ink;
    case ButtonVariant::outline:
    case ButtonVariant::text:
        // Accent text on the ground; lifted toward ink if the accent is too dark to read.
        return contrastRatio(c.accent, c.ground) >= kMinTextContrast ? c.accent : mix(c.accent, c.ink, 0.5f);
    }
    return c.ink;
}

}  // namespace

// --- Button ---

std::shared_ptr<Button> Button::create(ButtonProps props) {
    auto node = std::make_shared<Button>();
    Button *raw = node.get();
    node->variant_ = props.variant;
    node->enabled_ = props.enabled;
    const bool outline = props.variant == ButtonVariant::outline;
    const bool text = props.variant == ButtonVariant::text;
    node->setStyle({.direction = Direction::row,
                    .justify = Justify::center,
                    .alignItems = Align::center,
                    .gap = 8,
                    .padding = Edges::symmetric(text ? 12.0f : outline ? 16.0f : 22.0f, 0),
                    .height = outline ? 40.0f : kButtonHeight,
                    .minWidth = 64});
    node->setDebugName("Button");

    std::shared_ptr<Icon> icon;
    if (!props.icon.empty()) {
        icon = Icon::create(props.icon, 20);
        node->appendChild(icon);
    }
    const Theme &initial = currentTheme();
    gfx::TextStyle style = outline ? withWeight(withSize(initial.type.label, 12), 700) : initial.type.button;
    auto label = Text::create({}, style);
    label->setMaxLines(1);
    label->setStyle({.shrink = 1});
    label->updateSemantics([](Semantics &s) { s.hidden = true; });
    node->appendChild(label);

    node->pressable_ = makePressable(*node, {.onTap = std::move(props.onTap),
                                             .enabled = props.enabled,
                                             .haptic = props.haptic});

    Prop<std::string> labelProp = props.label;
    std::string a11y = props.accessibilityLabel;
    label->bind([l = label.get(), labelProp] { l->setText(labelProp.get()); });
    node->bind([raw, labelProp, a11y] {
        std::string text = a11y.empty() ? labelProp.get() : a11y;
        untrack([&] {
            raw->updateSemantics([&](Semantics &s) {
                s.role = Role::button;
                s.label = std::move(text);
            });
        });
    });
    const ButtonVariant variant = props.variant;
    // The utilitarian style: every kind 40 dp tall, its label in the plain button face.
    node->bind([raw, l = label.get(), outline] {
        const Theme &t = theme();
        untrack([&] {
            const bool dense = t.style.utilitarian;
            raw->updateStyle([&](Style &s) { s.height = dense ? 40.0f : outline ? 40.0f : kButtonHeight; });
            l->setTextStyle(outline && !dense ? withWeight(withSize(t.type.label, 12), 700) : t.type.button);
        });
    });
    node->bind([raw, l = label.get(), i = icon.get(), variant] {
        const Theme &t = theme();
        const bool on = raw->enabled_.get();
        const bool pressed = raw->pressable_->pressed();
        (void)raw->trackFocused();
        gfx::Color color = labelColor(t, variant);
        if (!on) {
            color = variant == ButtonVariant::glass ? ink(t, 0.5f)
                    : t.colors.light               ? ink(t, 0.45f)
                                                   : disabledColor(color);
        }
        untrack([&] {
            l->setColor(color);
            if (i != nullptr) {
                i->setColor(color);
            }
            raw->setTransform(pressed && !t.style.utilitarian ? gfx::Transform::translation({0, 1}) : gfx::Transform{});
            raw->markPaintDirty();
        });
    });
    return node;
}

gfx::RoundedRect Button::shape(const PaintContext &context) const {
    const float radius = variant_ == ButtonVariant::outline ? context.px(currentTheme().corner(kOutlineRadius))
                                                            : currentTheme().cornerPx(1e6f, context.density);
    return {context.bounds(), gfx::CornerRadii::uniform(radius)};
}

void Button::paint(PaintContext &context) {
    const Theme &theme = currentTheme();
    const ColorTokens &c = theme.colors;
    const bool on = untrack([this] { return enabled_.get(); });
    const bool pressed = pressable_->pressed.peek();
    const gfx::RoundedRect s = shape(context);
    switch (variant_) {
    case ButtonVariant::primary:
    case ButtonVariant::danger: {
        gfx::Color base = variant_ == ButtonVariant::primary ? c.accent : c.destructive;
        if (!on) {
            // Darkened on a dark ground; on a light one, a pale grey the dimmed label reads on.
            base = c.light ? mix(c.ground, c.ink, 0.12f) : disabledColor(base);
        }
        recipes::raised(context.list, s, base, context.density);
        break;
    }
    case ButtonVariant::glass:
        recipes::glass(context.list, s, theme, context.density);
        break;
    case ButtonVariant::outline: {
        const gfx::Color edge = on ? c.accent : disabledColor(c.accent);
        context.list.fill(s, edge.withAlpha(0.06f));
        context.list.stroke(s, context.px(1.5f), edge);
        break;
    }
    case ButtonVariant::text:
        break;
    }
    if (pressed) {
        if (variant_ == ButtonVariant::text || variant_ == ButtonVariant::outline) {
            context.list.fill(s, c.accent.withAlpha(0.14f));
        } else {
            recipes::pressedOverlay(context.list, s);
        }
    }
}

void Button::paintOverlay(PaintContext &context) {
    if (focused()) {
        recipes::focusRing(context.list, shape(context), currentTheme(), context.density);
    }
}

// --- IconButton ---

std::shared_ptr<IconButton> IconButton::create(IconButtonProps props) {
    auto node = std::make_shared<IconButton>();
    IconButton *raw = node.get();
    node->style_ = props.style;
    node->enabled_ = props.enabled;
    node->selected_ = props.selected;
    node->filled_ = props.filled;
    node->setStyle({.justify = Justify::center,
                    .alignItems = Align::center,
                    .width = props.size,
                    .height = props.size});
    node->setDebugName("IconButton");
    auto icon = Icon::create(props.icon, props.size >= 40 ? 24.0f : 20.0f, {1, 1, 1, 1},
                             props.style == IconButtonStyle::filled);
    node->appendChild(icon);
    node->pressable_ = makePressable(*node, {.onTap = std::move(props.onTap),
                                             .enabled = props.enabled,
                                             .haptic = props.haptic});
    node->updateSemantics([&](Semantics &s) {
        s.role = Role::button;
        s.label = props.label;
    });
    const IconButtonStyle style = props.style;
    const bool destructive = props.destructive;
    node->bind([raw, i = icon.get(), style, destructive] {
        const Theme &t = theme();
        const ColorTokens &c = t.colors;
        const bool on = raw->enabled_.get();
        const bool selected = raw->selected_.get();
        const bool filled = raw->filled_.get();
        const bool pressed = raw->pressable_->pressed();
        (void)raw->trackFocused();
        gfx::Color color = style == IconButtonStyle::filled ? c.onAccent
                           : destructive                    ? mix(c.destructive, c.ink, 0.25f)
                           : selected                       ? c.accent
                                                            : ink(t, 0.9f);
        if (!on) {
            color = style == IconButtonStyle::filled ? disabledColor(color) : ink(t, 0.4f);
        }
        untrack([&] {
            i->setColor(color);
            i->setFilled(style == IconButtonStyle::filled || selected || filled);
            setSemanticsFlag(*raw, semantics_flags::kSelected, selected);
            raw->setTransform(pressed ? gfx::Transform::translation({0, 1}) : gfx::Transform{});
            raw->markPaintDirty();
        });
    });
    return node;
}

void IconButton::paint(PaintContext &context) {
    const Theme &theme = currentTheme();
    const bool on = untrack([this] { return enabled_.get(); });
    const bool selected = untrack([this] { return selected_.get(); });
    const bool pressed = pressable_->pressed.peek();
    const gfx::RoundedRect s{context.bounds(), gfx::CornerRadii::uniform(theme.cornerPx(1e6f, context.density))};
    switch (style_) {
    case IconButtonStyle::glass:
        recipes::glass(context.list, s, theme, context.density);
        break;
    case IconButtonStyle::filled:
        recipes::raised(context.list, s, on ? theme.colors.accent : disabledColor(theme.colors.accent),
                        context.density);
        break;
    case IconButtonStyle::plain:
        if (selected) {
            context.list.fill(s, theme.colors.accent.withAlpha(0.14f));
        }
        break;
    }
    if (pressed) {
        recipes::pressedOverlay(context.list, s, style_ == IconButtonStyle::plain ? 0.12f : 0.10f);
    }
}

void IconButton::paintOverlay(PaintContext &context) {
    if (focused()) {
        const Theme &theme = currentTheme();
        recipes::focusRing(context.list,
                           {context.bounds(), gfx::CornerRadii::uniform(theme.cornerPx(1e6f, context.density))}, theme,
                           context.density);
    }
}

}  // namespace filepan::ui::components
