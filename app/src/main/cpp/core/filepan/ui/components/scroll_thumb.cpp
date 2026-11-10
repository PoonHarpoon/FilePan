// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/components/scroll_thumb.hpp"

#include <algorithm>

#include "filepan/ui/components/common.hpp"
#include "filepan/ui/gesture.hpp"
#include "filepan/ui/theme.hpp"

namespace filepan::ui::components {
namespace {

// Content shorter than this many screens gets no thumb: it scrolls easily enough.
constexpr float kMinScreens = 1.5f;
constexpr float kMinLength = 36;
constexpr float kThinWidth = 3;
constexpr float kExpandedWidth = 8;
constexpr float kEdgeGap = 2;

}  // namespace

std::shared_ptr<ScrollThumb> ScrollThumb::create(ScrollNode &scroll, std::function<void()> dependencies) {
    auto node = std::make_shared<ScrollThumb>(scroll);
    ScrollThumb *raw = node.get();
    node->setDebugName("ScrollThumb");
    node->setStyle({.width = kWidth, .position = Position::absolute, .right = 0});
    node->setHitTestBehavior(HitTestBehavior::opaque);
    // The list scrolls for screen readers by itself.
    node->updateSemantics([](Semantics &s) { s.hidden = true; });

    auto tap = std::make_shared<TapRecognizer>();
    tap->onTapDown = [raw](gfx::Vec2) { raw->setExpanded(true); };
    tap->onTap = [raw] { raw->setExpanded(false); };
    tap->onTapCancel = [raw] {
        if (!raw->scroll_.animating()) {
            raw->setExpanded(false);
        }
    };
    node->addGestureRecognizer(tap);

    auto drag = std::make_shared<DragRecognizer>(DragAxis::vertical);
    drag->onStart = [raw](gfx::Vec2 local) {
        const auto g = raw->geometry();
        if (!g) {
            return;
        }
        raw->setExpanded(true);
        raw->scroll_.stopScrolling();
        // Grabbed off the thumb: it jumps there first.
        const float y = local.y / environment().density.peek();
        if (y < g->top || y > g->top + g->length) {
            raw->centerAt(y);
        }
    };
    drag->onUpdate = [raw](gfx::Vec2 delta) { raw->dragBy(delta.y / environment().density.peek()); };
    drag->onEnd = [raw](gfx::Vec2) { raw->setExpanded(false); };
    drag->onCancel = [raw] { raw->setExpanded(false); };
    node->addGestureRecognizer(drag);

    node->bind([raw, dependencies = std::move(dependencies)] {
        (void)raw->scroll_.offsetSignal()();
        if (dependencies) {
            dependencies();
        }
        untrack([&] { raw->markPaintDirty(); });
    });
    return node;
}

std::optional<ScrollThumb::Geometry> ScrollThumb::geometry() const {
    const float track = frame().height;
    const float viewport = scroll_.viewportExtent();
    const float content = scroll_.contentExtent();
    const float max = scroll_.maxOffset();
    if (track <= kMinLength || viewport <= 0 || content < viewport * kMinScreens || max <= 0) {
        return std::nullopt;
    }
    const float length = std::clamp(track * viewport / content, kMinLength, track);
    const float at = std::clamp(scroll_.offset() / max, 0.0f, 1.0f);
    return Geometry{.top = at * (track - length), .length = length};
}

void ScrollThumb::setExpanded(bool expanded) {
    if (expanded_ != expanded) {
        expanded_ = expanded;
        markPaintDirty();
    }
}

void ScrollThumb::centerAt(float y) {
    const auto g = geometry();
    if (!g) {
        return;
    }
    const float travel = frame().height - g->length;
    const float at = std::clamp((y - g->length / 2) / std::max(travel, 1.0f), 0.0f, 1.0f);
    scroll_.scrollTo(at * scroll_.maxOffset());
}

void ScrollThumb::dragBy(float dp) {
    const auto g = geometry();
    if (!g) {
        return;
    }
    // The thumb follows the finger: its travel stands for the whole list.
    const float travel = std::max(frame().height - g->length, 1.0f);
    const float max = scroll_.maxOffset();
    scroll_.scrollTo(std::clamp(scroll_.offset() + dp * max / travel, 0.0f, max));
}

void ScrollThumb::paint(PaintContext &context) {
    const auto g = geometry();
    if (!g) {
        return;
    }
    const Theme &theme = currentTheme();
    const float density = context.density;
    const gfx::Rect b = context.bounds();
    const float width = (expanded_ ? kExpandedWidth : kThinWidth) * density;
    const gfx::Rect thumb{b.x + b.width - width - kEdgeGap * density, b.y + g->top * density, width,
                          g->length * density};
    context.list.fill(gfx::RoundedRect{thumb, gfx::CornerRadii::uniform(width / 2)},
                      ink(theme, expanded_ ? 0.55f : 0.28f));
}

}  // namespace filepan::ui::components
