// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <optional>

#include "filepan/gfx/geometry.hpp"
#include "filepan/gfx/text.hpp"

// Glass Signal theme tokens, adapted for a dense phone file manager.
//
// Colors come from two of the design doc's directions (§3), both dark: Acid (the
// default) and Ultraviolet. Clean is the plain one: no glass or marks, light or dark with the
// system. Each has the same structure, ground -> ink -> accent ->
// secondary -> tertiary, plus a destructive color and the panels' tint. The accent can
// be overridden at runtime; the ink drawn on it is then picked for WCAG contrast.
//
// Type sizes are sp and lengths dp; the framework converts with the font scale and density.
// Sized for phones: list rows are 60 dp, radii inside lists are small, touch targets stay at 48 dp,
// and the list's marks are quiet.
namespace filepan::ui {

// Settings lists them in this order, by index.
enum class ThemePreset : std::uint8_t { ultraviolet, acid, clean };

// Every corner radius, in dp: panels, sheets, buttons, inputs, tiles and the design doc's pills.
// Tags keep their sharper 4 dp; status dots, swatches and the meter ring stay round.
inline constexpr float kCorner = 6;

struct ColorTokens {
    gfx::Color ground;      // Window background.
    gfx::Color groundDeep;  // A second ground for bars and sheets.
    gfx::Color ink;         // Text and icons on the ground; scale with TextOpacity.
    gfx::Color accent;      // Live / primary / active.
    gfx::Color onAccent;    // Ink on accent fills; dark or light for >= 4.5:1.
    gfx::Color secondary;   // Warning / attention.
    gfx::Color onSecondary;
    gfx::Color tertiary;    // Info.
    gfx::Color highlight;   // Status dots, a second accent.
    gfx::Color destructive;
    gfx::Color onDestructive;
    // The direction's signature three-stop gradient (Ultraviolet's pink -> purple -> blue):
    // meter, switch track.
    gfx::Color spectrum[3];

    // Panels (recipes::glass) tint the deep ground with this and draw their border in it.
    gfx::Color glassTint{1, 1, 1, 1};

    // A light ground with dark ink, where the dark presets have the reverse.
    bool light = false;

    // Films over the ground (presses, hairlines on pictures and wells): white on dark grounds,
    // black on light ones. Shadows stay black on both.
    gfx::Color film{1, 1, 1, 1};
    // The black wash that sinks tracks and fields into the ground.
    float wellAlpha = 0.34f;

    // The storage home's 48 dp background grid, as an ink alpha; 0 for none.
    float gridAlpha = 0.04f;

    // Flat surfaces and states, as ink alphas over the ground.
    float surface = 0.05f;
    float surfaceRaised = 0.09f;
    float divider = 0.10f;
    float pressed = 0.12f;   // Ink overlay while pressed.
    float selected = 0.13f;  // Accent overlay on selected rows.
    float scrim = 0.60f;     // Black behind dialogs and sheets.

    bool operator==(const ColorTokens &) const = default;
};

// Ink alphas for text on dark (§3.4 and the mobile doc). Nothing goes below kFloor: 52% ink on
// either preset's ground or deep ground still clears 4.5:1. Quiet is the list's section labels;
// meta its second lines.
struct TextOpacity {
    static constexpr float kFloor = 0.52f;
    // Small text once had a higher floor; it now shares the one floor.
    static constexpr float kSmallFloor = kFloor;
    static constexpr float kSmallTextSp = 13;
    float primary = 1.0f;
    float body = 0.82f;
    float label = 0.74f;
    float meta = 0.62f;
    float quiet = 0.52f;
    // This preset's own floor, over kFloor: a light ground needs more ink for the same contrast.
    float minimum = kFloor;

    bool operator==(const TextOpacity &) const = default;
};

// Clamps a requested ink alpha to the floor.
constexpr float inkAlpha(float requested) { return requested < TextOpacity::kFloor ? TextOpacity::kFloor : requested; }
// Clamps a requested ink alpha for text of sizeSp to its floor.
constexpr float inkAlpha(float requested, float sizeSp) {
    const float floor = sizeSp < TextOpacity::kSmallTextSp ? TextOpacity::kSmallFloor : TextOpacity::kFloor;
    return requested < floor ? floor : requested;
}

// Type roles; sizes in sp.
struct TypeScale {
    gfx::TextStyle display;    // Condensed caps: hero numbers, empty states.
    gfx::TextStyle title;      // Screen and dialog titles.
    gfx::TextStyle body;       // File names, list primary lines.
    gfx::TextStyle bodySmall;  // Grid tile names.
    gfx::TextStyle meta;       // A row's second line: size and date, in sentence case.
    gfx::TextStyle section;    // Mono caps list section labels, quieter than label.
    gfx::TextStyle label;      // Mono caps: tags, metrics, form labels.
    gfx::TextStyle button;     // Condensed caps on buttons.

