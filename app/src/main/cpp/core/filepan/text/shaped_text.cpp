// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// gfx::ShapedText's caret queries. They only read the shaped data, so they're in their own
// library (filepan_shaped_text) that the UI framework can link without fonts or HarfBuzz.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

#include "filepan/gfx/text.hpp"

namespace filepan::gfx {

namespace {

// The byte after cluster index in logical order: the next cluster's start on the same line, or
// the line's end.
std::uint32_t byteAfter(const ShapedText &text, std::size_t index) {
    const TextCluster &cluster = text.clusters[index];
    if (index + 1 < text.clusters.size() && text.clusters[index + 1].line == cluster.line) {
        return text.clusters[index + 1].byte;
    }
    return text.lines[cluster.line].endByte;
}

}  // namespace

std::uint32_t ShapedText::hitTest(Vec2 point) const {
    if (lines.empty()) {
        return 0;
    }
    std::size_t line = lines.size() - 1;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (point.y < lines[i].top + lines[i].height) {
            line = i;
            break;
        }
    }

    // The cluster on the line nearest to point.x.
    std::size_t best = clusters.size();
    float bestDistance = std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < clusters.size(); ++i) {
        const TextCluster &cluster = clusters[i];
        if (cluster.line != line) {
            continue;
        }
        const float left = cluster.x;
        const float right = cluster.x + cluster.width;
        const float distance = point.x < left    ? left - point.x
                               : point.x > right ? point.x - right
                                                 : 0;
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    if (best == clusters.size()) {
        return lines[line].startByte;
    }
    const TextCluster &cluster = clusters[best];
    const bool leftHalf = point.x < cluster.x + cluster.width / 2;
    // The caret goes before a cluster on its leading side: left for left-to-right text.
    return leftHalf != cluster.rightToLeft ? cluster.byte : byteAfter(*this, best);
}

Rect ShapedText::caretRect(std::uint32_t byte) const {
    if (lines.empty()) {
        return {};
    }
    if (clusters.empty()) {
        return {0, lines.front().top, 0, lines.front().height};
    }
    // The last cluster starting at or before byte.
    const auto it =
            std::upper_bound(clusters.begin(), clusters.end(), byte,
                             [](std::uint32_t b, const TextCluster &c) { return b < c.byte; });
    const TextCluster &cluster = it == clusters.begin() ? clusters.front() : *(it - 1);
    const TextLine &line = lines[cluster.line];
    // Past the last cluster's start: after it (byte is normally the end of the text).
    const bool after = it == clusters.end() && byte > cluster.byte;
    const bool rightEdge = after != cluster.rightToLeft;
    const float x = rightEdge ? cluster.x + cluster.width : cluster.x;
    return {x, line.top, 0, line.height};
}

}  // namespace filepan::gfx
