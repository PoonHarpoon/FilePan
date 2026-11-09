// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <cstring>
#include <string>
#include <vector>

#include "filepan/ui/gesture.hpp"
#include "filepan/ui/scroll.hpp"
#include "ui_harness.hpp"

using namespace filepan;
using namespace filepan::ui;
using filepan::test::UiHarness;

namespace {

// Reads the wire format back, as Kotlin will.
class Reader {
public:
    explicit Reader(const ByteWriter &writer) : data_(writer.data()), size_(writer.size()) {}

    template <typename T>
    T get() {
        EXPECT(offset_ + sizeof(T) <= size_);
        T value;
        std::memcpy(&value, data_ + offset_, sizeof(T));
        offset_ += sizeof(T);
        return value;
    }
    std::string string() {
        const auto length = get<std::uint32_t>();
        EXPECT(offset_ + length <= size_);
        std::string out(reinterpret_cast<const char *>(data_ + offset_), length);
        offset_ += length;
        return out;
    }
    [[nodiscard]] bool done() const { return offset_ == size_; }

private:
    const std::uint8_t *data_;
    std::size_t size_;
    std::size_t offset_ = 0;
};

struct Update {
    bool full = false;
    std::uint32_t root = 0;
    std::vector<std::uint32_t> removed;
    std::vector<SemanticsNode> nodes;

