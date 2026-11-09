// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/components/surfaces.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "filepan/ui/components/common.hpp"
#include "filepan/ui/components/recipes.hpp"

namespace filepan::ui::components {

namespace {

Style fill(Style style) {
    style.position = Position::absolute;
    style.left = 0;
    style.top = 0;
    style.right = 0;
    style.bottom = 0;
    return style;
}

}  // namespace

// --- Background ---

std::shared_ptr<Background> Background::create(BackgroundProps props) {
    auto node = std::make_shared<Background>();
    node->props_ = props;
    node->setStyle(fill({}));
    node->setHitTestBehavior(HitTestBehavior::none);
    node->updateSemantics([](Semantics &s) { s.hidden = true; });
    return node;
}

void Background::paint(PaintContext &context) {
    const Theme &theme = currentTheme();
    const ColorTokens &c = theme.colors;
    const gfx::Rect bounds = context.bounds();
    const float w = bounds.width;
    const float h = bounds.height;
    context.list.fill(bounds, c.ground);
    if (props_.grid && c.gridAlpha > 0) {
        const float pitch = context.px(theme.spacing.grid);
        const float t = std::max(1.0f, std::round(context.density * 0.5f));
        const gfx::Color line = c.ink.withAlpha(c.gridAlpha);
        for (float x = std::fmod(w / 2, pitch); x < w; x += pitch) {
            context.list.fill(gfx::Rect{std::round(x), 0, t, h}, line);
        }
        for (float y = pitch; y < h; y += pitch) {
            context.list.fill(gfx::Rect{0, std::round(y), w, t}, line);
        }
    }
    context.list.backgroundEnd();
}

NodePtr BottomFade(float height, float solid) {
    solid = std::clamp(solid, 0.0f, height);
    auto node = Box::create();
    node->setDebugName("BottomFade");
    node->setHitTestBehavior(HitTestBehavior::none);
    node->updateSemantics([](Semantics &s) { s.hidden = true; });
    node->bind([raw = node.get(), height] {
        const float inset = environment().safeArea().bottom / environment().density();
        untrack([&] {
            raw->setStyle({.height = height + inset, .position = Position::absolute, .left = 0, .right = 0, .bottom = 0});
        });
    });
    // The fade shows the screen's own background (the cached BlurSource::background copy, grid
    // and all) rather than a plain ground, so the solid part hides rows completely without a seam
    // in the grid. The ramp above it is that copy in thin steps of rising opacity. A preset without
    // backdrop blur paints the plain ground instead.
    node->setPaintHook([solid](PaintContext &context) {
        constexpr int kSteps = 28;
        constexpr float kSigma = 2;
        const gfx::Rect b = context.bounds();
        const float solidTop = b.height - context.px(solid) -
                               environment().safeArea.peek().bottom;  // The navigation bar is solid too.
        const float rampTop = 0;
        const float ramp = std::max(0.0f, solidTop - rampTop);
        const bool blur = currentTheme().style.backdropBlur;
        const gfx::Color ground = currentTheme().colors.ground;
        for (int i = 0; i < kSteps && ramp > 0; ++i) {
            const float t0 = static_cast<float>(i) / kSteps;
            const float t1 = static_cast<float>(i + 1) / kSteps;
            // Smoothstep: eases in from transparent and into the solid part.
            const float t = (t0 + t1) / 2;
            const float alpha = t * t * (3 - 2 * t);
            const float y0 = std::floor(rampTop + ramp * t0);
            const float y1 = std::floor(rampTop + ramp * t1);
            if (y1 <= y0) {
                continue;
            }
            const gfx::Rect step{0, y0, b.width, y1 - y0};
            if (!blur) {
                context.list.fill(step, ground.withAlpha(alpha));
                continue;
            }
            context.list.pushOpacity(alpha);
            context.list.backdropBlur({step, {}}, context.px(kSigma), 1,
                                      gfx::BlurSource::background);
            context.list.pop();
        }
        const gfx::Rect solidRect{0, std::floor(std::max(0.0f, solidTop)), b.width,
                                  b.height - std::floor(std::max(0.0f, solidTop))};
        if (!blur) {
            context.list.fill(solidRect, ground);
            return;
        }
        context.list.backdropBlur({solidRect, {}}, context.px(kSigma), 1, gfx::BlurSource::background);
    });
    return node;
}

// --- Glass ---

std::shared_ptr<Glass> Glass::create(Style style, GlassProps props) {
    auto node = std::make_shared<Glass>();
    node->setStyle(style);
    node->props_ = props;
    node->setHitTestBehavior(HitTestBehavior::opaque);
    if (props.clip) {
        // The clip's corners are capped like the panel's.
        node->bind([raw = node.get()] {
            const Theme &t = theme();
            untrack([&] { raw->updateClip(t); });
        });
    }
    return node;
}

void Glass::setProps(GlassProps props) {
    props_ = props;
    updateClip(currentTheme());
    markPaintDirty();
}

void Glass::updateClip(const Theme &theme) {
    if (props_.clip) {
        setClip(true, theme.corners(props_.radii));
    }
}

void Glass::paint(PaintContext &context) {
    const Theme &theme = currentTheme();
    recipes::glass(context.list, {context.bounds(), context.px(theme.corners(props_.radii))}, theme, context.density);
}

void Glass::paintOverlay(PaintContext &context) {
    if (props_.cornerTicks && currentTheme().style.marks) {
        const Theme &theme = currentTheme();
        recipes::cornerTicks(context.list, context.bounds(), ink(theme, 0.48f), context.density);
    }
}

std::shared_ptr<Glass> GlassPanel(Style style, float radius) {
    return Glass::create(style, {.radii = gfx::CornerRadii::uniform(radius)});
}

// --- Icon ---

std::shared_ptr<Icon> Icon::create(std::string name, float sizeDp, gfx::Color color, bool filled) {
    auto node = std::make_shared<Icon>();
    node->setText(std::move(name));
    node->setTextStyle({.family = gfx::FontFamily::icons, .size = sizeDp, .weight = 400, .lineHeight = 1,
                        .iconFill = filled ? 1.0f : 0.0f});
    node->setColor(color);
    node->setMaxLines(1);
    node->setOverflow(gfx::TextOverflow::clip);
    node->updateSemantics([](Semantics &s) { s.hidden = true; });
    return node;
}

void Icon::setFilled(bool filled) {
    gfx::TextStyle style = textStyle();
    style.iconFill = filled ? 1.0f : 0.0f;
    setTextStyle(style);
}

void Icon::setSize(float sizeDp) {
    gfx::TextStyle style = textStyle();
    style.size = sizeDp;
    setTextStyle(style);
}

}  // namespace filepan::ui::components
