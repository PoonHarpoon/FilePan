// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include "filepan/gfx/geometry.hpp"
#include "filepan/ui/reactive.hpp"
#include "filepan/ui/theme.hpp"

// Platform state as signals. The Android shell writes these as configuration changes arrive;
// components read them in binds (environment().theme().colors.accent) and update when they
// change. UiRoot owns the instance and re-lays out and repaints when density or font scale
// change.
namespace filepan::ui {

// Window insets in physical px.
struct Insets {
    float left = 0;
    float top = 0;
    float right = 0;
    float bottom = 0;

    constexpr bool operator==(const Insets &) const = default;
};

struct Environment {
    Signal<float> density{1.0f};        // px per dp.
    Signal<float> fontScale{1.0f};      // sp per dp.
    Signal<gfx::Size> windowSize;       // px.
    Signal<Insets> safeArea;            // System bars and cutouts, px.
    Signal<Insets> imeInsets;           // The keyboard, px.
    Signal<bool> reducedMotion{false};  // Animations snap or crossfade.
    Signal<bool> powerSave{false};      // Battery saver: skip costly effects.
    Signal<bool> night{true};           // The system is in dark mode.
    Signal<Theme> theme{makeTheme()};
};

// The UI thread's environment: the current UiRoot's, or a default one outside any.
Environment &environment();

}  // namespace filepan::ui
