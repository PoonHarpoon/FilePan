// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <string>
#include <vector>

#include "filepan/ui/for_each.hpp"
#include "filepan/ui/node.hpp"
#include "ui_test_util.hpp"

using namespace filepan;
using namespace filepan::ui;

namespace {

struct Item {
    int id = 0;
    std::string name;

    bool operator==(const Item &) const = default;
};

std::vector<std::string> texts(const Node &container) {
    std::vector<std::string> out;
    for (const auto &child : container.children()) {
        out.push_back(static_cast<const Text &>(*child).text());
    }
    return out;
}

void testKeyedReconciliation() {
    auto container = Box::create();
    Signal<std::vector<Item>> items(std::vector<Item>{{1, "a"}, {2, "b"}, {3, "c"}});
    int builds = 0;
    int cleanups = 0;
    std::vector<Node *> firstNodes;
    std::weak_ptr<Node> removedNode;
    auto reconciler = forEach<Item, int>(
            *container, [items] { return items(); }, [](const Item &item) { return item.id; },
            [&](Signal<Item> item, Signal<std::size_t>) -> NodePtr {
                ++builds;
                auto text = Text::create();
                text->bind([raw = text.get(), item] { raw->setText(item().name); });
                onCleanup([&] { ++cleanups; });
                return text;
            });
    EXPECT(builds == 3);
    EXPECT(texts(*container) == (std::vector<std::string>{"a", "b", "c"}));
    for (const auto &child : container->children()) {
        firstNodes.push_back(child.get());
        if (static_cast<const Text &>(*child).text() == "b") {
            removedNode = child;
        }
    }

    // Reorder: no rebuilds, nodes move.
    items.set({{3, "c"}, {1, "a"}, {2, "b"}});
    EXPECT(builds == 3);
    EXPECT(reconciler->lastStats().kept == 3 && reconciler->lastStats().moved == 1);
    EXPECT(container->children()[0].get() == firstNodes[2]);
    EXPECT(container->children()[1].get() == firstNodes[0]);

    // Insert one and rename one: one build, the renamed node updates in place.
    items.set({{3, "c"}, {4, "d"}, {1, "A"}, {2, "b"}});
    EXPECT(builds == 4);
    EXPECT(reconciler->lastStats().inserted == 1 && reconciler->lastStats().moved == 0);
    EXPECT(texts(*container) == (std::vector<std::string>{"c", "d", "A", "b"}));
    EXPECT(container->children()[2].get() == firstNodes[0]);

    // Remove: the dropped row's scope is disposed.
    items.set({{3, "c"}, {1, "A"}});
    EXPECT(reconciler->lastStats().removed == 2);
    EXPECT(cleanups == 2);
    EXPECT(texts(*container) == (std::vector<std::string>{"c", "A"}));
    EXPECT(removedNode.expired());  // Nothing keeps a dropped row alive.

    // Duplicate keys: the second occurrence gets its own row.
    items.set({{3, "c"}, {3, "c2"}});
    EXPECT(texts(*container) == (std::vector<std::string>{"c", "c2"}));
}

}  // namespace

int main() {
    testKeyedReconciliation();
    return 0;
}
