// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "filepan/ui/scroll.hpp"

// A virtualized list or grid for very long data (100k files).
//
// Only the items in the viewport plus an overscan margin have nodes. When an item scrolls out,
// its node goes to a pool for its item type; an item scrolling in takes a pooled node of its
// type (or creates one) and the bind callback points it at the new index. Item nodes are built
// once by create() with signals inside; bind() sets those signals, so rebinding re-shapes only
// the text that actually changed.
//
// Rows are either a fixed extent (no measuring, pure arithmetic) or measured: unmeasured rows
// use an estimate and a Fenwick tree keeps offsets O(log n) as real extents arrive.
//
// Scroll anchoring: the scroll position is kept as "item i, d dp into it". When items are
// inserted or removed above (notifyInserted/notifyRemoved), when a reset maps the anchor's key
// to its new index (setKeys + notifyReset), or when rows above get measured, the anchor item
// stays put on screen.
//
// A header node (setHeader) scrolls with the content before the first row; for a collapsing app
// bar outside the list, use ScrollNode::setPreScroll and bind to offsetSignal().
namespace filepan::ui {

class VirtualList : public ScrollNode {
public:
    using TypeFn = std::function<std::uint32_t(std::size_t index)>;
    using CreateFn = std::function<NodePtr(std::uint32_t type)>;
    using BindFn = std::function<void(std::size_t index, Node &node)>;
    using KeyFn = std::function<std::uint64_t(std::size_t index)>;
    using IndexOfKeyFn = std::function<std::optional<std::size_t>(std::uint64_t key)>;

    static std::shared_ptr<VirtualList> create(ScrollAxis axis = ScrollAxis::vertical);
    explicit VirtualList(ScrollAxis axis = ScrollAxis::vertical);

    // --- Content ---
    void setCreate(CreateFn create) { create_ = std::move(create); }
    void setBind(BindFn bind) { bind_ = std::move(bind); }
    // Optional; all items are type 0 without it.
    void setItemType(TypeFn type) { type_ = std::move(type); }
    // Optional; lets notifyReset keep the anchor item in place by key.
    void setKeys(KeyFn key, IndexOfKeyFn indexOfKey);
    void setHeader(NodePtr header);

    // A new data set: every visible item is rebound, and the anchor follows its key if keys
    // are set.
    void setItemCount(std::size_t count);
    [[nodiscard]] std::size_t itemCount() const { return count_; }
    void notifyInserted(std::size_t index, std::size_t count);
    void notifyRemoved(std::size_t index, std::size_t count);
    // Rebinds these items if they're visible; in measured mode they're re-measured too.
    void notifyChanged(std::size_t index, std::size_t count = 1);
    void notifyReset() { setItemCount(count_); }

    // --- Geometry ---
    // Every row is exactly this many dp along the axis.
    void setFixedItemExtent(float extent);
    // Rows are measured; this is the guess for rows not measured yet.
    void setEstimatedItemExtent(float extent);
    // A grid: a fixed number of columns, or as many as fit at minColumnWidth.
    void setColumns(int columns);
    void setMinColumnWidth(float width);
    void setSpacing(float mainSpacing, float crossSpacing);
    // Extra dp rendered beyond each viewport edge; defaults to half a viewport.
    void setOverscan(std::optional<float> overscan) { overscan_ = overscan; }

    // --- Queries ---
    // Items that currently have nodes: [first, last).
    [[nodiscard]] std::pair<std::size_t, std::size_t> boundRange() const { return boundRange_; }
    [[nodiscard]] Node *nodeFor(std::size_t index) const;
    // The item's top (or left) in content dp, header included.
    [[nodiscard]] float itemOffset(std::size_t index) const;
    void scrollToItem(std::size_t index, bool animated = false);
    [[nodiscard]] std::size_t anchorIndex() const { return anchorIndex_; }
    [[nodiscard]] int columns() const { return columns_; }

    void layoutCustom(LayoutEngine &engine, gfx::Size size) override;

    struct Stats {
        std::uint64_t creates = 0;
        std::uint64_t binds = 0;
        std::uint64_t recycles = 0;
    };
    [[nodiscard]] const Stats &stats() const { return stats_; }

protected:
    float layoutContent(LayoutEngine &engine, gfx::Size size) override;
    void onOffsetChanged(float previous) override;

private:
    // Row offsets: prefix sums of (extent + spacing), O(log n) update and search.
    class ExtentIndex {
    public:
        void reset(std::size_t count, float value);
        void set(std::size_t index, float value);
        [[nodiscard]] float get(std::size_t index) const { return values_[index]; }
        [[nodiscard]] double prefix(std::size_t count) const;
        // The largest row whose start is <= offset.
        [[nodiscard]] std::size_t find(double offset) const;
        [[nodiscard]] std::size_t size() const { return values_.size(); }
        [[nodiscard]] std::vector<float> &values() { return values_; }
        void rebuild();

    private:
        std::vector<float> values_;
        std::vector<double> tree_;
    };

    struct Active {
        NodePtr node;
        std::uint32_t type = 0;
        bool stale = false;  // Needs rebinding.
    };

    [[nodiscard]] std::size_t rowCount() const;
    [[nodiscard]] double rowStart(std::size_t row) const;  // Relative to the first row.
    [[nodiscard]] float rowStride(std::size_t row) const;
    [[nodiscard]] std::size_t rowAt(double offset) const;  // Relative to the first row.
    [[nodiscard]] bool measured() const { return !fixedExtent_.has_value(); }
    void resetExtents();
    Node &obtain(std::size_t index);
    void recycle(std::size_t index, Active active);
    void layoutRow(LayoutEngine &engine, std::size_t row, float columnExtent);
    gfx::Size measureItem(LayoutEngine &engine, Node &node, float columnExtent);
    void updateAnchorFromOffset();
    void markStructureChanged();

    CreateFn create_;
    BindFn bind_;
    TypeFn type_;
    KeyFn key_;
    IndexOfKeyFn indexOfKey_;
    NodePtr header_;
    float headerExtent_ = 0;

    std::size_t count_ = 0;
    std::optional<float> fixedExtent_ = 56.0f;
    float estimatedExtent_ = 56;
    int requestedColumns_ = 1;
    float minColumnWidth_ = 0;
    int columns_ = 1;
    float mainSpacing_ = 0;
    float crossSpacing_ = 0;
    std::optional<float> overscan_;

    ExtentIndex extents_;
    std::vector<bool> rowMeasured_;
    bool extentsValid_ = false;
    float contentStart_ = 0;  // Padding + header, before row 0.

    std::unordered_map<std::size_t, Active> active_;
    std::unordered_map<std::uint32_t, std::vector<NodePtr>> pool_;
    std::pair<std::size_t, std::size_t> boundRange_{0, 0};

    std::size_t anchorIndex_ = 0;
    float anchorDelta_ = 0;
    std::optional<std::uint64_t> anchorKey_;
    bool anchorPending_ = false;
    bool inLayout_ = false;
    // Before the first layout there's no anchor to keep: the list starts at offset 0, padding
    // and header included.
    bool laidOut_ = false;
    Stats stats_;
};

}  // namespace filepan::ui