    [[nodiscard]] const SemanticsNode *find(std::uint32_t id) const {
        for (const auto &node : nodes) {
            if (node.id == id) {
                return &node;
            }
        }
        return nullptr;
    }
};

Update parse(const ByteWriter &writer) {
    Reader r(writer);
    Update update;
    EXPECT(r.get<std::uint32_t>() == 0x4D535046);
    EXPECT(r.get<std::uint16_t>() == 1);
    update.full = (r.get<std::uint16_t>() & 1) != 0;
    update.root = r.get<std::uint32_t>();
    const auto removed = r.get<std::uint32_t>();
    for (std::uint32_t i = 0; i < removed; ++i) {
        update.removed.push_back(r.get<std::uint32_t>());
    }
    const auto count = r.get<std::uint32_t>();
    for (std::uint32_t i = 0; i < count; ++i) {
        SemanticsNode node;
        node.id = r.get<std::uint32_t>();
        node.parent = r.get<std::uint32_t>();
        const auto left = static_cast<float>(r.get<std::int32_t>());
        const auto top = static_cast<float>(r.get<std::int32_t>());
        const auto right = static_cast<float>(r.get<std::int32_t>());
        const auto bottom = static_cast<float>(r.get<std::int32_t>());
        node.bounds = gfx::Rect::fromEdges(left, top, right, bottom);
        node.role = static_cast<Role>(r.get<std::uint8_t>());
        EXPECT(r.get<std::uint8_t>() == 0);
        node.flags = r.get<std::uint16_t>();
        node.actions = r.get<std::uint32_t>();
        node.label = r.string();
        node.value = r.string();
        node.hint = r.string();
        const auto children = r.get<std::uint32_t>();
        for (std::uint32_t c = 0; c < children; ++c) {
            node.children.push_back(r.get<std::uint32_t>());
        }
        const auto custom = r.get<std::uint32_t>();
        for (std::uint32_t c = 0; c < custom; ++c) {
            CustomAction action;
            action.id = r.get<std::uint32_t>();
            action.label = r.string();
            node.customActions.push_back(action);
        }
        update.nodes.push_back(std::move(node));
    }
    EXPECT(r.done());
    return update;
}

bool has(std::uint32_t actions, Action action) { return (actions & static_cast<std::uint32_t>(action)) != 0; }

void testSnapshotAndDeltas() {
    UiHarness h;
    h.root->setAccessibilityEnabled(true);

    auto screen = Box::create({.padding = Edges::all(10)});
    auto title = Text::create("Files", {.size = 20});
    title->updateSemantics([](Semantics &s) { s.role = Role::heading; });
    auto decoration = Box::create({.height = 4});  // No semantics: transparent.
    auto sort = Box::create({.width = 24, .height = 24});
    int sorts = 0;
    auto tap = std::make_shared<TapRecognizer>();
    tap->onTap = [&] { ++sorts; };
    sort->addGestureRecognizer(tap);
    sort->updateSemantics([](Semantics &s) {
        s.role = Role::button;
        s.label = "Sort";
    });
    auto row = Box::create({.height = 60});
    row->updateSemantics([](Semantics &s) {
        s.role = Role::listItem;
        s.mergeDescendants = true;
        s.customActions = {{7, "Share"}};
    });
    row->appendChild(Text::create("photo.jpg"));
    row->appendChild(Text::create("2 MB"));
    auto hidden = Box::create({.height = 10});
    hidden->updateSemantics([](Semantics &s) {
        s.hidden = true;
        s.label = "glow";
    });
    Signal<bool> checked(false);
    auto checkbox = Box::create({.height = 48});
    checkbox->bind([raw = checkbox.get(), checked] {
        raw->updateSemantics([&](Semantics &s) {
            s.role = Role::checkbox;
            s.label = "Show hidden files";
            s.flags = semantics_flags::kCheckable | (checked() ? semantics_flags::kChecked : 0);
            s.actions = static_cast<std::uint32_t>(Action::click);
            s.onAction = [checked](Action action, const ActionArgs &) {
                if (action != Action::click) {
                    return false;
                }
                checked.set(!checked.peek());
                return true;
            };
        });
    });
    for (const auto &child : std::vector<NodePtr>{title, decoration, sort, row, hidden, checkbox}) {
        screen->appendChild(child);
    }
    h.root->setContent(screen);
    EXPECT(h.frame());

    ByteWriter full;
    EXPECT(h.root->takeSemanticsUpdate(full, true));
    const Update first = parse(full);
    EXPECT(first.full && first.root == screen->id() && first.removed.empty());
    EXPECT(first.nodes.size() == 5);  // Screen, title, sort, row, checkbox.
    EXPECT(first.nodes.front().id == screen->id());
    EXPECT(first.find(screen->id())->children ==
           (std::vector<std::uint32_t>{title->id(), sort->id(), row->id(), checkbox->id()}));
    const SemanticsNode *titleNode = first.find(title->id());
    EXPECT(titleNode->role == Role::heading && titleNode->label == "Files" && titleNode->parent == screen->id());
    const SemanticsNode *sortNode = first.find(sort->id());
    EXPECT(sortNode->role == Role::button && has(sortNode->actions, Action::click));
    // 10dp padding + 24dp title line + 4dp decoration, at density 2.
    EXPECT(sortNode->bounds == (gfx::Rect{20, 76, 48, 48}));
    const SemanticsNode *rowNode = first.find(row->id());
    EXPECT(rowNode->label == "photo.jpg, 2 MB" && rowNode->children.empty());
    EXPECT(rowNode->customActions.size() == 1 && has(rowNode->actions, Action::custom));
    EXPECT(first.find(hidden->id()) == nullptr);

    // Nothing changed: nothing to send.
    EXPECT(!h.frame());
    ByteWriter none;
    EXPECT(!h.root->takeSemanticsUpdate(none));

    // Actions route back by id.
    EXPECT(h.root->performAccessibilityAction(sort->id(), Action::click));
    EXPECT(sorts == 1);
    EXPECT(h.root->performAccessibilityAction(checkbox->id(), Action::click));
    EXPECT(checked.peek());
    h.frame();
    ByteWriter delta;
    EXPECT(h.root->takeSemanticsUpdate(delta));
    const Update second = parse(delta);
    EXPECT(!second.full && second.nodes.size() == 1);
    EXPECT(second.nodes[0].id == checkbox->id() && (second.nodes[0].flags & semantics_flags::kChecked) != 0);

    // Removing a node sends its id and the parent's new child list.
    screen->removeChild(row.get());
    h.frame();
    ByteWriter removal;
    EXPECT(h.root->takeSemanticsUpdate(removal));
    const Update third = parse(removal);
    EXPECT(third.removed == std::vector<std::uint32_t>{row->id()});
    EXPECT(third.find(screen->id()) != nullptr);
    EXPECT(!h.root->performAccessibilityAction(row->id(), Action::click));
}

void testScrollActionsAndClipping() {
    UiHarness h;
    h.root->setAccessibilityEnabled(true);
    auto scroll = ScrollNode::create();
    auto content = Box::create();
    std::vector<std::shared_ptr<Text>> items;
    for (int i = 0; i < 40; ++i) {
        items.push_back(Text::create("item " + std::to_string(i), {.size = 20}));
        items.back()->setStyle({.height = 50});
        content->appendChild(items.back());
    }
    scroll->appendChild(content);
    h.root->setContent(scroll);
    h.frame();
    const SemanticsNode *node = h.root->semantics().find(scroll->id());
    EXPECT(node && node->role == Role::scroll);
    EXPECT(has(node->actions, Action::scrollForward) && !has(node->actions, Action::scrollBackward));
    // Only the 16 rows on screen are in the snapshot.
    EXPECT(h.root->semantics().find(items[15]->id()) && !h.root->semantics().find(items[16]->id()));

    EXPECT(h.root->performAccessibilityAction(scroll->id(), Action::scrollForward));
    h.advance(500);
    EXPECT(test::near(scroll->offset(), 640));
    node = h.root->semantics().find(scroll->id());
    EXPECT(has(node->actions, Action::scrollBackward));
    EXPECT(h.root->semantics().find(items[20]->id()) && !h.root->semantics().find(items[2]->id()));
}

}  // namespace

int main() {
    testSnapshotAndDeltas();
    testScrollActionsAndClipping();
    return 0;
}
