// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "filepan/adirstat/tree.hpp"

namespace filepan::adirstat {

struct TileRect {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;

    [[nodiscard]] bool contains(float px, float py) const {
        return px >= x && py >= y && px < x + width && py < y + height;
    }
};

// One rectangle of the treemap. A leaf is drawn as a tile; the others frame their children.
struct Tile {
    NodeId node;
    TileRect rect;
    std::uint16_t depth;  // 0 for the layout's root.
    bool leaf;
};

struct TreemapOptions {
    // Children smaller than this on either side aren't laid out, so their parent shows as one
    // leaf. In the units of the bounds (pixels).
    float minTileSize = 2;
    // Each folder below the root lays its children out this far inside its own rect, so it shows
    // as a frame around them.
    float padding = 0;
};

// Lays out the subtree under root in bounds with the squarified algorithm (Bruls, Huizing and
// van Wijk), children in the tree's order (largest first). Tiles come out in preorder, root
// first; empty children get none.
[[nodiscard]] std::vector<Tile> layoutTreemap(const Tree &tree, NodeId root, TileRect bounds,
                                              const TreemapOptions &options = {});

// The leaf at a point, as an index into tiles; nullopt outside them.
[[nodiscard]] std::optional<std::size_t> leafAt(std::span<const Tile> tiles, float x, float y);

}  // namespace filepan::adirstat
