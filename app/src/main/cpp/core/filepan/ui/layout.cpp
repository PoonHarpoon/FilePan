// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/layout.hpp"

#include <algorithm>
#include <vector>

namespace filepan::ui {

namespace {

// Like CSS, min wins over max.
float clampSize(float value, float min, float max) { return std::max(min, std::min(value, max)); }

float nonNegative(float value) { return std::max(0.0f, value); }

struct FlexItem {
    Node *node = nullptr;
    float marginMainStart = 0;
    float marginMainEnd = 0;
    float marginCrossStart = 0;
    float marginCrossEnd = 0;
    float base = 0;
    float hypothetical = 0;
    float minMain = 0;
    float maxMain = kInfinity;
    float minCross = 0;
    float maxCross = kInfinity;
    float main = 0;
    float target = 0;  // Unclamped size from the last flex round.
    float cross = 0;
    float grow = 0;
    float shrink = 0;
    Align align = Align::stretch;
    bool crossAuto = true;
    bool frozen = false;

    [[nodiscard]] float mainMargins() const { return marginMainStart + marginMainEnd; }
    [[nodiscard]] float crossMargins() const { return marginCrossStart + marginCrossEnd; }
};

struct FlexLine {
    std::size_t begin = 0;
    std::size_t end = 0;
    float cross = 0;
};

// CSS "resolving flexible lengths", for items [begin, end) sharing available main space.
void resolveFlexibleLengths(std::vector<FlexItem> &items, std::size_t begin, std::size_t end,
                            float available) {
    float hypotheticalSum = 0;
    for (std::size_t i = begin; i < end; ++i) {
        hypotheticalSum += items[i].hypothetical + items[i].mainMargins();
    }
    const bool growing = hypotheticalSum < available;
    for (std::size_t i = begin; i < end; ++i) {
        auto &item = items[i];
        const float factor = growing ? item.grow : item.shrink;
        item.frozen = factor == 0 || (growing && item.base > item.hypothetical) ||
                      (!growing && item.base < item.hypothetical);
        item.main = item.hypothetical;
    }
    for (std::size_t round = 0; round <= end - begin; ++round) {
        float used = 0;
        float factorSum = 0;
        bool anyUnfrozen = false;
        for (std::size_t i = begin; i < end; ++i) {
            const auto &item = items[i];
            used += item.mainMargins() + (item.frozen ? item.main : item.base);
            if (!item.frozen) {
                anyUnfrozen = true;
                factorSum += growing ? item.grow : item.shrink * item.base;
            }
        }
        if (!anyUnfrozen) {
            return;
        }
        const float free = available - used;
        float violation = 0;
        for (std::size_t i = begin; i < end; ++i) {
            auto &item = items[i];
            if (item.frozen) {
                continue;
            }
            float target = item.base;
            if (factorSum > 0) {
                const float factor = growing ? item.grow : item.shrink * item.base;
                target += free * factor / factorSum;
            }
            const float clamped = nonNegative(clampSize(target, item.minMain, item.maxMain));
            violation += clamped - target;
            item.main = clamped;
            item.target = target;
        }
        for (std::size_t i = begin; i < end; ++i) {
            auto &item = items[i];
            if (item.frozen) {
                continue;
            }
            if (violation == 0 || (violation > 0 && item.main > item.target) ||
                (violation < 0 && item.main < item.target)) {
                item.frozen = true;
            }
        }
    }
}

float alignOffset(Align align, float free) {
    switch (align) {
    case Align::end:
        return free;
    case Align::center:
        return free / 2;
    default:
        return 0;
    }
}

Align justifyAsAlign(Justify justify) {
    switch (justify) {
    case Justify::end:
        return Align::end;
    case Justify::center:
        return Align::center;
    default:
        return Align::start;
    }
}

}  // namespace

void LayoutEngine::layoutRoot(Node &root, gfx::Size size) {
    compute(root, size.width, size.height, size.width, size.height, true);
    place(root, {0, 0, size.width, size.height});
}

gfx::Size LayoutEngine::compute(Node &node, std::optional<float> width, std::optional<float> height,
                                float availableWidth, float availableHeight, bool perform) {
    const bool fixedSize = width.has_value() && height.has_value();
    const auto matches = [&](const Node::LayoutCacheEntry &entry) {
        if (entry.width != width || entry.height != height) {
            return false;
        }
        // With both sizes known, the available space doesn't matter.
        return fixedSize ||
               (entry.availableWidth == availableWidth && entry.availableHeight == availableHeight);
    };
    if (node.layoutValid_ && matches(node.layoutCache_)) {
        return node.layoutCache_.result;
    }
    if (!perform) {
        for (std::size_t i = 0; i < node.measureCacheCount_; ++i) {
            if (matches(node.measureCache_[i])) {
                return node.measureCache_[i].result;
            }
        }
    }
    ++computeCount_;
    const gfx::Size result =
            computeUncached(node, width, height, availableWidth, availableHeight, perform);
    const Node::LayoutCacheEntry entry{width, height, availableWidth, availableHeight, result};
    if (perform) {
        node.layoutCache_ = entry;
        node.layoutValid_ = true;
    } else {
        node.measureCache_[node.measureCacheNext_] = entry;
        node.measureCacheNext_ = static_cast<std::uint8_t>((node.measureCacheNext_ + 1) % Node::kMeasureCacheSize);
        node.measureCacheCount_ = static_cast<std::uint8_t>(
                std::min<std::size_t>(node.measureCacheCount_ + 1, Node::kMeasureCacheSize));
    }
    return result;
}

gfx::Size LayoutEngine::computeUncached(Node &node, std::optional<float> width,
                                        std::optional<float> height, float availableWidth,
                                        float availableHeight, bool perform) {
    const Style &style = node.style_;
    if (!width) {
        width = style.width.resolve(availableWidth);
    }
    if (!height) {
        height = style.height.resolve(availableHeight);
    }
    if (style.aspectRatio > 0) {
        if (width && !height) {
            height = *width / style.aspectRatio;
        } else if (height && !width) {
            width = *height * style.aspectRatio;
        }
    }
    if (width) {
        width = clampSize(*width, style.minWidth, style.maxWidth);
    }
    if (height) {
        height = clampSize(*height, style.minHeight, style.maxHeight);
    }

    if (node.hasCustomLayout()) {
        gfx::Size size = node.measureCustom(*this, width, height, availableWidth, availableHeight);
        size.width = clampSize(size.width, style.minWidth, style.maxWidth);
        size.height = clampSize(size.height, style.minHeight, style.maxHeight);
        if (perform) {
            node.layoutCustom(*this, size);
        }
        return size;
    }

    if (node.hasMeasure()) {
        if (width && height) {
            return {*width, *height};
        }
        const Edges &padding = style.padding;
        const float maxWidth =
                nonNegative((width ? *width : std::min(availableWidth, style.maxWidth)) - padding.horizontal());
        const float maxHeight =
                nonNegative((height ? *height : std::min(availableHeight, style.maxHeight)) - padding.vertical());
        const gfx::Size content = node.measureContent(context_, maxWidth, maxHeight);
        return {width ? *width
                      : clampSize(content.width + padding.horizontal(), style.minWidth, style.maxWidth),
                height ? *height
                       : clampSize(content.height + padding.vertical(), style.minHeight, style.maxHeight)};
    }

    return computeFlex(node, width, height, availableWidth, availableHeight, perform);
}

gfx::Size LayoutEngine::computeFlex(Node &node, std::optional<float> width, std::optional<float> height,
                                    float availableWidth, float availableHeight, bool perform) {
    const Style &style = node.style_;
    const bool row = style.direction == Direction::row;
    const Edges &padding = style.padding;
    const float padMain = row ? padding.horizontal() : padding.vertical();
    const float padCross = row ? padding.vertical() : padding.horizontal();
    const float padMainStart = row ? padding.left : padding.top;
    const float padCrossStart = row ? padding.top : padding.left;
    const std::optional<float> mainSize = row ? width : height;
    const std::optional<float> crossSize = row ? height : width;
    const float availableMain = row ? availableWidth : availableHeight;
    const float availableCross = row ? availableHeight : availableWidth;
    const float minMainStyle = row ? style.minWidth : style.minHeight;
    const float maxMainStyle = row ? style.maxWidth : style.maxHeight;
    const float minCrossStyle = row ? style.minHeight : style.minWidth;
    const float maxCrossStyle = row ? style.maxHeight : style.maxWidth;

    const std::optional<float> innerMain =
            mainSize ? std::optional<float>(nonNegative(*mainSize - padMain)) : std::nullopt;
    const std::optional<float> innerCross =
            crossSize ? std::optional<float>(nonNegative(*crossSize - padCross)) : std::nullopt;
    const float innerMainAvailable =
            innerMain ? *innerMain : nonNegative(std::min(availableMain, maxMainStyle) - padMain);
    const float innerCrossAvailable =
            innerCross ? *innerCross : nonNegative(std::min(availableCross, maxCrossStyle) - padCross);

    const auto measure = [&](Node &child, std::optional<float> main, std::optional<float> cross,
                             float availMain, float availCross, bool performChild) {
        return row ? compute(child, main, cross, availMain, availCross, performChild)
                   : compute(child, cross, main, availCross, availMain, performChild);
    };
    const auto mainOf = [row](gfx::Size size) { return row ? size.width : size.height; };
    const auto crossOf = [row](gfx::Size size) { return row ? size.height : size.width; };

    std::vector<FlexItem> items;
    items.reserve(node.children_.size());
    std::vector<std::optional<float>> fixedCross;
    fixedCross.reserve(node.children_.size());
    for (const auto &childPtr : node.children_) {
        Node &child = *childPtr;
        const Style &cs = child.style_;
        if (!cs.display || cs.position == Position::absolute) {
            continue;
        }
        FlexItem item;
        item.node = &child;
        item.marginMainStart = row ? cs.margin.left : cs.margin.top;
        item.marginMainEnd = row ? cs.margin.right : cs.margin.bottom;
        item.marginCrossStart = row ? cs.margin.top : cs.margin.left;
        item.marginCrossEnd = row ? cs.margin.bottom : cs.margin.right;
        item.align = cs.alignSelf == Align::automatic ? style.alignItems : cs.alignSelf;
        item.minMain = row ? cs.minWidth : cs.minHeight;
        item.maxMain = row ? cs.maxWidth : cs.maxHeight;
        item.minCross = row ? cs.minHeight : cs.minWidth;
        item.maxCross = row ? cs.maxHeight : cs.maxWidth;
        item.grow = cs.grow;
        // Scroll containers always shrink: their content scrolls rather than overflowing.
        item.shrink = child.hasCustomLayout() ? std::max(cs.shrink, 1.0f) : cs.shrink;

        const Length &mainLength = row ? cs.width : cs.height;
        const Length &crossLength = row ? cs.height : cs.width;
        std::optional<float> cross = crossLength.resolve(innerCross ? *innerCross : kInfinity);
        item.crossAuto = !cross.has_value();
        if (cross) {
            cross = clampSize(*cross, item.minCross, item.maxCross);
        } else if (item.align == Align::stretch && innerCross) {
            cross = nonNegative(clampSize(*innerCross - item.crossMargins(), item.minCross, item.maxCross));
        }

        std::optional<float> basis = cs.basis.resolve(innerMain ? *innerMain : kInfinity);
        if (!basis) {
            basis = mainLength.resolve(innerMain ? *innerMain : kInfinity);
        }
        if (!basis && cs.aspectRatio > 0 && cross) {
            basis = row ? *cross * cs.aspectRatio : *cross / cs.aspectRatio;
        }
        if (!basis) {
            const gfx::Size size =
                    measure(child, std::nullopt, cross, nonNegative(innerMainAvailable - item.mainMargins()),
                            nonNegative(innerCrossAvailable - item.crossMargins()), false);
            basis = mainOf(size);
        }
        item.base = *basis;
        item.hypothetical = nonNegative(clampSize(item.base, item.minMain, item.maxMain));
        items.push_back(item);
        fixedCross.push_back(cross);
    }

    std::vector<FlexLine> lines;
    if (!style.wrap || innerMainAvailable >= kInfinity || items.empty()) {
        lines.push_back({0, items.size(), 0});
    } else {
        std::size_t start = 0;
        float used = 0;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const float size = items[i].hypothetical + items[i].mainMargins();
            if (i > start && used + style.gap + size > innerMainAvailable) {
                lines.push_back({start, i, 0});
                start = i;
                used = size;
            } else {
                used += (i > start ? style.gap : 0) + size;
            }
        }
        lines.push_back({start, items.size(), 0});
    }

