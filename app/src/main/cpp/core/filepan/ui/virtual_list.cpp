// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/virtual_list.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

#include "filepan/ui/layout.hpp"

namespace filepan::ui {

namespace {

// Pooled nodes per item type beyond which recycled nodes are dropped.
constexpr std::size_t kMaxPooled = 64;

}  // namespace

// --- ExtentIndex ---

void VirtualList::ExtentIndex::reset(std::size_t count, float value) {
    values_.assign(count, value);
    rebuild();
}

void VirtualList::ExtentIndex::rebuild() {
    const std::size_t n = values_.size();
    tree_.assign(n + 1, 0);
    for (std::size_t i = 1; i <= n; ++i) {
        tree_[i] += values_[i - 1];
        const std::size_t parent = i + (i & (~i + 1));
        if (parent <= n) {
            tree_[parent] += tree_[i];
        }
    }
}

void VirtualList::ExtentIndex::set(std::size_t index, float value) {
    const double delta = static_cast<double>(value) - values_[index];
    if (delta == 0) {
        return;
    }
    values_[index] = value;
    for (std::size_t j = index + 1; j < tree_.size(); j += j & (~j + 1)) {
        tree_[j] += delta;
    }
}

double VirtualList::ExtentIndex::prefix(std::size_t count) const {
    double sum = 0;
    for (std::size_t j = std::min(count, values_.size()); j > 0; j -= j & (~j + 1)) {
        sum += tree_[j];
    }
    return sum;
}

std::size_t VirtualList::ExtentIndex::find(double offset) const {
    const std::size_t n = values_.size();
    std::size_t position = 0;
    double remaining = offset;
    for (std::size_t step = n == 0 ? 0 : std::bit_floor(n); step > 0; step >>= 1) {
        if (position + step <= n && tree_[position + step] <= remaining) {
            position += step;
            remaining -= tree_[position];
        }
    }
    return position;
}

// --- VirtualList ---

std::shared_ptr<VirtualList> VirtualList::create(ScrollAxis axis) { return std::make_shared<VirtualList>(axis); }

VirtualList::VirtualList(ScrollAxis axis) : ScrollNode(axis) {
    updateSemantics([](Semantics &s) { s.role = Role::list; });
}

void VirtualList::setKeys(KeyFn key, IndexOfKeyFn indexOfKey) {
    key_ = std::move(key);
    indexOfKey_ = std::move(indexOfKey);
}

void VirtualList::setHeader(NodePtr header) {
    if (header_) {
        removeChild(header_.get());
    }
    header_ = std::move(header);
    if (header_) {
        insertChild(0, header_);
    }
    markLayoutDirty();
}

void VirtualList::markStructureChanged() {
    anchorPending_ = true;
    markLayoutDirty();
}

void VirtualList::setItemCount(std::size_t count) {
    count_ = count;
    if (key_ && indexOfKey_ && anchorKey_) {
        if (auto index = indexOfKey_(*anchorKey_)) {
            anchorIndex_ = *index;
        }
    }
    anchorIndex_ = count_ == 0 ? 0 : std::min(anchorIndex_, count_ - 1);
    // The data changed under every bound item.
    auto active = std::move(active_);
    active_.clear();
    for (auto &[index, item] : active) {
        recycle(index, std::move(item));
    }
    extentsValid_ = false;
    markStructureChanged();
}

void VirtualList::notifyInserted(std::size_t index, std::size_t count) {
    if (count == 0) {
        return;
    }
    index = std::min(index, count_);
    std::unordered_map<std::size_t, Active> shifted;
    shifted.reserve(active_.size());
    for (auto &[i, item] : active_) {
        shifted.emplace(i >= index ? i + count : i, std::move(item));
    }
    active_ = std::move(shifted);
    count_ += count;
    if (measured() && columns_ == 1 && extentsValid_) {
        auto &values = extents_.values();
        values.insert(values.begin() + static_cast<std::ptrdiff_t>(index), count, estimatedExtent_ + mainSpacing_);
        rowMeasured_.insert(rowMeasured_.begin() + static_cast<std::ptrdiff_t>(index), count, false);
        extents_.rebuild();
    } else {
        extentsValid_ = false;
    }
    // The anchor item keeps its place, even when the insert is right at it.
    if (index <= anchorIndex_ && count_ > count) {
        anchorIndex_ += count;
    }
    markStructureChanged();
}

void VirtualList::notifyRemoved(std::size_t index, std::size_t count) {
    if (index >= count_ || count == 0) {
        return;
    }
    count = std::min(count, count_ - index);
    std::unordered_map<std::size_t, Active> shifted;
    shifted.reserve(active_.size());
    for (auto &[i, item] : active_) {
        if (i >= index && i < index + count) {
            recycle(i, std::move(item));
        } else {
            shifted.emplace(i >= index + count ? i - count : i, std::move(item));
        }
    }
    active_ = std::move(shifted);
    count_ -= count;
    if (measured() && columns_ == 1 && extentsValid_) {
        auto &values = extents_.values();
        values.erase(values.begin() + static_cast<std::ptrdiff_t>(index),
                     values.begin() + static_cast<std::ptrdiff_t>(index + count));
        rowMeasured_.erase(rowMeasured_.begin() + static_cast<std::ptrdiff_t>(index),
                           rowMeasured_.begin() + static_cast<std::ptrdiff_t>(index + count));
        extents_.rebuild();
    } else {
        extentsValid_ = false;
    }
    if (anchorIndex_ >= index + count) {
        anchorIndex_ -= count;
    } else if (anchorIndex_ >= index) {
        anchorIndex_ = index;
        anchorDelta_ = 0;
    }
    anchorIndex_ = count_ == 0 ? 0 : std::min(anchorIndex_, count_ - 1);
    markStructureChanged();
}

void VirtualList::notifyChanged(std::size_t index, std::size_t count) {
    const std::size_t end = std::min(count_, index + std::min(count, count_));
    if (index >= end) {
        return;
    }
    // Only bound items have anything to rebind. A change to everything (a folder count, the
    // selection) comes often while scrolling: visit the few bound items, not all 100k.
    if (end - index > active_.size()) {
        for (auto &[i, item] : active_) {
            if (i >= index && i < end) {
                item.stale = true;
            }
        }
    } else {
        for (std::size_t i = index; i < end; ++i) {
            if (auto it = active_.find(i); it != active_.end()) {
                it->second.stale = true;
            }
        }
    }
    if (measured() && extentsValid_) {
        const auto columns = static_cast<std::size_t>(columns_);
        const std::size_t firstRow = std::min(index / columns, rowMeasured_.size());
        const std::size_t lastRow = std::min((end - 1) / columns + 1, rowMeasured_.size());
        std::fill(rowMeasured_.begin() + static_cast<std::ptrdiff_t>(firstRow),
                  rowMeasured_.begin() + static_cast<std::ptrdiff_t>(lastRow), false);
    }
    anchorPending_ = true;
    markLayoutDirty();
}

void VirtualList::setFixedItemExtent(float extent) {
    fixedExtent_ = extent;
    extentsValid_ = false;
    markStructureChanged();
}

void VirtualList::setEstimatedItemExtent(float extent) {
    fixedExtent_.reset();
    estimatedExtent_ = extent;
    extentsValid_ = false;
    markStructureChanged();
}

void VirtualList::setColumns(int columns) {
    requestedColumns_ = std::max(1, columns);
    minColumnWidth_ = 0;
    markStructureChanged();
}

void VirtualList::setMinColumnWidth(float width) {
    minColumnWidth_ = width;
    markStructureChanged();
}

void VirtualList::setSpacing(float mainSpacing, float crossSpacing) {
    mainSpacing_ = mainSpacing;
    crossSpacing_ = crossSpacing;
    extentsValid_ = false;
    markStructureChanged();
}

Node *VirtualList::nodeFor(std::size_t index) const {
    auto it = active_.find(index);
    return it == active_.end() ? nullptr : it->second.node.get();
}

std::size_t VirtualList::rowCount() const {
    const auto columns = static_cast<std::size_t>(columns_);
    return (count_ + columns - 1) / columns;
}

float VirtualList::rowStride(std::size_t row) const {
    if (!measured()) {
        return *fixedExtent_ + mainSpacing_;
    }
    return row < extents_.size() ? extents_.get(row) : estimatedExtent_ + mainSpacing_;
}

double VirtualList::rowStart(std::size_t row) const {
    if (!measured()) {
        return static_cast<double>(row) * (*fixedExtent_ + mainSpacing_);
    }
    return extents_.prefix(row);
}

std::size_t VirtualList::rowAt(double offset) const {
    const std::size_t rows = rowCount();
    if (rows == 0 || offset <= 0) {
        return 0;
    }
    std::size_t row;
    if (!measured()) {
        const double stride = *fixedExtent_ + mainSpacing_;
        row = stride > 0 ? static_cast<std::size_t>(offset / stride) : 0;
    } else {
        row = extents_.find(offset);
    }
    return std::min(row, rows - 1);
}

float VirtualList::itemOffset(std::size_t index) const {
    return contentStart_ + static_cast<float>(rowStart(index / static_cast<std::size_t>(columns_)));
}

void VirtualList::scrollToItem(std::size_t index, bool animated) {
    if (animated) {
        scrollTo(itemOffset(index), true);
        return;
    }
    stopScrolling();
    anchorIndex_ = std::min(index, count_ == 0 ? 0 : count_ - 1);
    anchorDelta_ = 0;
    markStructureChanged();
}

void VirtualList::resetExtents() {
    if (measured()) {
        extents_.reset(rowCount(), estimatedExtent_ + mainSpacing_);
        rowMeasured_.assign(rowCount(), false);
    }
    extentsValid_ = true;
}

Node &VirtualList::obtain(std::size_t index) {
    if (auto it = active_.find(index); it != active_.end()) {
        if (it->second.stale) {
            it->second.stale = false;
            batch([&] { bind_(index, *it->second.node); });
            flushEffects();
            ++stats_.binds;
        }
        return *it->second.node;
    }
    const std::uint32_t type = type_ ? type_(index) : 0;
    NodePtr node;
    if (auto &pool = pool_[type]; !pool.empty()) {
        node = std::move(pool.back());
        pool.pop_back();
    } else {
        node = create_(type);
        ++stats_.creates;
    }
    // Bind while detached, so the binding's dirty marks stop at the item; then attach.
    batch([&] { bind_(index, *node); });
    flushEffects();
    ++stats_.binds;
    appendChild(node);
    Node &ref = *node;
    active_.emplace(index, Active{std::move(node), type, false});
    return ref;
}

void VirtualList::recycle(std::size_t, Active active) {
    removeChild(active.node.get());
    ++stats_.recycles;
    if (auto &pool = pool_[active.type]; pool.size() < kMaxPooled) {
        pool.push_back(std::move(active.node));
    }
}

gfx::Size VirtualList::measureItem(LayoutEngine &engine, Node &node, float columnExtent) {
    if (!measured()) {
        const float extent = *fixedExtent_;
        return vertical() ? engine.compute(node, columnExtent, extent, columnExtent, extent, true)
                          : engine.compute(node, extent, columnExtent, extent, columnExtent, true);
    }
    return vertical() ? engine.compute(node, columnExtent, std::nullopt, columnExtent, kInfinity, true)
                      : engine.compute(node, std::nullopt, columnExtent, kInfinity, columnExtent, true);
}

void VirtualList::layoutRow(LayoutEngine &engine, std::size_t row, float columnExtent) {
    const auto columns = static_cast<std::size_t>(columns_);
    float rowExtent = 0;
    for (std::size_t i = row * columns; i < std::min(count_, (row + 1) * columns); ++i) {
        const gfx::Size size = measureItem(engine, obtain(i), columnExtent);
        rowExtent = std::max(rowExtent, vertical() ? size.height : size.width);
    }
    if (measured() && row < extents_.size()) {
        extents_.set(row, rowExtent + mainSpacing_);
        rowMeasured_[row] = true;
    }
}

void VirtualList::layoutCustom(LayoutEngine &engine, gfx::Size size) {
    inLayout_ = true;
    ScrollNode::layoutCustom(engine, size);
    inLayout_ = false;
}

float VirtualList::layoutContent(LayoutEngine &engine, gfx::Size size) {
    const bool v = vertical();
    const Edges &pad = style().padding;
    const float viewport = v ? size.height : size.width;
    const float crossExtent = std::max(0.0f, v ? size.width - pad.horizontal() : size.height - pad.vertical());
    const float crossOrigin = v ? pad.left : pad.top;
    const float mainPadStart = v ? pad.top : pad.left;
    const float mainPadEnd = v ? pad.bottom : pad.right;

    int columns = requestedColumns_;
    if (minColumnWidth_ > 0) {
        columns = std::max(1, static_cast<int>(std::floor((crossExtent + crossSpacing_) /
                                                          (minColumnWidth_ + crossSpacing_))));
    }
    if (columns != columns_) {
        columns_ = columns;
        extentsValid_ = false;
    }
    if (!extentsValid_ || (measured() && extents_.size() != rowCount())) {
        resetExtents();
    }

    headerExtent_ = 0;
    if (header_ && header_->style().display) {
        const gfx::Size s = v ? engine.compute(*header_, crossExtent, std::nullopt, crossExtent, kInfinity, true)
                              : engine.compute(*header_, std::nullopt, crossExtent, kInfinity, crossExtent, true);
        LayoutEngine::place(*header_, v ? gfx::Rect{crossOrigin, mainPadStart, s.width, s.height}
                                        : gfx::Rect{mainPadStart, crossOrigin, s.width, s.height});
        headerExtent_ = v ? s.height : s.width;
    }
    contentStart_ = mainPadStart + headerExtent_;

    const std::size_t rows = rowCount();
    const auto contentExtent = [&] {
        const double rowsExtent = rows == 0 ? 0 : rowStart(rows) - mainSpacing_;
        return contentStart_ + static_cast<float>(rowsExtent) + mainPadEnd;
    };
    const auto clampOffset = [&](float offset) {
        return std::clamp(offset, 0.0f, std::max(0.0f, contentExtent() - viewport));
    };
    setContentExtent(contentExtent());

    if (rows == 0) {
        auto active = std::move(active_);
        active_.clear();
        for (auto &[index, item] : active) {
            recycle(index, std::move(item));
        }
        boundRange_ = {0, 0};
        anchorPending_ = false;
        setOffset(clampOffset(offset()));
        // Empty: whatever comes next starts at the top, padding and header included.
        anchorIndex_ = 0;
        anchorDelta_ = offset() - contentStart_;
        anchorKey_.reset();
        return contentExtent();
    }

    // Where the viewport starts, relative to row 0: from the anchor after data changes, else
    // from the scroll offset.
    std::size_t anchorRow;
    float anchorDelta;
    if (anchorPending_ && laidOut_) {
        anchorRow = std::min(anchorIndex_ / static_cast<std::size_t>(columns_), rows - 1);
        anchorDelta = anchorDelta_;
        anchorPending_ = false;
    } else {
        anchorPending_ = false;
        const double relative = static_cast<double>(offset()) - contentStart_;
        anchorRow = rowAt(relative);
        anchorDelta = static_cast<float>(relative - rowStart(anchorRow));
        // Scrolling a short way: anchor on a row that's already bound and measured, so rows
        // measured on the way in are added around it and what's on screen moves by exactly the
        // scroll delta. A long jump just uses the estimates.
        if (boundRange_.first < boundRange_.second && boundRange_.second <= count_) {
            const std::size_t firstBound = boundRange_.first / static_cast<std::size_t>(columns_);
            const double firstStart = rowStart(firstBound);
            if (relative < firstStart && relative >= firstStart - viewport) {
                anchorRow = firstBound;
                anchorDelta = static_cast<float>(relative - firstStart);
            }
        }
    }
    const float overscan = overscan_.value_or(viewport * 0.5f);
    const auto columnsCount = static_cast<std::size_t>(columns_);
    const float columnExtent = std::max(
            0.0f, (crossExtent - crossSpacing_ * static_cast<float>(columnsCount - 1)) / static_cast<float>(columnsCount));

    // Recycle what's clearly out of range first, so rows coming in reuse those nodes.
    {
        const double top = rowStart(anchorRow) + anchorDelta - overscan;
        const double bottom = rowStart(anchorRow) + anchorDelta + viewport + overscan;
        const std::size_t firstIndex = rowAt(top) * columnsCount;
        const std::size_t lastIndex = std::min(count_, (rowAt(bottom) + 1) * columnsCount);
        for (auto it = active_.begin(); it != active_.end();) {
            if (it->first < firstIndex || it->first >= lastIndex) {
                Active item = std::move(it->second);
                const std::size_t index = it->first;
                it = active_.erase(it);
                recycle(index, std::move(item));
            } else {
                ++it;
            }
        }
    }

    // Bind and measure forward from the anchor row, then backward. Rows measured above the
    // anchor move the anchor row's start, and with it the offset: the anchor stays put.
    std::size_t endRow = anchorRow;
    while (endRow < rows) {
        layoutRow(engine, endRow, columnExtent);
        const double rowEnd = rowStart(endRow) + rowStride(endRow);
        ++endRow;
        if (rowEnd >= rowStart(anchorRow) + anchorDelta + viewport + overscan) {
            break;
        }
    }
    std::size_t beginRow = anchorRow;
    while (beginRow > 0 && rowStart(beginRow) > rowStart(anchorRow) + anchorDelta - overscan) {
        --beginRow;
        layoutRow(engine, beginRow, columnExtent);
    }

    setContentExtent(contentExtent());
    setOffset(clampOffset(contentStart_ + static_cast<float>(rowStart(anchorRow)) + anchorDelta));

    // Place every bound row now that the extents are final.
    const std::size_t firstIndex = beginRow * columnsCount;
    const std::size_t lastIndex = std::min(count_, endRow * columnsCount);
    for (auto it = active_.begin(); it != active_.end();) {
        const std::size_t index = it->first;
        if (index < firstIndex || index >= lastIndex) {
            Active item = std::move(it->second);
            it = active_.erase(it);
            recycle(index, std::move(item));
            continue;
        }
        Node &node = *it->second.node;
        const std::size_t row = index / columnsCount;
        const std::size_t column = index % columnsCount;
        const float main = contentStart_ + static_cast<float>(rowStart(row));
        const float cross = crossOrigin + static_cast<float>(column) * (columnExtent + crossSpacing_);
        const gfx::Size frame = measureItem(engine, node, columnExtent);
        LayoutEngine::place(node, v ? gfx::Rect{cross, main, frame.width, frame.height}
                                    : gfx::Rect{main, cross, frame.width, frame.height});
        ++it;
    }
    boundRange_ = {firstIndex, lastIndex};

    // Remember the anchor for the next data change.
    const double relative = static_cast<double>(offset()) - contentStart_;
    const std::size_t row = rowAt(relative);
    anchorIndex_ = row * columnsCount;
    anchorDelta_ = static_cast<float>(relative - rowStart(row));
    if (key_) {
        anchorKey_ = key_(anchorIndex_);
    }
    laidOut_ = true;
    return contentExtent();
}

void VirtualList::onOffsetChanged(float previous) {
    if (inLayout_) {
        return;
    }
    if (anchorPending_) {
        // A data change is waiting for layout to put the anchor item back in place; this scroll
        // (a fling's frame, a drag) moves it from there rather than being undone by it.
        anchorDelta_ += offset() - previous;
    } else {
        // A scroll between layouts (scrollTo, a drag) moves the anchor too, so a data change
        // before the next layout keeps the new position rather than snapping back.
        updateAnchorFromOffset();
    }
    markLayoutDirty();
}

void VirtualList::updateAnchorFromOffset() {
    if (!laidOut_ || anchorPending_ || !extentsValid_ || count_ == 0) {
        return;
    }
    const double relative = static_cast<double>(offset()) - contentStart_;
    const std::size_t row = rowAt(relative);
    anchorIndex_ = std::min(row * static_cast<std::size_t>(columns_), count_ - 1);
    anchorDelta_ = static_cast<float>(relative - rowStart(row));
    if (key_) {
        anchorKey_ = key_(anchorIndex_);
    }
}

}  // namespace filepan::ui
