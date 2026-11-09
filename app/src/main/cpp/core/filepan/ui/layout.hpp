// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <optional>

#include "filepan/gfx/geometry.hpp"
#include "filepan/ui/node.hpp"

// Flexbox-subset layout in dp.
//
// compute() sizes a node given an optional known width and height (set by its parent's flex
// resolution or stretching) and the available space (possibly infinite), and, when perform is
// true, positions its children too. Supported: row/column, justify, align-items/self, gap,
// padding, margin, grow/shrink/basis, min/max/fixed and percentage sizes, aspect ratio, wrap, and
// absolute children placed by insets. Leaves measure through Node::measureContent; scroll
// containers and virtual lists lay out their children through Node::layoutCustom.
//
// Results are cached per node, keyed on the constraints, and markLayoutDirty clears the cache
// of the node and its ancestors, so a clean subtree costs one cache lookup. Frames are dp; the
// recorder converts to px with the density.
namespace filepan::ui {

class LayoutEngine {
public:
    explicit LayoutEngine(const LayoutContext &context) : context_(context) {}

    [[nodiscard]] const LayoutContext &context() const { return context_; }

    // Lays root out at exactly size and places it at the origin.
    void layoutRoot(Node &root, gfx::Size size);

    gfx::Size compute(Node &node, std::optional<float> width, std::optional<float> height,
                      float availableWidth, float availableHeight, bool perform);

    // Sets a child's frame, invalidating the paint or composite it affects.
    static void place(Node &node, const gfx::Rect &frame);

    // How many times a node's layout actually ran (cache misses); for tests.
    [[nodiscard]] std::uint32_t computeCount() const { return computeCount_; }

private:
    gfx::Size computeUncached(Node &node, std::optional<float> width, std::optional<float> height,
                              float availableWidth, float availableHeight, bool perform);
    gfx::Size computeFlex(Node &node, std::optional<float> width, std::optional<float> height,
                          float availableWidth, float availableHeight, bool perform);
    void layoutAbsolute(Node &node, gfx::Size size);

    LayoutContext context_;
    std::uint32_t computeCount_ = 0;
};

}  // namespace filepan::ui
