// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/theme.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace filepan::ui {

namespace {

using gfx::Color;

constexpr Color kWhite{1, 1, 1, 1};

TypeScale phoneTypeScale() {
    TypeScale type;
    type.display = {.family = gfx::FontFamily::sans, .size = 34, .weight = 800, .stretch = 62,
                    .letterSpacing = -0.01f, .lineHeight = 0.9f, .uppercase = true};
    type.title = {.family = gfx::FontFamily::sans, .size = 20, .weight = 750, .stretch = 75,
                  .letterSpacing = 0, .lineHeight = 1.15f, .uppercase = true};
    type.body = {.family = gfx::FontFamily::sans, .size = 15, .weight = 500, .stretch = 100,
                 .letterSpacing = 0, .lineHeight = 1.3f};
    type.bodySmall = {.family = gfx::FontFamily::sans, .size = 13, .weight = 400, .stretch = 100,
                      .letterSpacing = 0, .lineHeight = 1.3f};
    type.meta = {.family = gfx::FontFamily::sans, .size = 12, .weight = 450, .stretch = 100,
                 .letterSpacing = 0, .lineHeight = 1.3f};
    type.section = {.family = gfx::FontFamily::mono, .size = 11, .weight = 500, .stretch = 100,
                    .letterSpacing = 0.08f, .lineHeight = 1.3f, .uppercase = true};
    type.label = {.family = gfx::FontFamily::mono, .size = 11, .weight = 500, .stretch = 100,
                  .letterSpacing = 0.12f, .lineHeight = 1.3f, .uppercase = true};
    type.button = {.family = gfx::FontFamily::sans, .size = 14, .weight = 750, .stretch = 85,
                   .letterSpacing = 0.06f, .lineHeight = 1.2f, .uppercase = true};
    return type;
}

ColorTokens ultraviolet() {
    ColorTokens c;
    c.ground = Color::fromRgb(0x0A0612);
    c.groundDeep = Color::fromRgb(0x150D24);
    c.ink = Color::fromRgb(0xF1ECF7);
    c.accent = Color::fromRgb(0xFF4FC4);
    c.secondary = Color::fromRgb(0x9D5CFF);
    c.tertiary = Color::fromRgb(0x3D7BFF);
    c.highlight = Color::fromRgb(0x5FE0FF);
    c.destructive = Color::fromRgb(0xFF5A36);  // Orange-red, away from the pink and violet.
    c.spectrum[0] = c.accent;
    c.spectrum[1] = c.secondary;
    c.spectrum[2] = c.tertiary;
    c.glassTint = Color::fromRgb(0xFFEBFF);
    return c;
}

ColorTokens acid() {
    ColorTokens c;
    c.ground = Color::fromRgb(0x060708);
    c.groundDeep = Color::fromRgb(0x101214);
    c.ink = Color::fromRgb(0xECEDE8);
    c.accent = Color::fromRgb(0xC6FF1A);
    c.secondary = Color::fromRgb(0xFF2E88);
    c.tertiary = Color::fromRgb(0x3D6BFF);
    c.highlight = Color::fromRgb(0x22E6D2);
    c.destructive = Color::fromRgb(0xFF6A1A);  // Orange: the secondary is already a hot pink.
    c.spectrum[0] = c.accent;
    c.spectrum[1] = c.highlight;
    c.spectrum[2] = c.tertiary;
    return c;
}

// Clean: plain type in sentence case, no condensed widths or mono caps.
TypeScale cleanTypeScale() {
    TypeScale type;
    type.display = {.family = gfx::FontFamily::sans, .size = 22, .weight = 650, .stretch = 100,
                    .letterSpacing = -0.01f, .lineHeight = 1.05f};
    type.title = {.family = gfx::FontFamily::sans, .size = 17, .weight = 650, .stretch = 100,
                  .letterSpacing = 0, .lineHeight = 1.2f};
    type.body = {.family = gfx::FontFamily::sans, .size = 14, .weight = 500, .stretch = 100,
                 .letterSpacing = 0, .lineHeight = 1.3f};
    type.bodySmall = {.family = gfx::FontFamily::sans, .size = 13, .weight = 400, .stretch = 100,
                      .letterSpacing = 0, .lineHeight = 1.3f};
    type.meta = {.family = gfx::FontFamily::sans, .size = 12, .weight = 450, .stretch = 100,
                 .letterSpacing = 0, .lineHeight = 1.3f};
    type.section = {.family = gfx::FontFamily::sans, .size = 12, .weight = 650, .stretch = 100,
                    .letterSpacing = 0.01f, .lineHeight = 1.3f};
    type.label = {.family = gfx::FontFamily::sans, .size = 12, .weight = 550, .stretch = 100,
                  .letterSpacing = 0, .lineHeight = 1.3f};
    type.button = {.family = gfx::FontFamily::sans, .size = 14, .weight = 600, .stretch = 100,
                   .letterSpacing = 0.01f, .lineHeight = 1.2f};
    return type;
}

// Clean's colors: neutral greys, a muted blue, and the panels as plain bordered cards.
ColorTokens cleanDark() {
    ColorTokens c;
    c.ground = Color::fromRgb(0x141517);
    c.groundDeep = Color::fromRgb(0x1D1F22);
    c.ink = Color::fromRgb(0xE4E5E7);
    c.accent = Color::fromRgb(0x6E9BEF);
    c.secondary = Color::fromRgb(0xE3B341);
    c.tertiary = Color::fromRgb(0x5DA9A0);
    c.highlight = Color::fromRgb(0x8DB0E6);
    c.destructive = Color::fromRgb(0xEF5350);
    c.spectrum[0] = c.spectrum[1] = c.spectrum[2] = c.accent;
    c.gridAlpha = 0;
    return c;
}

ColorTokens cleanLight() {
    ColorTokens c;
    c.light = true;
    c.ground = Color::fromRgb(0xEDEDEB);
    c.groundDeep = Color::fromRgb(0xFFFFFF);
    c.ink = Color::fromRgb(0x1B1C1E);
    c.accent = Color::fromRgb(0x2F5FB8);
    c.secondary = Color::fromRgb(0x8A6400);
    c.tertiary = Color::fromRgb(0x2E7D74);
    c.highlight = Color::fromRgb(0x3B6FB6);
    c.destructive = Color::fromRgb(0xC62828);
    c.spectrum[0] = c.spectrum[1] = c.spectrum[2] = c.accent;
    c.glassTint = Color{0, 0, 0, 1};
    c.film = Color{0, 0, 0, 1};
    c.wellAlpha = 0.06f;
    c.gridAlpha = 0;
    c.scrim = 0.40f;
    return c;
}

// Dark ink on a light ground needs a higher floor than the reverse for the same contrast.
TextOpacity cleanLightText() {
    TextOpacity text;
    text.body = 0.88f;
    text.label = 0.80f;
    text.meta = 0.72f;
    text.quiet = 0.68f;
    text.minimum = 0.66f;
    return text;
}

// Warm danger colors that read on every ground, tried in order when a preset's own danger color is
// too close to an accent the user picked. The design doc's magentas sat next to the pink accents,
// so danger is a red-orange family here.
constexpr std::uint32_t kDangerCandidates[] = {0xFF5A36, 0xFF6A1A, 0xFF4D5A, 0xFFB020,
                                               0xFFC21A, 0xFF3DA5, 0xD94DFF};

// Hue in degrees [0, 360) and HSV saturation [0, 1].
void hueAndSaturation(Color c, float &hue, float &saturation) {
    const float maxC = std::max({c.r, c.g, c.b});
    const float minC = std::min({c.r, c.g, c.b});
    const float delta = maxC - minC;
    saturation = maxC > 0 ? delta / maxC : 0;
    if (delta <= 0) {
        hue = 0;
        return;
    }
    if (maxC == c.r) {
        hue = 60 * std::fmod((c.g - c.b) / delta, 6.0f);
    } else if (maxC == c.g) {
        hue = 60 * ((c.b - c.r) / delta + 2);
    } else {
        hue = 60 * ((c.r - c.g) / delta + 4);
    }
    if (hue < 0) {
        hue += 360;
    }
}

float linearize(float channel) {
    return channel <= 0.04045f ? channel / 12.92f : std::pow((channel + 0.055f) / 1.055f, 2.4f);
}

}  // namespace

