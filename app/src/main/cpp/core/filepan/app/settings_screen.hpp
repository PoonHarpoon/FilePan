// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include "filepan/app/app.hpp"

// The settings screen (glass-signal-mobile.md, "Other screens"): grouped glass panels.
//
//   01 Look      The theme preset (segmented), the accent (a swatch row, and a picker sheet with
//                hue, saturation and lightness sliders, a hex field, a live contrast check and a
//                reset), and reduce glass.
//   02 Browsing  The default view and sort, folders first, hidden files (and the hidden items
//                screen, hidden_items_screen.hpp) and thumbnails, with the
//                thumbnail cache's size, decay and a clear action.
//   03 Remote    Mobile data mode, thumbnails for remote files and the largest file fetched for
//                one, and the viewer's prefetching.
//   04 FilePan   Backing up and restoring the settings (backup_sheets.hpp), the intro again,
//                about and the licenses.
//
// Changes apply at once and are saved.
namespace filepan::app {

inline constexpr const char *kSettingsScreen = "settings";

ScreenSpec settingsScreenSpec();

// The theme preset (Ultraviolet or Acid), and the accent swatches with the custom one that opens
// the picker: the settings screen's Look panel, and the intro's.
ui::NodePtr themePresetControl(AppState &state);
ui::NodePtr accentSwatches(AppContext &context);
// The "Mobile data mode" switch and what it does right now (on the onboarding's last page too).
ui::NodePtr dataSaverLine(AppContext &context);

// The accent picker sheet.
void showAccentPicker(AppContext &context);

struct Hsl {
    float h = 0;  // Degrees, 0-360.
    float s = 0;  // 0-1.
    float l = 0;  // 0-1.
};
Hsl toHsl(gfx::Color color);
gfx::Color fromHsl(Hsl hsl);

// How an accent reads: as an accent on the ground (UI marks want 3:1) and under its own ink
// (text on accent fills wants 4.5:1).
struct AccentContrast {
    float onGround = 1;
    float inkOnAccent = 1;
    [[nodiscard]] bool groundPasses() const { return onGround >= ui::kMinLargeTextContrast; }
    [[nodiscard]] bool inkPasses() const { return inkOnAccent >= ui::kMinTextContrast; }
};
AccentContrast accentContrast(ui::ThemePreset preset, gfx::Color accent);

}  // namespace filepan::app
