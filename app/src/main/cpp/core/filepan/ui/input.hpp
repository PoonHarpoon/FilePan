// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>

#include "filepan/gfx/geometry.hpp"

// Input events as the platform shell delivers them. Positions are physical pixels in window
// coordinates; times are a monotonic clock in nanoseconds (the same clock as frame times).
namespace filepan::ui {

enum class PointerAction : std::uint8_t { down, move, up, cancel };

enum class PointerKind : std::uint8_t { touch, mouse, stylus };

struct PointerEvent {
    std::int32_t pointer = 0;  // Stable from down to up/cancel.
    PointerAction action = PointerAction::down;
    PointerKind kind = PointerKind::touch;
    gfx::Vec2 position;        // Window px.
    std::int64_t timeNs = 0;
    // Filled in by the dispatcher for each node it delivers to: position in that node's local px
    // (after its transforms).
    gfx::Vec2 local;
};

enum class KeyAction : std::uint8_t { down, up };

namespace modifiers {
inline constexpr std::uint32_t kShift = 1 << 0;
inline constexpr std::uint32_t kCtrl = 1 << 1;
inline constexpr std::uint32_t kAlt = 1 << 2;
inline constexpr std::uint32_t kMeta = 1 << 3;
}  // namespace modifiers

// Key codes use Android's KeyEvent.KEYCODE_* values so the shell passes them through.
namespace keys {
inline constexpr std::int32_t kBack = 4;
inline constexpr std::int32_t kDpadUp = 19;
inline constexpr std::int32_t kDpadDown = 20;
inline constexpr std::int32_t kDpadLeft = 21;
inline constexpr std::int32_t kDpadRight = 22;
inline constexpr std::int32_t kDpadCenter = 23;
inline constexpr std::int32_t kA = 29;
inline constexpr std::int32_t kC = 31;
inline constexpr std::int32_t kS = 47;
inline constexpr std::int32_t kV = 50;
inline constexpr std::int32_t kX = 52;
inline constexpr std::int32_t kTab = 61;
inline constexpr std::int32_t kSpace = 62;
inline constexpr std::int32_t kEnter = 66;
inline constexpr std::int32_t kDel = 67;
inline constexpr std::int32_t kPageUp = 92;
inline constexpr std::int32_t kPageDown = 93;
inline constexpr std::int32_t kEscape = 111;
inline constexpr std::int32_t kForwardDel = 112;
inline constexpr std::int32_t kMoveHome = 122;
inline constexpr std::int32_t kMoveEnd = 123;
}  // namespace keys

struct KeyEvent {
    std::int32_t keyCode = 0;
    KeyAction action = KeyAction::down;
    std::uint32_t modifiers = 0;
    std::int32_t repeat = 0;
    std::uint32_t codePoint = 0;  // The character it types, if any.
    std::int64_t timeNs = 0;
};

}  // namespace filepan::ui
