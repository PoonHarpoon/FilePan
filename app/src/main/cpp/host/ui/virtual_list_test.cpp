// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <algorithm>
#include <string>
#include <vector>

#include "filepan/ui/virtual_list.hpp"
#include "ui_harness.hpp"

using namespace filepan;
using namespace filepan::ui;
using filepan::test::UiHarness;

namespace {

// A row as a component would build it: a box with a text bound to a per-row signal.
struct Row {
    std::shared_ptr<Box> node;
    Signal<std::string> name;
};

class Fixture {
public:
    explicit Fixture(UiHarness &h, std::size_t count) : harness(h) {
        for (std::size_t i = 0; i < count; ++i) {
            data.push_back(static_cast<std::uint64_t>(i));
        }
        list->setFixedItemExtent(50);
        list->setCreate([this](std::uint32_t) -> NodePtr {
            auto row = std::make_shared<Row>();
            row->node = Box::create({.direction = Direction::row});
            auto label = Text::create();
            label->setMaxLines(1);
            label->bind([raw = label.get(), name = row->name] { raw->setText(name()); });
            row->node->appendChild(label);
            rows[row->node.get()] = row;
            return row->node;
        });
        list->setBind([this](std::size_t index, Node &node) {
            ++binds;
            rows[&node]->name.set("item " + std::to_string(data[index]));
        });
        list->setKeys([this](std::size_t index) { return data[index]; },
                      [this](std::uint64_t key) -> std::optional<std::size_t> {
                          auto it = std::ranges::find(data, key);
                          if (it == data.end()) {
                              return std::nullopt;
                          }
                          return static_cast<std::size_t>(it - data.begin());
                      });
        list->setItemCount(count);
        list->setOverscan(100);
        h.root->setContent(list);
        h.frame();
    }

    [[nodiscard]] std::string textAt(std::size_t index) const {
        Node *node = list->nodeFor(index);
        return node == nullptr ? "" : static_cast<const Text &>(*node->children()[0]).text();
    }
    // The item's top on screen, in dp.
    [[nodiscard]] float screenTop(std::size_t index) const {
        return list->nodeFor(index)->frame().y - list->offset();
    }
    // Index of the item whose text says key.
    [[nodiscard]] std::optional<std::size_t> visibleWithKey(std::uint64_t key) const {
        const auto [first, last] = list->boundRange();
        for (std::size_t i = first; i < last; ++i) {
            if (textAt(i) == "item " + std::to_string(key)) {
                return i;
            }
        }
        return std::nullopt;
    }

