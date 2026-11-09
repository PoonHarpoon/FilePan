// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "filepan/ui/node.hpp"
#include "filepan/ui/reactive.hpp"

// Keyed reconciliation for dynamic children, like Solid's <For>.
//
// ForEach owns all children of one container. Each item gets a node built once by build() in
// its own scope, with the item and its index passed as signals. When the list changes, items
// are matched by key: a kept item keeps its node and just gets its signals updated, a new one
// is built, and a dropped one has its scope disposed and its node removed. Nothing is diffed
// beyond the keys, and nodes are never rebuilt for a reorder.
//
// For lists longer than a screen, use VirtualList instead.
namespace filepan::ui {

template <typename T, typename K>
class ForEach {
public:
    using KeyFn = std::function<K(const T &)>;
    using BuildFn = std::function<NodePtr(Signal<T> item, Signal<std::size_t> index)>;

    struct Stats {
        std::size_t inserted = 0;
        std::size_t removed = 0;
        std::size_t kept = 0;
        // Kept items that changed relative order (outside the longest increasing run).
        std::size_t moved = 0;
    };

    ForEach(Node &container, KeyFn key, BuildFn build)
        : container_(container), key_(std::move(key)), build_(std::move(build)) {}

    void update(const std::vector<T> &items) {
        Stats stats;
        std::unordered_map<K, std::size_t> oldIndex;
        oldIndex.reserve(rows_.size());
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            oldIndex.emplace(rows_[i].key, i);
        }
        std::vector<bool> used(rows_.size(), false);
        std::vector<std::size_t> sources;  // Old positions of kept rows, in new order.
        std::vector<Row> next;
        next.reserve(items.size());
        batch([&] {
            for (std::size_t j = 0; j < items.size(); ++j) {
                const T &item = items[j];
                K key = key_(item);
                auto found = oldIndex.find(key);
                if (found != oldIndex.end() && !used[found->second]) {
                    used[found->second] = true;
                    Row row = std::move(rows_[found->second]);
                    row.item.set(item);
                    row.index.set(j);
                    sources.push_back(found->second);
                    next.push_back(std::move(row));
                    ++stats.kept;
                } else {
                    Row row{std::move(key), nullptr, Signal<T>(item), Signal<std::size_t>(j),
                            std::make_unique<Scope>()};
                    row.node = row.scope->run([&] { return build_(row.item, row.index); });
                    next.push_back(std::move(row));
                    ++stats.inserted;
                }
            }
        });
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            if (!used[i]) {
                ++stats.removed;
                rows_[i].scope->dispose();
            }
        }
        stats.moved = stats.kept - longestIncreasing(sources);
        std::vector<NodePtr> children;
        children.reserve(next.size());
        for (const auto &row : next) {
            children.push_back(row.node);
        }
        rows_ = std::move(next);
        container_.setChildren(std::move(children));
        stats_ = stats;
    }

    [[nodiscard]] const Stats &lastStats() const { return stats_; }
    [[nodiscard]] std::size_t size() const { return rows_.size(); }

private:
    struct Row {
        K key;
        NodePtr node;
        Signal<T> item;
        Signal<std::size_t> index;
        std::unique_ptr<Scope> scope;
    };

    static std::size_t longestIncreasing(const std::vector<std::size_t> &values) {
        std::vector<std::size_t> tails;
        for (std::size_t value : values) {
            auto it = std::ranges::lower_bound(tails, value);
            if (it == tails.end()) {
                tails.push_back(value);
            } else {
                *it = value;
            }
        }
        return tails.size();
    }

    Node &container_;
    KeyFn key_;
    BuildFn build_;
    std::vector<Row> rows_;
    Stats stats_;
};

// Binds container's children to items(), a reactive list: whenever what it reads changes, the
// children are reconciled by key. The returned ForEach lives as long as the container.
template <typename T, typename K, typename ItemsFn>
std::shared_ptr<ForEach<T, K>> forEach(Node &container, ItemsFn items, typename ForEach<T, K>::KeyFn key,
                                       typename ForEach<T, K>::BuildFn build) {
    auto reconciler = std::make_shared<ForEach<T, K>>(container, std::move(key), std::move(build));
    container.bind([reconciler, items = std::move(items)] {
        const std::vector<T> &list = items();
        untrack([&] { reconciler->update(list); });
    });
    return reconciler;
}

}  // namespace filepan::ui
