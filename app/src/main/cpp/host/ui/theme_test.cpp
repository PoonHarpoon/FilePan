// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

#include "filepan/ui/theme.hpp"
#include "ui_test_util.hpp"

using namespace filepan;
using namespace filepan::ui;
using gfx::Color;

namespace {

void testContrast() {
    EXPECT(test::near(contrastRatio(Color{1, 1, 1, 1}, Color{0, 0, 0, 1}), 21, 0.01f));
    EXPECT(test::near(contrastRatio(Color{0, 0, 0, 1}, Color{0, 0, 0, 1}), 1));
    // WCAG's reference: #767676 on white is just over 4.5:1.
    const float gray = contrastRatio(Color::fromRgb(0x767676), Color{1, 1, 1, 1});
    EXPECT(gray > 4.5f && gray < 4.6f);
    // Symmetric.
    EXPECT(test::near(contrastRatio(Color::fromRgb(0xFF4FC4), Color::fromRgb(0x0A0612)),
                      contrastRatio(Color::fromRgb(0x0A0612), Color::fromRgb(0xFF4FC4))));
    // Translucent ink is composited over the background first.
    const Color ground = Color::fromRgb(0x0A0612);
    EXPECT(contrastRatio(Color{1, 1, 1, 0.5f}, ground) < contrastRatio(Color{1, 1, 1, 1}, ground));
}

// Every preset, Clean in both its dark and light tokens.
std::vector<Theme> allLooks(std::optional<Color> accent = std::nullopt) {
    return {makeTheme(ThemePreset::ultraviolet, accent), makeTheme(ThemePreset::acid, accent),
            makeTheme(ThemePreset::clean, accent, true), makeTheme(ThemePreset::clean, accent, false)};
}

void testPresets() {
    for (const Theme &theme : allLooks()) {
        // The faintest ink allowed: the global floor, or the preset's own where it's higher.
        const float floor = std::max(TextOpacity::kFloor, theme.text.minimum);
        const ColorTokens &c = theme.colors;
        EXPECT(contrastRatio(c.onAccent, c.accent) >= kMinTextContrast);
        EXPECT(contrastRatio(c.onSecondary, c.secondary) >= kMinTextContrast);
        EXPECT(contrastRatio(c.onDestructive, c.destructive) >= kMinTextContrast);
        // Danger is its own hue, and reads as text (error helpers) on the ground.
        EXPECT(hueDistance(c.destructive, c.accent) >= kMinDangerHueGap);
        EXPECT(hueDistance(c.destructive, c.secondary) >= kMinDangerHueGap);
        EXPECT(contrastRatio(c.destructive, c.ground) >= kMinTextContrast);
        // Even the faintest allowed ink reads on the ground.
        EXPECT(contrastRatio(c.ink.withAlpha(theme.text.meta), c.ground) >= kMinTextContrast);
        EXPECT(contrastRatio(c.ink.withAlpha(floor), c.ground) >= kMinTextContrast);
        EXPECT(theme.text.meta >= TextOpacity::kFloor);
        // Mono labels and meta lines stay at or above the floor (glass-signal-mobile.md).
        EXPECT(theme.text.meta >= TextOpacity::kSmallFloor && theme.text.label >= TextOpacity::kSmallFloor);
        EXPECT(contrastRatio(c.ink.withAlpha(theme.text.meta), c.groundDeep) >= kMinTextContrast);
        EXPECT(contrastRatio(c.ink.withAlpha(floor), c.groundDeep) >= kMinTextContrast);
        EXPECT(theme.text.quiet >= TextOpacity::kFloor && theme.text.meta >= floor);
    }
    // Acid is the default.
    EXPECT(makeTheme().preset == ThemePreset::acid && Theme{}.preset == ThemePreset::acid);
    const Theme uv = makeTheme(ThemePreset::ultraviolet);
    EXPECT(uv.preset == ThemePreset::ultraviolet);
    EXPECT(uv.colors.accent == Color::fromRgb(0xFF4FC4));
    EXPECT(uv.colors.onAccent == uv.colors.ground);         // Dark text on pink.
    EXPECT(uv.colors.destructive == Color::fromRgb(0xFF5A36));
    EXPECT(makeTheme(ThemePreset::acid).colors.onAccent == makeTheme(ThemePreset::acid).colors.ground);
    EXPECT(makeTheme(ThemePreset::acid).colors.destructive == Color::fromRgb(0xFF6A1A));
}

void testAccentOverride() {
    // A deep indigo accent needs light ink; a pale one, dark ink.
    const Theme indigo = makeTheme(ThemePreset::ultraviolet, Color::fromRgb(0x283593));
    EXPECT(indigo.colors.onAccent == (Color{1, 1, 1, 1}));
    EXPECT(contrastRatio(indigo.colors.onAccent, indigo.colors.accent) >= kMinTextContrast);
    const Theme lemon = makeTheme(ThemePreset::ultraviolet, Color::fromRgb(0xFFF59D));
    EXPECT(lemon.colors.onAccent == lemon.colors.ground);
    // Whatever accent the user picks, danger moves out of its way.
    for (const auto &[preset, night] : {std::pair{ThemePreset::ultraviolet, true}, std::pair{ThemePreset::acid, true},
                                        std::pair{ThemePreset::clean, true}, std::pair{ThemePreset::clean, false}}) {
        for (int hue = 0; hue < 360; hue += 5) {
            const float h = static_cast<float>(hue) / 60;
            const float x = 1 - std::fabs(std::fmod(h, 2.0f) - 1);
            const float rgb[6][3] = {{1, x, 0}, {x, 1, 0}, {0, 1, x}, {0, x, 1}, {x, 0, 1}, {1, 0, x}};
            const auto &p = rgb[hue / 60];
            const Theme t = makeTheme(preset, Color{p[0], p[1], p[2], 1}, night);
            EXPECT(hueDistance(t.colors.destructive, t.colors.accent) >= kMinDangerHueGap);
            EXPECT(contrastRatio(t.colors.onDestructive, t.colors.destructive) >= kMinTextContrast);
        }
        // A grey accent leaves the preset's own danger color alone.
        EXPECT(makeTheme(preset, Color::fromRgb(0x808080), night).colors.destructive ==
               makeTheme(preset, std::nullopt, night).colors.destructive);
    }
    // Mid-tones where neither reaches 4.5:1 get the better of the two.
    const Color mid = Color::fromRgb(0x7F7F7F);
    const Color ink = pickInk(mid, Color{0, 0, 0, 1}, Color{1, 1, 1, 1}, 10);
    EXPECT(ink == (Color{0, 0, 0, 1}));
}

void testClean() {
    // Plain: no decoration, no caps or condensed widths.
    for (bool night : {true, false}) {
        const Theme clean = makeTheme(ThemePreset::clean, std::nullopt, night);
        EXPECT(clean.colors.light == !night);
        EXPECT(!clean.style.backdropBlur && !clean.style.marks && !clean.style.hardShadow);
        // The dense layout, with rows still tall enough to tap.
        EXPECT(clean.style.utilitarian && clean.spacing.statusLine > 0);
        EXPECT(clean.spacing.listRow >= 40 && clean.spacing.listRowCompact >= 40);
        EXPECT(clean.colors.gridAlpha == 0);
        for (const gfx::TextStyle &style : {clean.type.display, clean.type.title, clean.type.section,
                                            clean.type.label, clean.type.button}) {
            EXPECT(!style.uppercase && style.stretch == 100);
        }
    }
    // Light: dark ink and films, and ink on the accent that isn't the light ground.
    const Theme light = makeTheme(ThemePreset::clean, std::nullopt, false);
    EXPECT(relativeLuminance(light.colors.ground) > relativeLuminance(light.colors.ink));
    EXPECT(light.colors.film == (Color{0, 0, 0, 1}));
    EXPECT(light.colors.onAccent == (Color{1, 1, 1, 1}));
    // The other presets ignore night, and keep their decoration.
    EXPECT(makeTheme(ThemePreset::acid, std::nullopt, false) == makeTheme(ThemePreset::acid));
    EXPECT(makeTheme(ThemePreset::acid).style == ThemeStyle{});
}

void testScales() {
    const Theme theme = makeTheme();
    EXPECT(inkAlpha(0.3f) == TextOpacity::kFloor && inkAlpha(0.8f) == 0.8f);
    EXPECT(inkAlpha(0.4f, 11) == TextOpacity::kSmallFloor && inkAlpha(0.62f, 15) == 0.62f);
    EXPECT(theme.type.body.size == 15 && theme.type.body.family == gfx::FontFamily::sans);
    EXPECT(theme.type.label.family == gfx::FontFamily::mono && theme.type.label.uppercase);
    EXPECT(theme.type.meta.family == gfx::FontFamily::sans && !theme.type.meta.uppercase && theme.type.meta.size == 12);
    EXPECT(theme.type.section.uppercase && theme.type.section.letterSpacing < theme.type.label.letterSpacing);
    EXPECT(theme.type.display.stretch == 62 && theme.type.display.weight == 800);
    EXPECT(theme.spacing.listRow == 60 && theme.spacing.iconTile == 40);
    EXPECT(theme.type.title.uppercase && theme.type.title.weight == 750);
    EXPECT(theme.colors.selected == 0.13f);
    EXPECT(theme.spacing.touchTarget == 48);
}

void testCorners() {
    for (auto preset : {ThemePreset::ultraviolet, ThemePreset::acid}) {
        const Radii &r = makeTheme(preset).radii;
        for (float radius : {r.sheet, r.panel, r.input, r.listItem, r.tile, r.iconTile, r.band, r.pill}) {
            EXPECT(radius == kCorner);
        }
        EXPECT(r.tag == 4);
    }
    // Clean is squarer: nothing past its own cap.
    const Theme clean = makeTheme(ThemePreset::clean);
    EXPECT(clean.cornerCap < kCorner && clean.corner(20) == clean.cornerCap);
    for (float radius : {clean.radii.sheet, clean.radii.panel, clean.radii.input, clean.radii.listItem, clean.radii.tile,
                         clean.radii.iconTile, clean.radii.band, clean.radii.tag, clean.radii.pill}) {
        EXPECT(radius <= clean.cornerCap);
    }
    // corner() caps at kCorner, keeping radii already under it.
    const Theme theme = makeTheme();
    EXPECT(theme.corner(20) == kCorner && theme.corner(1e6f) == kCorner && theme.corner(2) == 2);
    EXPECT(theme.cornerPx(52.5f, 2.625f) == kCorner * 2.625f && theme.cornerPx(3, 2.625f) == 3);
    EXPECT(theme.corners({20, 20, 0, 0}) == (gfx::CornerRadii{kCorner, kCorner, 0, 0}));
}

}  // namespace

int main() {
    testContrast();
    testPresets();
    testAccentOverride();
    testClean();
    testScales();
    testCorners();
    return 0;
}