    float contentMain = 0;
    for (auto &line : lines) {
        const std::size_t count = line.end - line.begin;
        const float gaps = count > 1 ? style.gap * static_cast<float>(count - 1) : 0;
        float lineMain;
        if (innerMain) {
            lineMain = *innerMain;
        } else {
            // Indefinite main size: fit the content, but no wider than the space available,
            // so items that may shrink (text) do.
            float sum = gaps;
            for (std::size_t i = line.begin; i < line.end; ++i) {
                sum += items[i].hypothetical + items[i].mainMargins();
            }
            lineMain = clampSize(std::min(sum, innerMainAvailable) + padMain, minMainStyle, maxMainStyle) - padMain;
        }
        resolveFlexibleLengths(items, line.begin, line.end, nonNegative(lineMain - gaps));

        float used = gaps;
        for (std::size_t i = line.begin; i < line.end; ++i) {
            auto &item = items[i];
            used += item.main + item.mainMargins();
            if (fixedCross[i]) {
                item.cross = *fixedCross[i];
            } else {
                const gfx::Size size = measure(*item.node, item.main, std::nullopt, item.main,
                                               nonNegative(innerCrossAvailable - item.crossMargins()), false);
                item.cross = clampSize(crossOf(size), item.minCross, item.maxCross);
            }
            line.cross = std::max(line.cross, item.cross + item.crossMargins());
        }
        contentMain = std::max(contentMain, used);
    }
    if (lines.size() == 1 && innerCross) {
        lines[0].cross = *innerCross;
    }
    float contentCross = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        contentCross += lines[i].cross + (i > 0 ? style.lineGap : 0);
    }

    const float outMain = mainSize ? *mainSize : clampSize(contentMain + padMain, minMainStyle, maxMainStyle);
    const float outCross = crossSize ? *crossSize : clampSize(contentCross + padCross, minCrossStyle, maxCrossStyle);
    const gfx::Size size = row ? gfx::Size{outMain, outCross} : gfx::Size{outCross, outMain};
    if (!perform) {
        return size;
    }

    const float finalInnerMain = nonNegative(outMain - padMain);
    const float finalInnerCross = nonNegative(outCross - padCross);
    if (lines.size() == 1) {
        lines[0].cross = finalInnerCross;
    }
    float crossCursor = padCrossStart;
    for (const auto &line : lines) {
        const std::size_t count = line.end - line.begin;
        float used = count > 1 ? style.gap * static_cast<float>(count - 1) : 0;
        for (std::size_t i = line.begin; i < line.end; ++i) {
            used += items[i].main + items[i].mainMargins();
        }
        const float free = finalInnerMain - used;
        float offset = 0;
        float between = 0;
        switch (style.justify) {
        case Justify::start:
            break;
        case Justify::end:
            offset = free;
            break;
        case Justify::center:
            offset = free / 2;
            break;
        case Justify::spaceBetween:
            between = free > 0 && count > 1 ? free / static_cast<float>(count - 1) : 0;
            break;
        case Justify::spaceAround:
            between = free > 0 ? free / static_cast<float>(count) : 0;
            offset = between / 2;
            break;
        case Justify::spaceEvenly:
            between = free > 0 ? free / static_cast<float>(count + 1) : 0;
            offset = between;
            break;
        }
        float cursor = padMainStart + offset;
        for (std::size_t i = line.begin; i < line.end; ++i) {
            auto &item = items[i];
            cursor += item.marginMainStart;
            float itemCross = item.cross;
            if (item.align == Align::stretch && item.crossAuto) {
                itemCross = nonNegative(clampSize(line.cross - item.crossMargins(), item.minCross, item.maxCross));
            }
            const float crossFree = line.cross - item.crossMargins() - itemCross;
            const float crossPosition = crossCursor + item.marginCrossStart + alignOffset(item.align, crossFree);
            const gfx::Size childSize =
                    measure(*item.node, item.main, itemCross, finalInnerMain, finalInnerCross, true);
            place(*item.node, row ? gfx::Rect{cursor, crossPosition, childSize.width, childSize.height}
                                  : gfx::Rect{crossPosition, cursor, childSize.width, childSize.height});
            cursor += item.main + item.marginMainEnd + style.gap + between;
        }
        crossCursor += line.cross + style.lineGap;
    }
    layoutAbsolute(node, size);
    return size;
}

