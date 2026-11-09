// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Geometry and color shared by the renderer, the text system and the UI framework. All lengths
// are in physical pixels unless a name says otherwise; the UI framework converts from dp.
namespace filepan::gfx {

struct Vec2 {
    float x = 0;
    float y = 0;

    constexpr Vec2 operator+(Vec2 other) const { return {x + other.x, y + other.y}; }
    constexpr Vec2 operator-(Vec2 other) const { return {x - other.x, y - other.y}; }
    constexpr Vec2 operator*(float scale) const { return {x * scale, y * scale}; }
    constexpr bool operator==(const Vec2 &) const = default;
};

struct Size {
    float width = 0;
    float height = 0;

    constexpr bool operator==(const Size &) const = default;
};

struct Rect {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;

    static constexpr Rect fromEdges(float left, float top, float right, float bottom) {
        return {left, top, right - left, bottom - top};
    }

    [[nodiscard]] constexpr float right() const { return x + width; }
    [[nodiscard]] constexpr float bottom() const { return y + height; }
    [[nodiscard]] constexpr Vec2 origin() const { return {x, y}; }
    [[nodiscard]] constexpr Size size() const { return {width, height}; }
    [[nodiscard]] constexpr Vec2 center() const { return {x + width / 2, y + height / 2}; }
    [[nodiscard]] constexpr bool empty() const { return width <= 0 || height <= 0; }

    [[nodiscard]] constexpr bool contains(Vec2 point) const {
        return point.x >= x && point.x < right() && point.y >= y && point.y < bottom();
    }

    [[nodiscard]] constexpr Rect translated(Vec2 offset) const {
        return {x + offset.x, y + offset.y, width, height};
    }

    // Shrinks each edge by amount; a negative amount grows it.
    [[nodiscard]] constexpr Rect inset(float amount) const {
        return {x + amount, y + amount, width - 2 * amount, height - 2 * amount};
    }

    // The overlap of two rects; empty if they don't overlap.
    [[nodiscard]] constexpr Rect intersect(const Rect &other) const {
        const float left = std::max(x, other.x);
        const float top = std::max(y, other.y);
        const float r = std::min(right(), other.right());
        const float b = std::min(bottom(), other.bottom());
        return r > left && b > top ? fromEdges(left, top, r, b) : Rect{};
    }

    // The smallest rect containing both; an empty rect contributes nothing.
    [[nodiscard]] constexpr Rect unite(const Rect &other) const {
        if (empty()) {
            return other;
        }
        if (other.empty()) {
            return *this;
        }
        return fromEdges(std::min(x, other.x), std::min(y, other.y), std::max(right(), other.right()),
                         std::max(bottom(), other.bottom()));
    }

    constexpr bool operator==(const Rect &) const = default;
};

struct CornerRadii {
    float topLeft = 0;
    float topRight = 0;
    float bottomRight = 0;
    float bottomLeft = 0;

    static constexpr CornerRadii uniform(float radius) { return {radius, radius, radius, radius}; }

    constexpr bool operator==(const CornerRadii &) const = default;
};

// A rect with rounded corners. Radii larger than half the shorter side are clamped when drawn,
// so a radius of 1e6 gives a pill.
struct RoundedRect {
    Rect rect;
    CornerRadii radii;

    constexpr bool operator==(const RoundedRect &) const = default;
};

// A 2D affine transform mapping (x, y) to (a*x + c*y + tx, b*x + d*y + ty).
struct Transform {
    float a = 1;
    float b = 0;
    float c = 0;
    float d = 1;
    float tx = 0;
    float ty = 0;

    static constexpr Transform translation(Vec2 offset) { return {1, 0, 0, 1, offset.x, offset.y}; }
    static constexpr Transform scale(float sx, float sy) { return {sx, 0, 0, sy, 0, 0}; }

    [[nodiscard]] constexpr Vec2 apply(Vec2 p) const {
        return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty};
    }

    // this * other: applies other first, then this.
    [[nodiscard]] constexpr Transform operator*(const Transform &o) const {
        return {a * o.a + c * o.b, b * o.a + d * o.b, a * o.c + c * o.d,
                b * o.c + d * o.d, a * o.tx + c * o.ty + tx, b * o.tx + d * o.ty + ty};
    }

    [[nodiscard]] constexpr bool isTranslation() const { return a == 1 && b == 0 && c == 0 && d == 1; }

    constexpr bool operator==(const Transform &) const = default;
};

// A non-premultiplied sRGB color with components in [0, 1]. The renderer blends in sRGB space,
// like browsers do, so gradients and translucency match the CSS the design was drawn in.
struct Color {
    float r = 0;
    float g = 0;
    float b = 0;
    float a = 1;

    // From 0xRRGGBB, as written in the design tokens.
    static constexpr Color fromRgb(std::uint32_t rgb, float alpha = 1) {
        return {static_cast<float>((rgb >> 16) & 0xff) / 255,
                static_cast<float>((rgb >> 8) & 0xff) / 255, static_cast<float>(rgb & 0xff) / 255,
                alpha};
    }

    [[nodiscard]] constexpr Color withAlpha(float alpha) const { return {r, g, b, alpha}; }
    [[nodiscard]] constexpr Color multipliedAlpha(float factor) const { return {r, g, b, a * factor}; }

    constexpr bool operator==(const Color &) const = default;
};

}  // namespace filepan::gfx
