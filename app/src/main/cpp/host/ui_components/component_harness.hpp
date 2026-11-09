// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string_view>

#include "filepan/gfx/text.hpp"
#include "filepan/ui/ui_root.hpp"
#include "test_util.hpp"

namespace filepan::test {

// Single-line text with trivial metrics, like ui_test_util's, but with a cluster per code point
// so caret rects and hit testing work: every code point is size * 0.5 wide.
class ClusterTextSystem final : public gfx::TextSystem {
public:
    // Wrap at '\n' and, between any two code points, at maxWidth, when maxLines isn't 1.
    bool wrapLines = false;

    gfx::ShapedText shape(std::string_view utf8, const gfx::TextStyle &style,
                          const gfx::TextLayoutOptions &options) override {
        if (wrapLines && options.maxLines != 1) {
            return wrapped(utf8, style, options);
        }
        gfx::ShapedText out;
        const float advance = style.size * 0.5f;
        const float height = style.size * (style.lineHeight > 0 ? style.lineHeight : 1.2f);
        float x = 0;
        for (std::uint32_t i = 0; i < utf8.size(); ++i) {
            if ((static_cast<unsigned char>(utf8[i]) & 0xC0) == 0x80) {
                continue;
            }
            out.clusters.push_back({i, 0, x, advance, false});
            x += advance;
        }
        out.lines.push_back({0, style.size, height, x, 0, static_cast<std::uint32_t>(utf8.size())});
        out.size = {x, height};
        return out;
    }

    bool rasterize(gfx::FaceKey, std::uint32_t, float, float, gfx::GlyphBitmap &) override { return false; }

private:
    static gfx::ShapedText wrapped(std::string_view utf8, const gfx::TextStyle &style,
                                   const gfx::TextLayoutOptions &options) {
        gfx::ShapedText out;
        const float advance = style.size * 0.5f;
        const float height = style.size * (style.lineHeight > 0 ? style.lineHeight : 1.2f);
        const auto size = static_cast<std::uint32_t>(utf8.size());
        float x = 0;
        float width = 0;
        std::uint32_t lineStart = 0;
        const auto endLine = [&](std::uint32_t end) {
            const auto index = static_cast<float>(out.lines.size());
            out.lines.push_back({index * height, index * height + style.size, height, x, lineStart, end});
            width = std::max(width, x);
            x = 0;
            lineStart = end;
        };
        for (std::uint32_t i = 0; i < size; ++i) {
            if ((static_cast<unsigned char>(utf8[i]) & 0xC0) == 0x80) {
                continue;
            }
            if (x > 0 && x + advance > options.maxWidth) {
                endLine(i);
            }
            const auto line = static_cast<std::uint32_t>(out.lines.size());
            out.clusters.push_back({i, line, x, utf8[i] == '\n' ? 0 : advance, false});
            if (utf8[i] == '\n') {
                endLine(i + 1);
            } else {
                x += advance;
            }
        }
        endLine(size);
        out.size = {width, height * static_cast<float>(out.lines.size())};
        return out;
    }
};

// A UiRoot on a fake clock: 400x800 dp at density 2, events in dp.
class ComponentHarness {
public:
    static constexpr float kDensity = 2;

    ComponentHarness() {
        loop.setClock([this] { return now; });
        root = std::make_unique<ui::UiRoot>(loop, &text);
        root->env().density.set(kDensity);
        root->env().windowSize.set({400 * kDensity, 800 * kDensity});
    }

    bool frame() { return root->frame(now).has_value(); }
    // Moves the clock, running due timers and a frame per 8 ms.
    void advance(std::int64_t ms) {
        for (std::int64_t step = 0; step < ms; step += 8) {
            now += 8'000'000;
            loop.runPending();
            frame();
        }
    }
    void pointer(ui::PointerAction action, float x, float y) {
        root->dispatchPointer({.pointer = 0, .action = action, .position = {x * kDensity, y * kDensity}, .timeNs = now});
    }
    void down(float x, float y) { pointer(ui::PointerAction::down, x, y); }
    void move(float x, float y) { pointer(ui::PointerAction::move, x, y); }
    void up(float x, float y) { pointer(ui::PointerAction::up, x, y); }
    void tap(float x, float y) {
        down(x, y);
        advance(16);
        up(x, y);
        frame();
    }
    // A drag from (x0, y0) to (x1, y1) over ms, in 8 ms steps.
    void drag(float x0, float y0, float x1, float y1, std::int64_t ms, bool release = true) {
        down(x0, y0);
        const int steps = static_cast<int>(ms / 8);
        for (int i = 1; i <= steps; ++i) {
            now += 8'000'000;
            const float t = static_cast<float>(i) / static_cast<float>(steps);
            move(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t);
        }
        if (release) {
            up(x1, y1);
        }
        frame();
    }
    // The node's center in window dp.
    gfx::Vec2 center(const ui::Node &node) const;

    const ui::SemanticsNode *semantics(const ui::Node &node) {
        root->setAccessibilityEnabled(true);
        frame();
        return root->semantics().find(node.id());
    }

    std::int64_t now = 1'000'000'000;
    ui::UiLoop loop;
    ClusterTextSystem text;
    std::unique_ptr<ui::UiRoot> root;
};

inline gfx::Vec2 ComponentHarness::center(const ui::Node &node) const {
    gfx::Vec2 p{node.frame().width / 2, node.frame().height / 2};
    gfx::Vec2 offset{node.frame().x, node.frame().y};
    for (const ui::Node *n = node.parent(); n != nullptr; n = n->parent()) {
        const gfx::Transform children = n->childrenTransformPx(kDensity);
        offset = offset + gfx::Vec2{children.tx / kDensity, children.ty / kDensity} + gfx::Vec2{n->frame().x, n->frame().y};
    }
    return p + offset;
}

inline bool near(float a, float b, float epsilon = 0.01f) { return std::abs(a - b) <= epsilon; }

}  // namespace filepan::test
