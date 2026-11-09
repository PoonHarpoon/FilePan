// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/layout.hpp"
#include "filepan/ui/node.hpp"
#include "filepan/ui/scroll.hpp"
#include "ui_test_util.hpp"

using namespace filepan;
using namespace filepan::ui;
using filepan::test::near;

namespace {

test::FakeTextSystem text;

std::shared_ptr<Box> box(Style style = {}) { return Box::create(style); }

void layout(Node &root, gfx::Size size, LayoutEngine *out = nullptr) {
    LayoutEngine engine(LayoutContext{&text, 1, 1});
    engine.layoutRoot(root, size);
    if (out != nullptr) {
        *out = engine;
    }
}

bool frameIs(const Node &node, float x, float y, float w, float h) {
    const gfx::Rect f = node.frame();
    return near(f.x, x) && near(f.y, y) && near(f.width, w) && near(f.height, h);
}

void testColumnPaddingGap() {
    auto root = box({.gap = 8, .padding = Edges::all(16)});
    auto a = box({.height = 40});
    auto b = box({.margin = {.top = 4}, .height = 60});
    root->appendChild(a);
    root->appendChild(b);
    layout(*root, {300, 500});
    EXPECT(frameIs(*a, 16, 16, 268, 40));  // Stretched across the content box.
    EXPECT(frameIs(*b, 16, 68, 268, 60));
}

void testGrowAndShrink() {
    auto row = box({.direction = Direction::row});
    auto a = box({.grow = 1, .basis = 0});
    auto b = box({.grow = 2, .basis = 0});
    row->appendChild(a);
    row->appendChild(b);
    layout(*row, {300, 50});
    EXPECT(frameIs(*a, 0, 0, 100, 50));
    EXPECT(frameIs(*b, 100, 0, 200, 50));

    auto narrow = box({.direction = Direction::row});
    auto c = box({.shrink = 1, .width = 80});
    auto d = box({.shrink = 1, .width = 80});
    auto fixed = box({.width = 20});  // shrink 0: keeps its size.
    narrow->appendChild(c);
    narrow->appendChild(d);
    narrow->appendChild(fixed);
    layout(*narrow, {120, 10});
    EXPECT(near(c->frame().width, 50) && near(d->frame().width, 50));
    EXPECT(frameIs(*fixed, 100, 0, 20, 10));
}

void testMaxConstraintRedistributes() {
    auto row = box({.direction = Direction::row});
    auto a = box({.grow = 1, .basis = 0, .maxWidth = 50});
    auto b = box({.grow = 1, .basis = 0});
    row->appendChild(a);
    row->appendChild(b);
    layout(*row, {300, 10});
    EXPECT(near(a->frame().width, 50));
    EXPECT(near(b->frame().width, 250));
}

void testJustifyAndAlign() {
    auto row = box({.direction = Direction::row, .justify = Justify::spaceBetween, .alignItems = Align::center});
    auto a = box({.width = 20, .height = 10});
    auto b = box({.width = 20, .height = 30});
    auto c = box({.alignSelf = Align::end, .width = 20, .height = 10});
    row->appendChild(a);
    row->appendChild(b);
    row->appendChild(c);
    layout(*row, {100, 40});
    EXPECT(frameIs(*a, 0, 15, 20, 10));
    EXPECT(frameIs(*b, 40, 5, 20, 30));
    EXPECT(frameIs(*c, 80, 30, 20, 10));

    auto centered = box({.justify = Justify::center});
    auto d = box({.height = 20});
    centered->appendChild(d);
    layout(*centered, {50, 100});
    EXPECT(frameIs(*d, 0, 40, 50, 20));
}

void testTextInRowEllipsizes() {
    // An icon and a file name: the name takes the rest and, too long, is cut to fit.
    auto row = box({.direction = Direction::row, .alignItems = Align::center, .padding = Edges::symmetric(16, 0)});
    auto icon = box({.margin = {.right = 16}, .width = 24, .height = 24});
    auto name = Text::create(std::string(100, 'x'), {.size = 10});
    name->setMaxLines(1);
    name->updateStyle([](Style &s) {
        s.grow = 1;
        s.shrink = 1;
    });
    row->appendChild(icon);
    row->appendChild(name);
    layout(*row, {200, 56});
    EXPECT(frameIs(*icon, 16, 16, 24, 24));
    EXPECT(near(name->frame().x, 56) && near(name->frame().width, 128));
    EXPECT(near(name->frame().height, 12));
    EXPECT(near(name->frame().y, 22));

    // A short name sizes to its content.
    auto row2 = box({.direction = Direction::row});
    auto shortName = Text::create("abcd", {.size = 10});
    row2->appendChild(shortName);
    layout(*row2, {200, 56});
    EXPECT(near(shortName->frame().width, 20));
}

void testWrappingText() {
    auto column = box({.alignItems = Align::start});
    auto paragraph = Text::create(std::string(30, 'x'), {.size = 10});  // 150 wide unwrapped.
    column->appendChild(paragraph);
    layout(*column, {100, 200});
    EXPECT(near(paragraph->frame().width, 100) && near(paragraph->frame().height, 24));
}

void testWrap() {
    auto row = box({.direction = Direction::row, .wrap = true, .gap = 10, .lineGap = 5});
    std::vector<std::shared_ptr<Box>> items;
    for (int i = 0; i < 5; ++i) {
        items.push_back(box({.width = 40, .height = 20}));
        row->appendChild(items.back());
    }
    row->updateStyle([](Style &s) { s.alignItems = Align::start; });
    layout(*row, {100, 200});
    EXPECT(frameIs(*items[0], 0, 0, 40, 20));
    EXPECT(frameIs(*items[1], 50, 0, 40, 20));
    EXPECT(frameIs(*items[2], 0, 25, 40, 20));
    EXPECT(frameIs(*items[4], 0, 50, 40, 20));
}

void testAbsoluteAndStack() {
    auto stack = box({.justify = Justify::center, .alignItems = Align::center});
    auto fill = box({.position = Position::absolute, .left = 0, .top = 0, .right = 0, .bottom = 0});
    auto badge = box({.width = 10, .height = 10, .position = Position::absolute, .top = 4, .right = 4});
    auto centered = box({.width = 20, .height = 20, .position = Position::absolute});
    stack->appendChild(fill);
    stack->appendChild(badge);
    stack->appendChild(centered);
    layout(*stack, {100, 60});
    EXPECT(frameIs(*fill, 0, 0, 100, 60));
    EXPECT(frameIs(*badge, 86, 4, 10, 10));
    EXPECT(frameIs(*centered, 40, 20, 20, 20));
}

void testAspectRatioAndPercent() {
    auto column = box({.alignItems = Align::start});
    auto thumb = box({.width = 100, .aspectRatio = 2});
    auto half = box({.width = Length::percent(50), .height = 10});
    column->appendChild(thumb);
    column->appendChild(half);
    layout(*column, {300, 300});
    EXPECT(frameIs(*thumb, 0, 0, 100, 50));
    EXPECT(frameIs(*half, 0, 50, 150, 10));

    // In a stretched row, the ratio gives the width from the height.
    auto row = box({.direction = Direction::row});
    auto square = box({.aspectRatio = 1});
    row->appendChild(square);
    layout(*row, {300, 40});
    EXPECT(frameIs(*square, 0, 0, 40, 40));
}

void testMinMaxOnContainer() {
    auto column = box({.alignItems = Align::start});
    auto child = box({.minHeight = 30, .maxWidth = 50});
    auto inner = box({.width = 80, .height = 10});
    child->appendChild(inner);
    column->appendChild(child);
    layout(*column, {300, 300});
    EXPECT(near(child->frame().width, 50) && near(child->frame().height, 30));
}

// A node measured under a few constraints, changed, then measured again: none of the sizes from
// before the change comes back, whichever slot of the cache it was in.
void testMeasureCacheForgetsOnChange() {
    auto container = box();
    auto leaf = box({.height = 40});
    container->appendChild(leaf);
    LayoutEngine engine(LayoutContext{&text, 1, 1});
    EXPECT(near(engine.compute(*container, 100, std::nullopt, 100, kInfinity, false).height, 40));
    EXPECT(near(engine.compute(*container, 100, std::nullopt, 100, 500, false).height, 40));
    leaf->updateStyle([](Style &s) { s.height = 80; });
    EXPECT(near(engine.compute(*container, 100, std::nullopt, 100, 700, false).height, 80));
    EXPECT(near(engine.compute(*container, 100, std::nullopt, 100, kInfinity, false).height, 80));
    EXPECT(near(engine.compute(*container, 100, std::nullopt, 100, 500, false).height, 80));
}

void testCacheSkipsCleanSubtrees() {
    auto root = box();
    std::vector<std::shared_ptr<Text>> labels;
    for (int i = 0; i < 20; ++i) {
        auto row = box({.direction = Direction::row, .height = 40});
        labels.push_back(Text::create("label", {.size = 10}));
        row->appendChild(labels.back());
        root->appendChild(row);
    }
    LayoutEngine first(LayoutContext{&text, 1, 1});
    first.layoutRoot(*root, {300, 1000});
    EXPECT(first.computeCount() > 40);

    LayoutEngine clean(LayoutContext{&text, 1, 1});
    clean.layoutRoot(*root, {300, 1000});
    EXPECT(clean.computeCount() == 0);

    labels[7]->setText("a longer label");
    LayoutEngine partial(LayoutContext{&text, 1, 1});
    partial.layoutRoot(*root, {300, 1000});
    // Only the path to the changed label: root, its row, the label (a few passes each).
    EXPECT(partial.computeCount() > 0 && partial.computeCount() <= 8);
    EXPECT(near(labels[7]->frame().width, 70));
}

void testDisplayNone() {
    auto column = box();
    auto hidden = box({.display = false, .height = 50});
    auto shown = box({.height = 20});
    column->appendChild(hidden);
    column->appendChild(shown);
    layout(*column, {100, 100});
    EXPECT(frameIs(*shown, 0, 0, 100, 20));
    hidden->updateStyle([](Style &s) { s.display = true; });
    EXPECT(column->layoutDirty());
    layout(*column, {100, 100});
    EXPECT(frameIs(*shown, 0, 50, 100, 20));
}

}  // namespace

