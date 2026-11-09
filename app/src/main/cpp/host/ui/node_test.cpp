// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <variant>

#include "filepan/ui/gesture.hpp"
#include "filepan/ui/layout.hpp"
#include "filepan/ui/node.hpp"
#include "filepan/ui/scroll.hpp"
#include "ui_test_util.hpp"

using namespace filepan;
using namespace filepan::ui;

namespace {

test::FakeTextSystem text;
constexpr float kDensity = 2;
const LayoutContext kContext{&text, kDensity, 1};

void layout(Node &root, gfx::Size size) {
    LayoutEngine engine(kContext);
    engine.layoutRoot(root, size);
}

std::shared_ptr<Box> at(float x, float y, float w, float h) {
    return Box::create({.width = w, .height = h, .position = Position::absolute, .left = x, .top = y});
}

Node *hit(Node &root, float xDp, float yDp) {
    std::vector<NodePtr> path;
    if (!Node::hitTest(root, {xDp * kDensity, yDp * kDensity}, kDensity, path)) {
        return nullptr;
    }
    return path.front().get();
}

void interactive(Node &node) { node.setPointerHandler([](const PointerEvent &) {}); }

void testHitTesting() {
    auto root = Box::create();
    auto a = at(10, 10, 100, 100);
    interactive(*a);
    root->appendChild(a);

    // A clip hides the part of a child outside it.
    auto clipper = at(200, 0, 100, 100);
    clipper->setClip(true);
    auto overflowing = at(80, 0, 50, 50);
    interactive(*overflowing);
    clipper->appendChild(overflowing);
    root->appendChild(clipper);

    // Transforms move and scale the hit area.
    auto moved = at(0, 300, 100, 100);
    moved->setTransform(gfx::Transform::translation({50, 0}));
    interactive(*moved);
    root->appendChild(moved);
    auto shrunk = at(200, 300, 100, 100);
    shrunk->setTransform(gfx::Transform::scale(0.5f, 0.5f));  // Around its center.
    interactive(*shrunk);
    root->appendChild(shrunk);

    // A small icon gets a 48dp target; a precise hit on its neighbor still wins.
    auto icon = at(300, 500, 24, 24);
    interactive(*icon);
    auto neighbor = at(326, 500, 24, 24);
    interactive(*neighbor);
    root->appendChild(icon);
    root->appendChild(neighbor);

    // Interactive container with a small child: a near miss on the child goes to the child.
    auto card = at(0, 600, 200, 100);
    interactive(*card);
    auto chip = at(20, 20, 20, 20);
    interactive(*chip);
    card->appendChild(chip);
    root->appendChild(card);

    layout(*root, {400, 800});

    EXPECT(hit(*root, 50, 50) == a.get());
    EXPECT(hit(*root, 290, 10) == overflowing.get());
    EXPECT(hit(*root, 310, 10) == nullptr);  // Inside the child, outside the clip.
    EXPECT(hit(*root, 120, 350) == moved.get());
    EXPECT(hit(*root, 20, 350) == nullptr);
    EXPECT(hit(*root, 249, 349) == shrunk.get());
    EXPECT(hit(*root, 210, 310) == nullptr);
    EXPECT(hit(*root, 296, 510) == icon.get());       // 4dp left of the icon: slop.
    EXPECT(hit(*root, 330, 510) == neighbor.get());   // Inside the neighbor.
    EXPECT(hit(*root, 280, 510) == nullptr);          // Beyond the 48dp target.
    EXPECT(hit(*root, 17, 630) == chip.get());
    EXPECT(hit(*root, 150, 650) == card.get());

    // Scrolled content hit-tests at its scrolled position.
    auto scroll = ScrollNode::create();
    scroll->setStyle({.width = 100, .height = 100, .position = Position::absolute, .left = 0, .top = 0});
    auto content = Box::create({.height = 1000});
    auto target = at(0, 150, 100, 50);
    interactive(*target);
    content->appendChild(target);
    scroll->appendChild(content);
    auto root2 = Box::create();
    root2->appendChild(scroll);
    layout(*root2, {400, 800});
    scroll->scrollTo(100);
    EXPECT(hit(*root2, 50, 75) == target.get());
    EXPECT(hit(*root2, 50, 20) == scroll.get());  // The scroll itself takes drags.
}

int countDraws(const gfx::DisplayList &list) {
    int count = 0;
    for (const auto &command : list.commands()) {
        count += std::holds_alternative<gfx::DisplayListCommand>(command) ? 1 : 0;
    }
    return count;
}

bool references(const gfx::DisplayList &list, const std::shared_ptr<const gfx::DisplayList> &child) {
    for (const auto &command : list.commands()) {
        if (const auto *draw = std::get_if<gfx::DisplayListCommand>(&command); draw && draw->list == child) {
            return true;
        }
    }
    return false;
}

void testDisplayListCaching() {
    auto root = Box::create({.gap = 10}, {.background = gfx::Paint{gfx::Color{0, 0, 0, 1}}});
    std::vector<std::shared_ptr<Box>> rows;
    std::vector<int> paints(3, 0);
    for (int i = 0; i < 3; ++i) {
        auto row = Box::create({.height = 40}, {.background = gfx::Paint{gfx::Color{1, 1, 1, 0.1f}}});
        row->setPaintHook([&paints, i](PaintContext &) { ++paints[static_cast<std::size_t>(i)]; });
        row->appendChild(Text::create("name", {.size = 15}));
        rows.push_back(row);
        root->appendChild(row);
    }
    layout(*root, {400, 800});
    const auto first = root->record(kContext);
    EXPECT(first && countDraws(*first) == 4);  // Its own background + three rows.
    EXPECT(paints == (std::vector<int>{1, 1, 1}));
    const auto row0 = rows[0]->composite();
    const auto row2 = rows[2]->composite();

    // Nothing dirty: the same list comes back.
    EXPECT(!root->needsRecord());
    EXPECT(root->record(kContext) == first);

    // Repainting one row re-records only it; the clean rows' lists are reused by reference.
    rows[1]->updateDecoration([](BoxDecoration &d) { d.background = gfx::Paint{gfx::Color{1, 0, 0, 1}}; });
    const auto second = root->record(kContext);
    EXPECT(second != first);
    EXPECT(paints == (std::vector<int>{1, 2, 1}));
    EXPECT(root->paintCount() == 1);  // The parent only recomposed.
    EXPECT(rows[0]->composite() == row0 && rows[2]->composite() == row2);
    EXPECT(references(*second, row0) && references(*second, row2));

    // Fading a row recomposes the parent without repainting anything.
    rows[2]->setOpacity(0.5f);
    const auto third = root->record(kContext);
    EXPECT(paints == (std::vector<int>{1, 2, 1}));
    bool faded = false;
    for (const auto &command : third->commands()) {
        if (const auto *opacity = std::get_if<gfx::PushOpacityCommand>(&command)) {
            faded = opacity->opacity == 0.5f;
        }
    }
    EXPECT(faded);

    // A resize from layout repaints the resized node.
    rows[0]->updateStyle([](Style &s) { s.height = 60; });
    layout(*root, {400, 800});
    root->record(kContext);
    EXPECT(paints == (std::vector<int>{2, 2, 1}));
}

void testPxConversion() {
    auto root = Box::create({.padding = Edges::all(8)});
    auto child = Box::create({.height = 20}, {.background = gfx::Paint{gfx::Color{1, 1, 1, 1}},
                                              .radii = gfx::CornerRadii::uniform(4)});
    root->appendChild(child);
    layout(*root, {100, 100});
    const auto list = root->record(kContext);
    // The child is translated by 8dp = 16px and fills 84x20dp = 168x40px with 8px radii.
    bool translated = false;
    for (const auto &command : list->commands()) {
        if (const auto *t = std::get_if<gfx::PushTransformCommand>(&command)) {
            translated = t->transform == gfx::Transform::translation({16, 16});
        }
    }
    EXPECT(translated);
    const auto &fill = std::get<gfx::FillCommand>(child->composite()->commands().front());
    EXPECT(fill.shape.rect == (gfx::Rect{0, 0, 168, 40}));
    EXPECT(fill.shape.radii == gfx::CornerRadii::uniform(8));
}

}  // namespace

int main() {
    testHitTesting();
    testDisplayListCaching();
    testPxConversion();
    return 0;
}
