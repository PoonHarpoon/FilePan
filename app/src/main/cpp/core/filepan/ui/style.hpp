// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <limits>
#include <optional>

// Layout style: a flexbox subset, in dp. See layout.hpp for how it's applied.
namespace filepan::ui {

inline constexpr float kInfinity = std::numeric_limits<float>::infinity();

// A size along one axis: automatic, dp, or a percentage of the parent's content box.
struct Length {
    enum class Unit : std::uint8_t { automatic, dp, percent };

    Unit unit = Unit::automatic;
    float value = 0;

    constexpr Length() = default;
    constexpr Length(float dp) : unit(Unit::dp), value(dp) {}  // NOLINT(google-explicit-constructor)
    static constexpr Length percent(float percent) {
        Length length;
        length.unit = Unit::percent;
        length.value = percent;
        return length;
    }
    static constexpr Length automatic() { return {}; }

    [[nodiscard]] constexpr bool isAuto() const { return unit == Unit::automatic; }
    // Resolves against base; percentages of an unknown (infinite) base are automatic.
    [[nodiscard]] constexpr std::optional<float> resolve(float base) const {
        switch (unit) {
        case Unit::dp:
            return value;
        case Unit::percent:
            if (base < kInfinity) {
                return base * value / 100;
            }
            return std::nullopt;
        case Unit::automatic:
            break;
        }
        return std::nullopt;
    }

    constexpr bool operator==(const Length &) const = default;
};

struct Edges {
    float left = 0;
    float top = 0;
    float right = 0;
    float bottom = 0;

    static constexpr Edges all(float value) { return {value, value, value, value}; }
    static constexpr Edges symmetric(float horizontal, float vertical) {
        return {horizontal, vertical, horizontal, vertical};
    }
    [[nodiscard]] constexpr float horizontal() const { return left + right; }
    [[nodiscard]] constexpr float vertical() const { return top + bottom; }

    constexpr bool operator==(const Edges &) const = default;
};

enum class Direction : std::uint8_t { row, column };

enum class Justify : std::uint8_t { start, end, center, spaceBetween, spaceAround, spaceEvenly };

// automatic, on alignSelf, means "use the parent's alignItems".
enum class Align : std::uint8_t { automatic, start, end, center, stretch };

enum class Position : std::uint8_t {
    relative,  // In the flex flow.
    absolute,  // Out of the flow, placed by insets in the parent's box: stacks and overlays.
};

struct Style {
    bool display = true;  // false takes no space and isn't painted or hit.

    Direction direction = Direction::column;
    Justify justify = Justify::start;
    Align alignItems = Align::stretch;
    Align alignSelf = Align::automatic;
    bool wrap = false;
    float gap = 0;       // Between items along the main axis.
    float lineGap = 0;   // Between wrapped lines.

    Edges padding;
    Edges margin;

    // Unlike CSS, shrink defaults to 0 (as in Yoga): only items that ask for it shrink, typically
    // the text in a row, which then ellipsizes. Scroll containers always shrink.
    float grow = 0;
    float shrink = 0;
    Length basis;

    Length width;
    Length height;
    float minWidth = 0;
    float minHeight = 0;
    float maxWidth = kInfinity;
    float maxHeight = kInfinity;
    float aspectRatio = 0;  // width / height; 0 means none.

    Position position = Position::relative;
    // Insets for absolute children, from the parent's edges.
    std::optional<float> left;
    std::optional<float> top;
    std::optional<float> right;
    std::optional<float> bottom;

    constexpr bool operator==(const Style &) const = default;
};

}  // namespace filepan::ui
