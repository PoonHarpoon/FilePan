// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/adirstat/treemap.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace filepan::adirstat {
namespace {

// The worst aspect ratio of a row of areas, largest first, laid along a side of length side.
double worstRatio(double largest, double smallest, double sum, double side) {
    const double side2 = side * side;
    const double sum2 = sum * sum;
    return std::max(side2 * largest / sum2, sum2 / (side2 * smallest));
}

class Layout {
public:
    Layout(const Tree &tree, const TreemapOptions &options, std::vector<Tile> &out)
        : tree_(tree), options_(options), out_(out) {}

    void place(NodeId id, TileRect rect, int depth) {
        const std::size_t index = out_.size();
        out_.push_back({.node = id, .rect = rect, .depth = static_cast<std::uint16_t>(depth), .leaf = true});

        std::vector<NodeId> children;
        double total = 0;
        for (NodeId child = tree_[id].firstChild; child != kNoNode; child = tree_[child].nextSibling) {
            if (tree_[child].size > 0) {
                children.push_back(child);
                total += static_cast<double>(tree_[child].size);
            }
        }
        // The root fills the bounds; a folder below it frames its children.
        const float inset = depth > 0 ? options_.padding : 0;
        const TileRect inner{rect.x + inset, rect.y + inset, rect.width - 2 * inset, rect.height - 2 * inset};
        if (children.empty() || total <= 0 || inner.width < options_.minTileSize
            || inner.height < options_.minTileSize) {
            return;
        }
        out_[index].leaf = false;
        squarify(children, total, inner, depth + 1);
    }

private:
    void squarify(const std::vector<NodeId> &children, double total, TileRect rect, int depth) {
        double remaining = total;
        std::size_t i = 0;
        while (i < children.size()) {
            const double scale = static_cast<double>(rect.width) * rect.height / remaining;
            const bool columns = rect.width >= rect.height;  // A row runs along the shorter side.
            const double side = columns ? rect.height : rect.width;
            const double first = static_cast<double>(tree_[children[i]].size) * scale;
            double sum = 0;
            double best = std::numeric_limits<double>::infinity();
            std::size_t end = i;
            while (end < children.size()) {
                const double area = static_cast<double>(tree_[children[end]].size) * scale;
                const double ratio = worstRatio(first, area, sum + area, side);
                if (ratio > best) {
                    break;
                }
                best = ratio;
                sum += area;
                ++end;
            }
            const double rowSize = static_cast<double>(sum) / scale;
            const bool last = end == children.size();
            // The last row takes what's left, so rounding never leaves a gap.
            const auto thickness = last ? (columns ? rect.width : rect.height) : static_cast<float>(sum / side);

            float offset = columns ? rect.y : rect.x;
            const float limit = columns ? rect.y + rect.height : rect.x + rect.width;
            for (std::size_t k = i; k < end; ++k) {
                const double share = static_cast<double>(tree_[children[k]].size) / rowSize;
                const float length = k + 1 == end ? limit - offset : static_cast<float>(share * side);
                const TileRect child = columns ? TileRect{rect.x, offset, thickness, length}
                                               : TileRect{offset, rect.y, length, thickness};
                offset += length;
                if (child.width >= options_.minTileSize && child.height >= options_.minTileSize) {
                    place(children[k], child, depth);
                }
            }
            if (columns) {
                rect.x += thickness;
                rect.width -= thickness;
            } else {
                rect.y += thickness;
                rect.height -= thickness;
            }
            remaining -= rowSize;
            i = end;
            if (rect.width <= 0 || rect.height <= 0 || remaining <= 0) {
                break;
            }
        }
    }

    const Tree &tree_;
    const TreemapOptions &options_;
    std::vector<Tile> &out_;
};

}  // namespace

std::vector<Tile> layoutTreemap(const Tree &tree, NodeId root, TileRect bounds, const TreemapOptions &options) {
    std::vector<Tile> tiles;
    if (bounds.width <= 0 || bounds.height <= 0 || tree[root].size == 0) {
        return tiles;
    }
    Layout(tree, options, tiles).place(root, bounds, 0);
    return tiles;
}

std::optional<std::size_t> leafAt(std::span<const Tile> tiles, float x, float y) {
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        if (tiles[i].leaf && tiles[i].rect.contains(x, y)) {
            return i;
        }
    }
    return std::nullopt;
}

}  // namespace filepan::adirstat
