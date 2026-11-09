// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "filepan/gfx/geometry.hpp"
#include "filepan/gfx/text.hpp"
#include "filepan/ui/environment.hpp"
#include "filepan/ui/gesture.hpp"
#include "filepan/ui/node.hpp"
#include "filepan/ui/reactive.hpp"
#include "filepan/ui/theme.hpp"

// Shared plumbing for the Glass Signal component kit: theme access, color math, haptics, and the
// pressable behaviour every tappable component uses.
//
// Conventions across the kit:
// - Components are Node subclasses created with a static create(Props). Props hold Prop<T> for
//   anything that may be bound to signals, so callers pass a literal or a lambda.
// - Paint hooks read the theme with currentTheme() at paint time; UiRoot repaints the tree when
//   the theme changes. Text colors are bound with theme() so they follow it too.
// - Lengths are dp and text sizes sp; paint code converts with PaintContext::px.
// - Every interactive component has semantics and a hit target of at least 48 dp.
namespace filepan::ui::components {

// Tracked: re-runs the calling bind when the theme changes.
const Theme &theme();
// Untracked, for paint hooks.
const Theme &currentTheme();

bool reducedMotion();

// --- Color ---

gfx::Color mix(gfx::Color a, gfx::Color b, float t);
inline gfx::Color lighten(gfx::Color c, float t) { return mix(c, {1, 1, 1, c.a}, t); }
inline gfx::Color darken(gfx::Color c, float t) { return mix(c, {0, 0, 0, c.a}, t); }
// CSS saturate(0) brightness(b).
gfx::Color grayscale(gfx::Color c, float brightness = 1);
// The theme's ink at alpha (clamped to the floor for its text size if sizeSp is given).
gfx::Color ink(const Theme &theme, float alpha);
gfx::Color textInk(const Theme &theme, float alpha, float sizeSp);
// color, lightened toward white (darkened toward black on a light background) just enough to reach
// minimum contrast on background: accent and destructive text on the ground.
gfx::Color legible(gfx::Color color, gfx::Color background, float minimum = kMinTextContrast);

// --- Text styles ---

gfx::TextStyle withSize(gfx::TextStyle style, float sizeSp);
gfx::TextStyle withWeight(gfx::TextStyle style, float weight);
// The mono label at 10 sp, bold: stamps and tags.
gfx::TextStyle stampStyle(const Theme &theme);

// --- Haptics ---

enum class Haptic : std::uint8_t { tap, toggle, longPress, tick, reject };
// The app installs a handler that maps these to the platform's haptics. Per UI thread.
void setHapticHandler(std::function<void(Haptic)> handler);
void haptic(Haptic kind);

// --- Pressable ---

// What a pressable node exposes: the pressed state (for paint) and the recognizer.
struct Pressable {
    Signal<bool> pressed{false};
    std::shared_ptr<TapRecognizer> tap;
    std::shared_ptr<LongPressRecognizer> longPress;
};

struct PressableOptions {
    std::function<void()> onTap;
    std::function<void()> onLongPress;
    Prop<bool> enabled = true;
    Haptic haptic = Haptic::tap;
    // Focusable and activated by Enter, Space and D-pad center.
    bool focusable = true;
};

// Makes node tappable: a tap recognizer (and a long-press one if asked), the pressed signal,
// keyboard activation, focusability and the disabled flag in its semantics. Callers set the
// role and label.
std::shared_ptr<Pressable> makePressable(Node &node, PressableOptions options);

// Follows a horizontal drag's finger position exactly. DragRecognizer holds the touch slop back
// from its first update so scrolls don't jump; a slider thumb or a dragged caret should stay
// under the finger, so this adds it back.
class FingerTracker {
public:
    void start(float xPx) {
        x_ = xPx;
        first_ = true;
    }
    float update(float deltaPx);
    [[nodiscard]] float x() const { return x_; }

private:
    float x_ = 0;
    bool first_ = false;
};

// Writes a semantics flag without touching the rest.
void setSemanticsFlag(Node &node, std::uint16_t flag, bool on);

// Moves focus to the next (or previous) focusable node inside root, wrapping: dialogs and sheets
// trap focus with it. Returns false if root has none.
bool moveFocusWithin(Node &root, bool forward);
// The first focusable node in root, or null.
Node *firstFocusable(Node &root);

// The node's top left in window px, through its ancestors' transforms and scroll offsets.
gfx::Transform nodeToWindow(const Node &node, float density);

}  // namespace filepan::ui::components
