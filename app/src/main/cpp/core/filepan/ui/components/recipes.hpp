// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <string_view>

#include "filepan/gfx/display_list.hpp"
#include "filepan/ui/theme.hpp"

// The Glass Signal paint recipes (design doc §5-§7), recorded straight into a display list.
// Components call these from their paint hooks; screens can too, for one-off decoration.
//
// Shapes and rects are in px. Lengths the recipes add themselves (shadow offsets, ring widths,
// tick lengths) are in dp and scaled with density.
namespace filepan::ui::components::recipes {

// A panel, the surface of bars, the dock, the data band, menus, sheets, dialogs, cards and glass
// buttons: the deep ground, opaque, with the glass tint at 5% over it, a 1 dp border of the tint
// at 13% and a hard 2 dp shadow straight down. No blur, sheen or gradient. Corners over kCorner
// come out at kCorner, so a pill passed as is still comes out right.
// The utilitarian style draws it flat: the deep ground and a hairline, no tint or shadow.
void glass(gfx::DisplayList &list, const gfx::RoundedRect &shape, const Theme &theme, float density);

// The hard 2 dp shadow straight down that panels and raised fills sit on.
void hardShadow(gfx::DisplayList &list, const gfx::RoundedRect &shape, float density);

// A well (§7.2, §7.6): a flat dark fill with a 1 dp hairline. Inputs, tracks, the segmented
// control.
void sunken(gfx::DisplayList &list, const gfx::RoundedRect &shape, const Theme &theme, float density);

// A raised fill in base: primary and danger buttons, the filled icon button, the segmented
// control's thumb, swatches. Solid, with a dark 1 dp hairline and the panels' hard shadow; no
// gradient or glow.
void raised(gfx::DisplayList &list, const gfx::RoundedRect &shape, gfx::Color base, float density);

// The knob of switches and sliders: a solid light square on the hard shadow.
void knob(gfx::DisplayList &list, const gfx::RoundedRect &shape, float density);

// The keyboard focus ring: a 2 dp accent edge 2 dp outside the shape (§7.6), with no halo.
void focusRing(gfx::DisplayList &list, const gfx::RoundedRect &shape, const Theme &theme, float density);

// Pressed: a brightening overlay (§7.1 `brightness(1.08)`).
void pressedOverlay(gfx::DisplayList &list, const gfx::RoundedRect &shape, float amount = 0.10f);

// --- Marks (§6) ---

// L-shaped ticks at each corner of rect, inset by inset dp, each arm length dp long.
void cornerTicks(gfx::DisplayList &list, const gfx::Rect &rect, gfx::Color color, float density,
                 float inset = 12, float length = 12);
// Diagonal hazard stripes filling shape.
void hazardStripes(gfx::DisplayList &list, const gfx::RoundedRect &shape, gfx::Color color, float density);
// Bars of 1-3 dp, derived from seed so the same ID always draws the same code.
void barcode(gfx::DisplayList &list, const gfx::Rect &rect, gfx::Color color, std::string_view seed,
             float density);
// A crosshair: a ring with ticks through it, for the orb's glass tile and the logo mark.
void crosshair(gfx::DisplayList &list, gfx::Vec2 center, float radius, gfx::Color color, float density);

// --- Gradients ---

// Top to bottom over rect.
gfx::LinearGradient verticalGradient(const gfx::Rect &rect, gfx::GradientStops stops);
// Left to right over rect.
gfx::LinearGradient horizontalGradient(const gfx::Rect &rect, gfx::GradientStops stops);

}  // namespace filepan::ui::components::recipes