    bool operator==(const TypeScale &) const = default;
};

struct Radii {
    float sheet = kCorner;     // Bottom sheets and dialogs.
    float panel = kCorner;     // Cards and panels (volume cards).
    float input = kCorner;
    float listItem = kCorner;  // Selection and press highlights inside lists.
    float tile = kCorner;      // Grid thumbnails.
    float iconTile = kCorner;  // The file kind tile in a row.
    float band = kCorner;      // The data band.
    float tag = 4;             // Deliberately sharp.
    float pill = kCorner;      // What the design doc draws as pills: buttons, the dock, chips.

    bool operator==(const Radii &) const = default;
};

struct Spacing {
    float xs = 4;
    float s = 8;
    float m = 12;
    float l = 16;
    float xl = 20;
    float xxl = 24;
    float page = 16;            // Screen edge padding.
    float listRow = 60;         // File row: name and meta line.
    float listRowCompact = 48;  // One-line rows: menus, settings.
    float listRowLarge = 64;    // Rows with a large thumbnail.
    float icon = 24;
    float iconTile = 40;        // The file kind tile in a row.
    float iconGap = 12;         // Between a row's icon and its text.
    float rowInset = 24;        // A row's content from the screen edge (its highlight sits at 8).
    float touchTarget = 48;
    float gridCellMin = 104;    // Grid view cell width floor.
    float appBar = 56;
    float dock = 64;            // The floating bottom dock's height.
    float dockInset = 16;       // From the screen sides and the navigation bar.
    float grid = 48;            // Background grid pitch (storage home only).
    float statusLine = 0;       // The utilitarian status line over the toolbar; 0 for none.

    bool operator==(const Spacing &) const = default;
};

// Durations in ms; the spring is the standard one for everything that moves.
struct Motion {
    float springStiffness = 500;
    float springDamping = 0.85f;
    float knobMs = 240;       // Switch knob, on the springy overshoot curve.
    float crossfadeMs = 120;  // What replaces movement under reduced motion.

    bool operator==(const Motion &) const = default;
};

// The Glass Signal decoration a preset draws; a plain preset turns it off.
struct ThemeStyle {
    bool backdropBlur = true;  // Blur behind the dock and other floating bars.
    bool marks = true;         // Corner ticks, barcodes and the empty state's orb.
    bool hardShadow = true;    // The 2 dp offset shadow under panels, buttons and knobs.
    // The dense tool layout instead of floating glass: bars fixed to the screen edges, one-line
    // rows with size and date columns, flat bordered panels, plain titles and a status line.
    bool utilitarian = false;

    bool operator==(const ThemeStyle &) const = default;
};

struct Theme {
    ThemePreset preset = ThemePreset::acid;
    ColorTokens colors;
    TextOpacity text;
    TypeScale type;
    Radii radii;
    Spacing spacing;
    Motion motion;
    ThemeStyle style;
    // Every radius is capped at this: kCorner, or less for a squarer preset.
    float cornerCap = kCorner;

    // A corner radius capped at cornerCap, for the radii that don't come from Radii (a menu's, a
    // pill's), so they come out as tight as the rest.
    [[nodiscard]] float corner(float dp) const { return dp > cornerCap ? cornerCap : dp; }
    [[nodiscard]] gfx::CornerRadii corners(gfx::CornerRadii dp) const {
        return {corner(dp.topLeft), corner(dp.topRight), corner(dp.bottomRight), corner(dp.bottomLeft)};
    }
    // The same in px at density.
    [[nodiscard]] float cornerPx(float px, float density) const {
        return px > cornerCap * density ? cornerCap * density : px;
    }

    bool operator==(const Theme &) const = default;
};

// The preset's tokens; with accentOverride, the accent (and its on-accent ink) is replaced. night
// picks Clean's dark tokens over its light ones; the other presets are always dark.
Theme makeTheme(ThemePreset preset = ThemePreset::acid,
                std::optional<gfx::Color> accentOverride = std::nullopt, bool night = true);

// --- WCAG 2.x contrast ---

// Relative luminance of an opaque sRGB color.
float relativeLuminance(gfx::Color color);
// color drawn over an opaque background, flattened to an opaque color.
gfx::Color composite(gfx::Color color, gfx::Color background);
// (L1 + 0.05) / (L2 + 0.05), in [1, 21]. A translucent foreground is composited first.
float contrastRatio(gfx::Color foreground, gfx::Color background);
inline constexpr float kMinTextContrast = 4.5f;
inline constexpr float kMinLargeTextContrast = 3.0f;
// Whichever of dark and light reads on background: dark if it reaches minimum (the design puts
// dark text on bright accents), else light if it does, else the higher-contrast one.
gfx::Color pickInk(gfx::Color background, gfx::Color dark, gfx::Color light,
                   float minimum = kMinTextContrast);

// --- Hue ---

// Distance between two colors' hues on the color wheel, in degrees [0, 180]. 180 if either is too
// grey to have a meaningful hue: a grey can't be mistaken for a hue.
float hueDistance(gfx::Color a, gfx::Color b);
// Danger must never be mistaken for the accent or the secondary color: at least this far apart.
inline constexpr float kMinDangerHueGap = 40;

}  // namespace filepan::ui