// A sheet: a panel at the bottom with a capped height, its body a scroller that fits its content.
void testFittingScroll() {
    const auto sheet = [](float content) {
        auto root = box();
        auto panel = box({.maxHeight = 400, .position = Position::absolute, .left = 0, .right = 0, .bottom = 0});
        auto header = box({.height = 50});
        auto scroll = ScrollNode::create();
        scroll->setStyle({.shrink = 1});
        scroll->setFitContent(true);
        scroll->appendChild(box({.height = content}));
        panel->appendChild(header);
        panel->appendChild(scroll);
        root->appendChild(panel);
        layout(*root, {300, 600});
        return std::pair{panel->frame(), scroll};
    };
    // Short: as tall as its content.
    const auto [shortPanel, shortScroll] = sheet(120);
    EXPECT(near(shortPanel.height, 170) && near(shortPanel.y, 430) && near(shortScroll->frame().height, 120));
    // Long: the panel stops at its cap and the body scrolls.
    const auto [longPanel, longScroll] = sheet(900);
    EXPECT(near(longPanel.height, 400) && near(longScroll->frame().height, 350));
    EXPECT(near(longScroll->maxOffset(), 550));
    // It follows its content when that changes.
    {
        auto root = box();
        auto panel = box({.maxHeight = 400, .position = Position::absolute, .left = 0, .right = 0, .bottom = 0});
        auto scroll = ScrollNode::create();
        scroll->setStyle({.shrink = 1});
        scroll->setFitContent(true);
        auto column = box();
        column->appendChild(box({.height = 100}));
        auto extra = box({.display = false, .height = 80});
        column->appendChild(extra);
        scroll->appendChild(column);
        panel->appendChild(scroll);
        root->appendChild(panel);
        layout(*root, {300, 600});
        EXPECT(near(scroll->frame().height, 100));
        extra->updateStyle([](Style &s) { s.display = true; });
        layout(*root, {300, 600});
        EXPECT(near(scroll->frame().height, 180) && near(panel->frame().height, 180));
    }
    // Without fitting, the scroller takes all the room.
    auto plain = ScrollNode::create();
    plain->appendChild(box({.height = 20}));
    auto column = box();
    column->appendChild(plain);
    layout(*column, {300, 600});
    EXPECT(near(plain->frame().height, 600));
}

int main() {
    testFittingScroll();
    testColumnPaddingGap();
    testGrowAndShrink();
    testMaxConstraintRedistributes();
    testJustifyAndAlign();
    testTextInRowEllipsizes();
    testWrappingText();
    testWrap();
    testAbsoluteAndStack();
    testAspectRatioAndPercent();
    testMinMaxOnContainer();
    testCacheSkipsCleanSubtrees();
    testMeasureCacheForgetsOnChange();
    testDisplayNone();
    return 0;
}
