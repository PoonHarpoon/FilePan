// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "filepan/adirstat/treemap.hpp"
#include "test_util.hpp"

using filepan::adirstat::layoutTreemap;
using filepan::adirstat::NodeId;
using filepan::adirstat::NodeKind;
using filepan::adirstat::Tile;
using filepan::adirstat::TileRect;
using filepan::adirstat::Tree;

namespace {

NodeId addFile(Tree &tree, NodeId parent, const char *name, std::uint64_t size) {
    const NodeId id = tree.add(parent, name, NodeKind::kFile);
    tree.mutableNode(id).ownSize = size;
    return id;
}

// A root with files of 6, 6, 4, 3, 2, 2, 1, the example from the squarified treemaps paper, plus
// a directory holding two more.
Tree sample() {
    Tree tree("/r");
    const char *names[] = {"a", "b", "c", "d", "e", "f", "g"};
    const int sizes[] = {600, 600, 400, 300, 200, 200, 100};
    for (int i = 0; i < 7; ++i) {
        addFile(tree, tree.root(), names[i], sizes[i]);
    }
    const NodeId dir = tree.add(tree.root(), "dir", NodeKind::kDirectory);
    addFile(tree, dir, "x", 500);
    addFile(tree, dir, "y", 300);
    addFile(tree, tree.root(), "empty", 0);
    tree.finalize();
    return tree;
}

double area(const TileRect &r) { return static_cast<double>(r.width) * r.height; }

void testAreasMatchSizes() {
    const Tree tree = sample();
    const TileRect bounds{0, 0, 600, 400};
    const auto tiles = layoutTreemap(tree, tree.root(), bounds);
    EXPECT(tiles.front().node == tree.root() && !tiles.front().leaf);
    double leafArea = 0;
    double total = 0;
    for (NodeId c = tree[tree.root()].firstChild; c != filepan::adirstat::kNoNode; c = tree[c].nextSibling) {
        total += static_cast<double>(tree[c].size);
    }
    for (const Tile &tile : tiles) {
        EXPECT(tree[tile.node].size > 0);
        EXPECT(tile.rect.x >= -0.01f && tile.rect.y >= -0.01f);
        EXPECT(tile.rect.x + tile.rect.width <= 600.01f && tile.rect.y + tile.rect.height <= 400.01f);
        if (tile.leaf) {
            leafArea += area(tile.rect);
        }
        if (tile.depth == 1) {
            const double expected = area(bounds) * static_cast<double>(tree[tile.node].size) / total;
            EXPECT(std::abs(area(tile.rect) - expected) < 1.0);
        }
    }
    // The leaves tile the whole map.
    EXPECT(std::abs(leafArea - area(bounds)) < 2.0);
    // Squarified: no sliver worse than 1:5 for these sizes (the smallest ends up alone in the last strip).
    for (const Tile &tile : tiles) {
        const float ratio = std::max(tile.rect.width / tile.rect.height, tile.rect.height / tile.rect.width);
        EXPECT(ratio < 5.0f);
    }
    // Same input, same layout.
    const auto again = layoutTreemap(tree, tree.root(), bounds);
    EXPECT(again.size() == tiles.size());
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        EXPECT(again[i].node == tiles[i].node && again[i].rect.x == tiles[i].rect.x);
    }
}

void testSmallTilesFoldIntoTheirParent() {
    const Tree tree = sample();
    const auto tiles = layoutTreemap(tree, tree.root(), {0, 0, 600, 400}, {.minTileSize = 1000});
    EXPECT(tiles.size() == 1 && tiles[0].leaf);
    EXPECT(layoutTreemap(tree, tree.root(), {0, 0, 0, 10}).empty());
}

void testLeafAt() {
    const Tree tree = sample();
    const auto tiles = layoutTreemap(tree, tree.root(), {0, 0, 600, 400});
    for (const Tile &tile : tiles) {
        if (!tile.leaf) {
            continue;
        }
        const auto hit = filepan::adirstat::leafAt(tiles, tile.rect.x + tile.rect.width / 2,
                                                   tile.rect.y + tile.rect.height / 2);
        EXPECT(hit && tiles[*hit].node == tile.node);
    }
    EXPECT(!filepan::adirstat::leafAt(tiles, 700, 10));
}

void testFoldersFrameTheirChildren() {
    const Tree tree = sample();
    const auto tiles = layoutTreemap(tree, tree.root(), {0, 0, 600, 400}, {.padding = 6});
    NodeId dir = filepan::adirstat::kNoNode;
    for (const NodeId child : tree.children(tree.root())) {
        if (tree.name(child) == "dir") {
            dir = child;
        }
    }
    const Tile *frame = nullptr;
    for (const Tile &tile : tiles) {
        if (tile.node == dir) {
            frame = &tile;
        }
    }
    EXPECT(frame != nullptr && !frame->leaf);
    std::size_t inside = 0;
    for (const Tile &tile : tiles) {
        if (tree[tile.node].parent != dir) {
            continue;
        }
        ++inside;
        EXPECT(tile.rect.x >= frame->rect.x + 5.99f && tile.rect.y >= frame->rect.y + 5.99f);
        EXPECT(tile.rect.x + tile.rect.width <= frame->rect.x + frame->rect.width - 5.99f);
        EXPECT(tile.rect.y + tile.rect.height <= frame->rect.y + frame->rect.height - 5.99f);
    }
    EXPECT(inside == 2);
    // The root isn't framed: its children still reach the bounds.
    float right = 0;
    for (const Tile &tile : tiles) {
        if (tile.depth == 1) {
            right = std::max(right, tile.rect.x + tile.rect.width);
        }
    }
    EXPECT(right > 599.9f);
}

}  // namespace

int main() {
    testAreasMatchSizes();
    testSmallTilesFoldIntoTheirParent();
    testLeafAt();
    testFoldersFrameTheirChildren();
    std::puts("adirstat_treemap_test: OK");
    return 0;
}