float hueDistance(Color a, Color b) {
    float hueA = 0, satA = 0, hueB = 0, satB = 0;
    hueAndSaturation(a, hueA, satA);
    hueAndSaturation(b, hueB, satB);
    if (satA < 0.2f || satB < 0.2f) {
        return 180;
    }
    const float d = std::fabs(hueA - hueB);
    return d > 180 ? 360 - d : d;
}

float relativeLuminance(Color color) {
    return 0.2126f * linearize(color.r) + 0.7152f * linearize(color.g) + 0.0722f * linearize(color.b);
}

Color composite(Color color, Color background) {
    // Blending happens in sRGB space, like the renderer and CSS.
    const float a = color.a;
    return {color.r * a + background.r * (1 - a), color.g * a + background.g * (1 - a),
            color.b * a + background.b * (1 - a), 1};
}

float contrastRatio(Color foreground, Color background) {
    const Color fg = foreground.a < 1 ? composite(foreground, background) : foreground;
    const float l1 = relativeLuminance(fg);
    const float l2 = relativeLuminance(background);
    const float lighter = l1 > l2 ? l1 : l2;
    const float darker = l1 > l2 ? l2 : l1;
    return (lighter + 0.05f) / (darker + 0.05f);
}

Color pickInk(Color background, Color dark, Color light, float minimum) {
    const float darkContrast = contrastRatio(dark, background);
    const float lightContrast = contrastRatio(light, background);
    if (darkContrast >= minimum) {
        return dark;
    }
    if (lightContrast >= minimum) {
        return light;
    }
    return darkContrast >= lightContrast ? dark : light;
}

