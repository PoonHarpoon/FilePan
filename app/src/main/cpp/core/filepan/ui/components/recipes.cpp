// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/components/recipes.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "filepan/ui/components/common.hpp"

namespace filepan::ui::components::recipes {

namespace {

using gfx::Color;
using gfx::RoundedRect;

constexpr Color kBlack{0, 0, 0, 1};

gfx::Shadow shadow(float x, float y, float blur, float spread, Color color, float density, bool inset = false) {
    return {{x * density, y * density}, blur * density, spread * density, color, inset};
}

RoundedRect grow(const RoundedRect &shape, float amount) {
    RoundedRect out = shape;
    out.rect = shape.rect.inset(-amount);
    auto r = [amount](float radius) { return std::max(0.0f, radius + amount); };
    out.radii = {r(shape.radii.topLeft), r(shape.radii.topRight), r(shape.radii.bottomRight),
                 r(shape.radii.bottomLeft)};
    return out;
}

}  // namespace

gfx::LinearGradient verticalGradient(const gfx::Rect &rect, gfx::GradientStops stops) {
    return {{rect.x, rect.y}, {rect.x, rect.bottom()}, stops};
}

gfx::LinearGradient horizontalGradient(const gfx::Rect &rect, gfx::GradientStops stops) {
    return {{rect.x, rect.y}, {rect.right(), rect.y}, stops};
}

void hardShadow(gfx::DisplayList &list, const RoundedRect &shape, float density) {
    if (!currentTheme().style.hardShadow) {
        return;
    }
    list.shadow(shape, shadow(0, 2, 0, 0, kBlack.withAlpha(0.45f), density));
}

void glass(gfx::DisplayList &list, const RoundedRect &rounded, const Theme &theme, float density) {
    const ColorTokens &c = theme.colors;
    RoundedRect shape = rounded;
    const auto cap = [&](float radius) { return theme.cornerPx(radius, density); };
    shape.radii = {cap(rounded.radii.topLeft), cap(rounded.radii.topRight), cap(rounded.radii.bottomRight),
                   cap(rounded.radii.bottomLeft)};
    if (theme.style.utilitarian) {
        // A flat panel with a hairline.
        list.fill(shape, c.groundDeep);
        list.stroke(shape, std::max(1.0f, std::round(density * 0.5f)), c.ink.withAlpha(c.light ? 0.16f : 0.14f));
        return;
    }
    hardShadow(list, shape, density);
    list.fill(shape, c.groundDeep);
    list.fill(shape, c.glassTint.withAlpha(0.05f));
    list.stroke(shape, density, c.glassTint.withAlpha(0.13f));
}

void sunken(gfx::DisplayList &list, const RoundedRect &shape, const Theme &theme, float density) {
    list.fill(shape, kBlack.withAlpha(theme.colors.wellAlpha));
    // A faint ink film so the well reads on the darkest grounds.
    list.fill(shape, theme.colors.ink.withAlpha(0.025f));
    list.stroke(shape, density, theme.colors.film.withAlpha(0.07f));
}

void raised(gfx::DisplayList &list, const RoundedRect &shape, Color base, float density) {
    hardShadow(list, shape, density);
    list.fill(shape, base);
    list.stroke(shape, density, kBlack.withAlpha(0.25f));
}

void knob(gfx::DisplayList &list, const RoundedRect &shape, float density) {
    hardShadow(list, shape, density);
    list.fill(shape, Color::fromRgb(0xE4E1EA));
    list.stroke(shape, density, kBlack.withAlpha(0.2f));
}

void focusRing(gfx::DisplayList &list, const RoundedRect &shape, const Theme &theme, float density) {
    const Color accent = theme.colors.accent;
    list.stroke(grow(shape, 2 * density), 2 * density, accent);
}

void pressedOverlay(gfx::DisplayList &list, const RoundedRect &shape, float amount) {
    list.fill(shape, currentTheme().colors.film.withAlpha(amount));
}

void cornerTicks(gfx::DisplayList &list, const gfx::Rect &rect, Color color, float density, float inset,
                 float length) {
    const float t = std::max(1.0f, std::round(density));
    const float i = inset * density;
    const float l = length * density;
    const float left = rect.x + i;
    const float top = rect.y + i;
    const float right = rect.right() - i;
    const float bottom = rect.bottom() - i;
    // Each corner: a horizontal and a vertical arm meeting at the corner point.
    list.fill(gfx::Rect{left, top, l, t}, color);
    list.fill(gfx::Rect{left, top + t, t, l - t}, color);
    list.fill(gfx::Rect{right - l, top, l, t}, color);
    list.fill(gfx::Rect{right - t, top + t, t, l - t}, color);
    list.fill(gfx::Rect{left, bottom - t, l, t}, color);
    list.fill(gfx::Rect{left, bottom - l, t, l - t}, color);
    list.fill(gfx::Rect{right - l, bottom - t, l, t}, color);
    list.fill(gfx::Rect{right - t, bottom - l, t, l - t}, color);
}

void hazardStripes(gfx::DisplayList &list, const RoundedRect &shape, Color color, float density) {
    // repeating-linear-gradient(-45deg, ink 0 6px, transparent 6px 12px), at phone scale.
    list.fill(shape, gfx::Stripes{.angle = std::numbers::pi_v<float> / 4,
                                  .width = 4 * density,
                                  .period = 8 * density,
                                  .phase = 0,
                                  .color = color});
}

void barcode(gfx::DisplayList &list, const gfx::Rect &rect, Color color, std::string_view seed, float density) {
    std::uint32_t hash = 2166136261u;
    for (char ch : seed) {
        hash = (hash ^ static_cast<unsigned char>(ch)) * 16777619u;
    }
    const float unit = std::max(1.0f, std::round(density));
    float x = rect.x;
    bool bar = true;
    while (x < rect.right()) {
        hash ^= hash << 13;
        hash ^= hash >> 17;
        hash ^= hash << 5;
        const float width = static_cast<float>(1 + hash % (bar ? 3 : 2)) * unit;
        const float w = std::min(width, rect.right() - x);
        if (bar) {
            list.fill(gfx::Rect{x, rect.y, w, rect.height}, color);
        }
        x += width;
        bar = !bar;
    }
}

void crosshair(gfx::DisplayList &list, gfx::Vec2 center, float radius, Color color, float density) {
    const float t = std::max(1.0f, std::round(density * 0.75f));
    const RoundedRect ring{{center.x - radius, center.y - radius, radius * 2, radius * 2},
                           gfx::CornerRadii::uniform(radius)};
    list.stroke(ring, t, color);
    const float reach = radius * 1.45f;
    const float gap = radius * 0.45f;
    list.fill(gfx::Rect{center.x - t / 2, center.y - reach, t, reach - gap}, color);
    list.fill(gfx::Rect{center.x - t / 2, center.y + gap, t, reach - gap}, color);
    list.fill(gfx::Rect{center.x - reach, center.y - t / 2, reach - gap, t}, color);
    list.fill(gfx::Rect{center.x + gap, center.y - t / 2, reach - gap, t}, color);
    const float dot = std::max(2.0f, density * 1.5f);
    list.fill(RoundedRect{{center.x - dot, center.y - dot, dot * 2, dot * 2}, gfx::CornerRadii::uniform(dot)}, color);
}

}  // namespace filepan::ui::components::recipes
