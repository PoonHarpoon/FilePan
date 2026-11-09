// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>
#include <variant>

#include "filepan/gfx/geometry.hpp"

namespace filepan::gfx {

struct GradientStop {
    float offset = 0;  // In [0, 1].
    Color color;

    constexpr bool operator==(const GradientStop &) const = default;
};

// Up to kMaxStops stops: the design never uses more than three (pink -> purple -> blue).
struct GradientStops {
    static constexpr std::size_t kMaxStops = 4;

    std::array<GradientStop, kMaxStops> stops{};
    std::uint8_t count = 0;

    constexpr GradientStops() = default;
    constexpr GradientStops(std::initializer_list<GradientStop> list) {
        for (const auto &stop : list) {
            if (count < kMaxStops) {
                stops[count++] = stop;
            }
        }
    }

    constexpr bool operator==(const GradientStops &) const = default;
};

// Positions are in the same coordinate space as the shape being filled.
struct LinearGradient {
    Vec2 from;
    Vec2 to;
    GradientStops stops;

    constexpr bool operator==(const LinearGradient &) const = default;
};

struct RadialGradient {
    Vec2 center;
    float radius = 0;
    GradientStops stops;

    constexpr bool operator==(const RadialGradient &) const = default;
};

// Sweeps clockwise from startAngle (radians, 0 = pointing right, like CSS "from 90deg").
struct ConicGradient {
    Vec2 center;
    float startAngle = 0;
    GradientStops stops;

    constexpr bool operator==(const ConicGradient &) const = default;
};

// Parallel stripes, like repeating-linear-gradient(angle, color 0 width, transparent width period).
// Used for hazard stripes and barcodes.
struct Stripes {
    float angle = 0;    // Radians; the stripes run perpendicular to this direction.
    float width = 0;    // Painted width of each stripe.
    float period = 0;   // Distance from one stripe to the next.
    float phase = 0;    // Offset along the direction.
    Color color;

    constexpr bool operator==(const Stripes &) const = default;
};

using Paint = std::variant<Color, LinearGradient, RadialGradient, ConicGradient, Stripes>;

// Box shadow, with CSS box-shadow semantics: blur is the CSS blur radius, spread grows (or for
// negative values shrinks) the shape, and inset shadows are drawn inside the shape.
struct Shadow {
    Vec2 offset;
    float blur = 0;
    float spread = 0;
    Color color;
    bool inset = false;

    constexpr bool operator==(const Shadow &) const = default;
};

}  // namespace filepan::gfx
