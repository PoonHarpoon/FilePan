// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// A small screen built the way the component kit will build them: a column with a header, a
// virtual list and a button bound to a signal.

#include <memory>
#include <string>
#include <vector>

#include "filepan/ui/ui.hpp"
#include "ui_harness.hpp"

using namespace filepan;
using namespace filepan::ui;

namespace {

NodePtr filesScreen(const std::vector<std::string> &files, Signal<int> selected) {
    const Theme &theme = environment().theme.peek();
    const ColorTokens &c = theme.colors;

    auto screen = Box::create({}, {.background = gfx::Paint{c.ground}});

    auto header = Text::create("Downloads", theme.type.title, c.ink);
    header->setStyle({.padding = Edges::symmetric(theme.spacing.page, theme.spacing.m)});
    header->updateSemantics([](Semantics &s) { s.role = Role::heading; });

    auto list = VirtualList::create();
    list->setStyle({.grow = 1});
    list->setFixedItemExtent(theme.spacing.listRowCompact);
    // Rows are built once per recycled node and rebound by index.
    list->setCreate([theme](std::uint32_t) -> NodePtr {
        auto row = Box::create({.direction = Direction::row, .alignItems = Align::center,
                                .padding = Edges::symmetric(theme.spacing.page, 0)});
        auto name = Text::create({}, theme.type.body, theme.colors.ink);
        name->setMaxLines(1);
        name->setOverflow(gfx::TextOverflow::ellipsisMiddle);
        name->setStyle({.grow = 1, .shrink = 1});
        row->appendChild(name);
        return row;
    });
    list->setBind([&files](std::size_t index, Node &row) {
        static_cast<Text &>(*row.children()[0]).setText(files[index]);
    });
    list->setItemCount(files.size());

    auto button = Box::create({.justify = Justify::center, .alignItems = Align::center,
                               .margin = Edges::all(theme.spacing.page), .height = 48},
                              {.background = gfx::Paint{c.destructive}, .radii = gfx::CornerRadii::uniform(theme.radii.pill)});
    auto label = Text::create({}, theme.type.button, c.onDestructive);
    // Fine-grained: when selected changes, only this text and the button's state update.
    label->bind([raw = label.get(), selected] { raw->setText("Delete " + std::to_string(selected())); });
    button->bind([raw = button.get(), selected] {
        const bool enabled = selected() > 0;
        raw->setOpacity(enabled ? 1 : 0.6f);
        raw->updateSemantics([&](Semantics &s) {
            s.role = Role::button;
            s.label = "Delete " + std::to_string(selected.peek()) + " files";
            s.flags = enabled ? 0 : semantics_flags::kDisabled;
        });
    });
    auto tap = std::make_shared<TapRecognizer>();
    tap->onTap = [selected] { selected.set(0); };
    button->addGestureRecognizer(tap);
    button->appendChild(label);

    screen->appendChild(header);
    screen->appendChild(list);
    screen->appendChild(button);
    return screen;
}

void testExampleScreen() {
    test::UiHarness h;
    std::vector<std::string> files;
    for (int i = 0; i < 5000; ++i) {
        files.push_back("file " + std::to_string(i) + ".txt");
    }
    Signal<int> selected(3);
    auto screen = filesScreen(files, selected);
    h.root->setContent(screen);
    EXPECT(h.frame());

    auto *button = screen->children()[2].get();
    auto *label = static_cast<Text *>(button->children()[0].get());
    EXPECT(label->text() == "Delete 3");
    // The button sits at the bottom: 800dp minus its 16dp margin and 48dp height.
    EXPECT(test::near(button->frame().y, 736));
    auto *list = static_cast<VirtualList *>(screen->children()[1].get());
    EXPECT(list->boundRange().first == 0 && list->boundRange().second > 10);

    h.down(200, 760);
    h.up(200, 760);
    EXPECT(selected.peek() == 0);
    EXPECT(h.frame());
    EXPECT(label->text() == "Delete 0" && button->opacity() == 0.6f);
}

}  // namespace

int main() {
    testExampleScreen();
    return 0;
}