Theme makeTheme(ThemePreset preset, std::optional<Color> accentOverride, bool night) {
    Theme theme;
    theme.preset = preset;
    theme.type = phoneTypeScale();
    switch (preset) {
    case ThemePreset::ultraviolet:
        theme.colors = ultraviolet();
        break;
    case ThemePreset::acid:
        theme.colors = acid();
        break;
    case ThemePreset::clean:
        theme.colors = night ? cleanDark() : cleanLight();
        if (!night) {
            theme.text = cleanLightText();
        }
        theme.type = cleanTypeScale();
        theme.style = {.backdropBlur = false, .marks = false, .hardShadow = false, .utilitarian = true};
        theme.cornerCap = 2;
        theme.radii = {.sheet = 2, .panel = 2, .input = 2, .listItem = 0, .tile = 2, .iconTile = 2, .band = 0,
                       .tag = 2, .pill = 2};
        // One-line rows, bars on the screen edges.
        theme.spacing.listRow = 44;
        theme.spacing.listRowCompact = 44;
        theme.spacing.iconTile = 24;
        theme.spacing.rowInset = 12;
        theme.spacing.gridCellMin = 80;
        theme.spacing.appBar = 48;
        theme.spacing.dock = 52;
        theme.spacing.dockInset = 0;
        theme.spacing.statusLine = 26;
        break;
    }
    ColorTokens &c = theme.colors;
    if (accentOverride) {
        c.accent = accentOverride->withAlpha(1);
        c.spectrum[0] = c.accent;
    }
    // Keep danger clearly apart from the accent and secondary, including a user-picked accent.
    const auto distinct = [&c](Color danger) {
        return hueDistance(danger, c.accent) >= kMinDangerHueGap &&
               hueDistance(danger, c.secondary) >= kMinDangerHueGap;
    };
    if (!distinct(c.destructive)) {
        // The first candidate clear of both; failing that, the one farthest from both that still
        // keeps clear of the accent, which matters most.
        float best = -1;
        for (const std::uint32_t rgb : kDangerCandidates) {
            const Color candidate = Color::fromRgb(rgb);
            if (distinct(candidate)) {
                c.destructive = candidate;
                break;
            }
            const float accentGap = hueDistance(candidate, c.accent);
            const float gap = std::min(accentGap, hueDistance(candidate, c.secondary));
            if (accentGap >= kMinDangerHueGap && gap > best) {
                best = gap;
                c.destructive = candidate;
            }
        }
    }
    // Dark ink is the ground itself (the ink, on a light ground); light ink is white, which beats
    // the slightly tinted bone on mid-tone fills.
    const Color dark = c.light ? c.ink : c.ground;
    c.onAccent = pickInk(c.accent, dark, kWhite);
    c.onSecondary = pickInk(c.secondary, dark, kWhite);
    c.onDestructive = pickInk(c.destructive, dark, kWhite);
    return theme;
}

}  // namespace filepan::ui