void LayoutEngine::layoutAbsolute(Node &node, gfx::Size size) {
    const Style &style = node.style_;
    const bool row = style.direction == Direction::row;
    const Align horizontalAlign = row ? justifyAsAlign(style.justify) : style.alignItems;
    const Align verticalAlign = row ? style.alignItems : justifyAsAlign(style.justify);
    for (const auto &childPtr : node.children_) {
        Node &child = *childPtr;
        const Style &cs = child.style_;
        if (!cs.display || cs.position != Position::absolute) {
            continue;
        }
        std::optional<float> width = cs.width.resolve(size.width);
        std::optional<float> height = cs.height.resolve(size.height);
        if (!width && cs.left && cs.right) {
            width = nonNegative(size.width - *cs.left - *cs.right - cs.margin.horizontal());
        }
        if (!height && cs.top && cs.bottom) {
            height = nonNegative(size.height - *cs.top - *cs.bottom - cs.margin.vertical());
        }
        const gfx::Size childSize = compute(child, width, height, nonNegative(size.width - cs.margin.horizontal()),
                                            nonNegative(size.height - cs.margin.vertical()), true);
        const auto position = [](std::optional<float> start, std::optional<float> end, float marginStart,
                                 float marginEnd, float padStart, float padEnd, float container, float child,
                                 Align align) {
            if (start) {
                return *start + marginStart;
            }
            if (end) {
                return container - *end - marginEnd - child;
            }
            const float free = container - padStart - padEnd - marginStart - marginEnd - child;
            return padStart + marginStart + alignOffset(align == Align::stretch ? Align::start : align, free);
        };
        const Align selfAlignX = !row && cs.alignSelf != Align::automatic ? cs.alignSelf : horizontalAlign;
        const Align selfAlignY = row && cs.alignSelf != Align::automatic ? cs.alignSelf : verticalAlign;
        const float x = position(cs.left, cs.right, cs.margin.left, cs.margin.right, style.padding.left,
                                 style.padding.right, size.width, childSize.width, selfAlignX);
        const float y = position(cs.top, cs.bottom, cs.margin.top, cs.margin.bottom, style.padding.top,
                                 style.padding.bottom, size.height, childSize.height, selfAlignY);
        place(child, {x, y, childSize.width, childSize.height});
    }
}

void LayoutEngine::place(Node &node, const gfx::Rect &frame) {
    if (frame == node.frame_) {
        return;
    }
    const bool resized = frame.width != node.frame_.width || frame.height != node.frame_.height;
    node.frame_ = frame;
    if (resized) {
        node.paintDirty_ = true;
        node.compositeDirty_ = true;
        node.onSizeChanged();
    }
    if (node.parent_ != nullptr) {
        node.parent_->invalidateComposite();
    }
}

}  // namespace filepan::ui