    UiHarness &harness;
    std::shared_ptr<VirtualList> list = VirtualList::create();
    std::vector<std::uint64_t> data;
    std::unordered_map<Node *, std::shared_ptr<Row>> rows;
    int binds = 0;
};

void testVisibleRangeAndRecycling() {
    UiHarness h;
    Fixture f(h, 100'000);
    // 800dp viewport, 50dp rows, 100dp overscan below: rows 0..18.
    EXPECT(f.list->boundRange().first == 0 && f.list->boundRange().second == 18);
    EXPECT(f.textAt(3) == "item 3");
    EXPECT(test::near(f.list->contentExtent(), 5'000'000));
    const auto creates = f.list->stats().creates;

    // Scroll far: rows are recycled, not created.
    f.list->scrollTo(25'000);  // Row 500 at the top.
    h.frame();
    const auto [first, last] = f.list->boundRange();
    EXPECT(first == 498 && last == 518);
    EXPECT(f.textAt(500) == "item 500" && test::near(f.screenTop(500), 0));
    EXPECT(f.list->stats().creates <= creates + 2);
    EXPECT(f.list->children().size() == last - first);

    // Small scroll: only the rows entering are bound.
    const int binds = f.binds;
    f.list->scrollTo(25'100);
    h.frame();
    EXPECT(f.binds - binds == 2);
}

void testAnchoringOnInsertAndRemove() {
    UiHarness h;
    Fixture f(h, 1000);
    f.list->scrollTo(5'025);  // Item 100, 25dp in.
    h.frame();
    EXPECT(f.textAt(100) == "item 100" && test::near(f.screenTop(100), -25));

    // Ten items inserted above: item 100 is now index 110, still at the same spot.
    std::vector<std::uint64_t> fresh;
    for (std::uint64_t i = 0; i < 10; ++i) {
        fresh.push_back(10'000 + i);
    }
    f.data.insert(f.data.begin() + 5, fresh.begin(), fresh.end());
    f.list->notifyInserted(5, 10);
    h.frame();
    EXPECT(f.textAt(110) == "item 100" && test::near(f.screenTop(110), -25));

    // Removed above: it moves back up the index, not on screen.
    f.data.erase(f.data.begin(), f.data.begin() + 20);
    f.list->notifyRemoved(0, 20);
    h.frame();
    EXPECT(f.textAt(90) == "item 100" && test::near(f.screenTop(90), -25));

    // A whole new data set: the anchor follows its key.
    std::ranges::reverse(f.data);
    f.list->notifyReset();
    h.frame();
    const auto index = f.visibleWithKey(100);
    EXPECT(index && test::near(f.screenTop(*index), -25));
}

// A scroll followed by a data change before the next layout keeps the new position.
void testScrollThenResetBeforeLayout() {
    UiHarness h;
    Fixture f(h, 1000);
    f.list->scrollTo(10'010);  // Item 200, 10dp in.
    f.list->notifyReset();
    h.frame();
    EXPECT(f.textAt(200) == "item 200" && test::near(f.screenTop(200), -10));
    EXPECT(test::near(f.list->offset(), 10'010));
}

// Emptied and refilled with other items, a list starts at the top.
void testEmptiedListStartsAtTop() {
    UiHarness h;
    Fixture f(h, 1000);
    f.list->setStyle({.padding = {0, 30, 0, 0}});
    f.list->scrollTo(5'000);
    h.frame();
    f.data.clear();
    f.list->setItemCount(0);
    h.frame();
    for (std::uint64_t i = 0; i < 500; ++i) {
        f.data.push_back(50'000 + i);
    }
    f.list->setItemCount(f.data.size());
    h.frame();
    EXPECT(test::near(f.list->offset(), 0) && f.textAt(0) == "item 50000");
}

void testMeasuredRowsKeepTheAnchor() {
    UiHarness h;
    // Variable-height rows: every third one wraps to more lines.
    auto list = VirtualList::create();
    list->setEstimatedItemExtent(40);
    list->setCreate([](std::uint32_t) -> NodePtr {
        auto text = Text::create({}, {.size = 10});
        return text;
    });
    list->setBind([](std::size_t index, Node &node) {
        // 10sp: 5dp per char; the 400dp width fits 80 chars a line.
        static_cast<Text &>(node).setText(std::string(index % 3 == 0 ? 200 : 10, 'x'));
    });
    list->setItemCount(10'000);
    h.root->setContent(list);
    h.frame();
    EXPECT(list->nodeFor(0) && test::near(list->nodeFor(0)->frame().height, 36));  // 3 lines.
    EXPECT(test::near(list->nodeFor(1)->frame().height, 12));

    list->scrollToItem(5000);
    h.frame();
    const float top = list->nodeFor(5000)->frame().y - list->offset();
    EXPECT(test::near(top, 0));
    // Scroll up into unmeasured rows: their real heights replace the estimate, but the
    // rows on screen don't jump.
    const float before = list->nodeFor(5003)->frame().y - list->offset();
    list->scrollTo(list->offset() - 300);
    h.frame();
    const float after = list->nodeFor(5003)->frame().y - list->offset();
    EXPECT(test::near(after - before, 300, 0.5f));
}

void testGridAndHeader() {
    UiHarness h;
    auto list = VirtualList::create();
    list->setMinColumnWidth(104);
    list->setSpacing(8, 8);
    list->setFixedItemExtent(120);
    auto header = Box::create({.height = 100});
    list->setHeader(header);
    list->setCreate([](std::uint32_t) -> NodePtr { return Box::create(); });
    list->setBind([](std::size_t, Node &) {});
    list->setItemCount(1000);
    h.root->setContent(list);
    h.frame();
    // (400 + 8) / (104 + 8) = 3 columns of (400 - 16) / 3 = 128dp.
    EXPECT(list->columns() == 3);
    EXPECT(header->frame().y == 0 && test::near(header->frame().height, 100));
    const gfx::Rect item4 = list->nodeFor(4)->frame();
    EXPECT(test::near(item4.x, 136) && test::near(item4.y, 228) && test::near(item4.width, 128));
    // 334 rows of 128dp, minus the last spacing, plus the header.
    EXPECT(test::near(list->contentExtent(), 100 + 334 * 128 - 8));
    // Items set before the first layout start at the top, header in view.
    EXPECT(list->offset() == 0);
}

void testPaddingBeforeFirstLayout() {
    UiHarness h;
    auto list = VirtualList::create();
    list->setEstimatedItemExtent(56);
    list->setCreate([](std::uint32_t) -> NodePtr { return Box::create({.height = 56}); });
    list->setBind([](std::size_t, Node &) {});
    list->setItemCount(100);
    // A top bar floats over the list: its padding keeps row 0 below it.
    list->setStyle({.padding = {0, 130, 0, 0}});
    h.root->setContent(list);
    h.frame();
    EXPECT(list->offset() == 0);
    EXPECT(test::near(list->nodeFor(0)->frame().y, 130));
    // A later reset keeps the list where it is, padding included.
    list->setItemCount(100);
    h.frame();
    EXPECT(list->offset() == 0);
}

void testCollapsingHeaderPreScroll() {
    UiHarness h;
    // An app bar that collapses from 120 to 56dp before the list scrolls.
    Signal<float> barHeight(120.0f);
    auto screen = Box::create();
    auto bar = Box::create();
    bar->bind([raw = bar.get(), barHeight] {
        raw->updateStyle([&](Style &s) { s.height = barHeight(); });
    });
    auto list = VirtualList::create();
    list->setStyle({.grow = 1});
    list->setFixedItemExtent(50);
    list->setCreate([](std::uint32_t) -> NodePtr { return Box::create(); });
    list->setBind([](std::size_t, Node &) {});
    list->setItemCount(500);
    // Collapses before the list scrolls down; expands once the list is back at its top.
    list->setPreScroll([barHeight](float delta) {
        const float height = barHeight.peek();
        const float next = delta > 0 ? std::max(56.0f, height - delta) : height;
        barHeight.set(next);
        return height - next;
    });
    list->setPostScroll([barHeight](float delta) {
        const float height = barHeight.peek();
        const float next = delta < 0 ? std::min(120.0f, height - delta) : height;
        barHeight.set(next);
        return height - next;
    });
    screen->appendChild(bar);
    screen->appendChild(list);
    h.root->setContent(screen);
    h.frame();
    EXPECT(test::near(list->frame().y, 120));

    h.down(200, 600);
    h.move(200, 560);
    h.move(200, 500);
    h.frame();
    // 100dp dragged minus the 8dp slop: 64dp collapse the bar, 28dp scroll the list.
    EXPECT(test::near(barHeight.peek(), 56) && test::near(list->offset(), 28));
    EXPECT(test::near(list->frame().y, 56));
    h.move(200, 600);
    h.frame();
    EXPECT(list->offset() == 0 && test::near(barHeight.peek(), 120));
    h.up(200, 600);
}

// Rows changing under a moving list (thumbnails and folder counts arriving) don't hold it back:
// each frame's scroll still lands, whether it came from a fling or the finger.
void testChangesDuringScrollKeepTheMotion() {
    const auto run = [](bool changes) {
        UiHarness h;
        Fixture f(h, 100'000);
        f.list->fling(4000);
        for (int frame = 0; frame < 60; ++frame) {
            if (changes) {
                // Between frames, as a decoded thumbnail's callback does.
                f.list->notifyChanged(f.list->boundRange().first + 3);
            }
            h.advance(8);
        }
        return f.list->offset();
    };
    const float clean = run(false);
    EXPECT(clean > 1000);
    EXPECT(test::near(run(true), clean, 1));

    // Dragging, with a change pending at every move.
    UiHarness h;
    Fixture f(h, 100'000);
    h.down(200, 700);
    for (int i = 1; i <= 10; ++i) {
        f.list->notifyChanged(5);
        h.move(200, 700 - 40.0f * static_cast<float>(i));
        h.advance(8);
    }
    EXPECT(test::near(f.list->offset(), 400 - 8));
    h.up(200, 300);
}

}  // namespace

int main() {
    testVisibleRangeAndRecycling();
    testAnchoringOnInsertAndRemove();
    testScrollThenResetBeforeLayout();
    testEmptiedListStartsAtTop();
    testMeasuredRowsKeepTheAnchor();
    testGridAndHeader();
    testPaddingBeforeFirstLayout();
    testCollapsingHeaderPreScroll();
    testChangesDuringScrollKeepTheMotion();
    return 0;
}
