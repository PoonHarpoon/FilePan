// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Sheets, dialogs, menus and snackbars: dismissal, focus and semantics.

#include <string>
#include <vector>

#include "component_harness.hpp"
#include "filepan/ui/components/components.hpp"
#include "filepan/ui/ui.hpp"

using namespace filepan;
using namespace filepan::ui;
using namespace filepan::ui::components;
using filepan::test::ComponentHarness;

namespace {

struct SheetFixture {
    ComponentHarness h;
    int dismissed = 0;
    std::shared_ptr<BottomSheet> sheet;
    std::shared_ptr<Button> first;
    std::shared_ptr<Button> second;
    std::shared_ptr<Button> behind;

    SheetFixture() {
        auto root = Box::create();
        behind = Button::create({.label = "Behind"});
        root->appendChild(behind);
        auto content = Box::create({.gap = 8, .padding = Edges::all(16), .height = 300});
        first = Button::create({.label = "Name"});
        second = Button::create({.label = "Date"});
        content->appendChild(first);
        content->appendChild(second);
        sheet = BottomSheet::create({.title = "Sort", .content = content, .onDismiss = [this] { ++dismissed; }});
        root->appendChild(sheet);
        h.root->setContent(root);
        h.frame();
        sheet->open(false);
        h.frame();
    }

    float panelHeight() { return sheet->panel().frame().height; }
    gfx::Vec2 panelPoint() { return {200, 800 - panelHeight() + 40}; }
};

void testSheetDragBelowThresholdSpringsBack() {
    SheetFixture f;
    EXPECT(f.sheet->isOpen() && f.sheet->offset() == 0);
    EXPECT(test::near(f.sheet->panel().frame().bottom(), 800));
    const float height = f.panelHeight();
    EXPECT(height > 300);
    const gfx::Vec2 p = f.panelPoint();
    // Slowly, to just under the threshold (the drag loses its 8 dp slop).
    const float distance = height * BottomSheet::kDismissFraction - 4;
    f.h.drag(p.x, p.y, p.x, p.y + distance, 800);
    f.h.advance(600);
    EXPECT(f.sheet->isOpen() && f.dismissed == 0 && test::near(f.sheet->offset(), 0, 0.5f));
}

void testSheetDragPastThresholdDismisses() {
    SheetFixture f;
    const float height = f.panelHeight();
    const gfx::Vec2 p = f.panelPoint();
    // Mid-drag the panel follows the finger.
    f.h.drag(p.x, p.y, p.x, p.y + 60, 400, false);
    EXPECT(test::near(f.sheet->offset(), 60 - 8, 1));
    f.h.drag(p.x, p.y + 60, p.x, p.y + height * BottomSheet::kDismissFraction + 30, 600);
    EXPECT(!f.sheet->isOpen());
    f.h.advance(800);
    EXPECT(f.dismissed == 1 && f.sheet->offset() >= height);
    // Gone from input and semantics once dismissed.
    EXPECT(f.sheet->hitTestBehavior() == HitTestBehavior::none && f.sheet->semantics().hidden);
}

void testSheetFlingDismisses() {
    SheetFixture f;
    const gfx::Vec2 p = f.panelPoint();
    // 50 dp in 32 ms is about 1500 dp/s, past kDismissVelocity, though short of the threshold.
    f.h.drag(p.x, p.y, p.x, p.y + 50, 32);
    f.h.advance(800);
    EXPECT(f.dismissed == 1);
}

void testSheetRisesAboveKeyboard() {
    SheetFixture f;
    const float height = f.panelHeight();
    // The keyboard comes up: the panel sits on it, and drops the navigation bar's padding.
    f.h.root->env().safeArea.set({0, 0, 0, 48});  // px, at density 2.
    f.h.frame();
    const float withBar = f.panelHeight();
    EXPECT(test::near(f.sheet->panel().frame().bottom(), 800) && test::near(withBar, height + 24));
    f.h.root->env().imeInsets.set({0, 0, 0, 600});
    f.h.frame();
    EXPECT(test::near(f.sheet->panel().frame().bottom(), 500) && test::near(f.panelHeight(), height));
    f.h.root->env().imeInsets.set({});
    f.h.frame();
    EXPECT(test::near(f.sheet->panel().frame().bottom(), 800) && test::near(f.panelHeight(), withBar));
}

void testSheetSemanticsFocusAndBack() {
    SheetFixture f;
    const auto *dialog = f.h.semantics(f.sheet->panel());
    EXPECT(dialog != nullptr && dialog->role == Role::dialog && dialog->label == "Sort");
    EXPECT(dialog->actions & static_cast<std::uint32_t>(Action::dismiss));

    // Tab cycles within the sheet, never reaching the button behind it.
    f.first->requestFocus();
    const auto tab = [&] {
        f.h.root->dispatchKey({.keyCode = keys::kTab, .action = KeyAction::down});
        f.h.root->dispatchKey({.keyCode = keys::kTab, .action = KeyAction::up});
    };
    tab();
    EXPECT(f.second->focused());
    tab();
    EXPECT(f.first->focused() && !f.behind->focused());

    EXPECT(f.h.root->performAccessibilityAction(f.sheet->panel().id(), Action::dismiss));
    EXPECT(!f.sheet->isOpen() && !f.first->focused());
    EXPECT(!f.sheet->handleBack());
    f.h.advance(800);
    EXPECT(f.dismissed == 1);
}

void testSheetScrimTapDismisses() {
    SheetFixture f;
    f.h.tap(200, 40);
    f.h.advance(800);
    EXPECT(f.dismissed == 1);
}

void testDialog() {
    ComponentHarness h;
    int dismissed = 0;
    int replaced = 0;
    auto root = Box::create();
    auto dialog = Dialog::create({.title = "Replace file?",
                                  .message = "notes.md exists.",
                                  .actions = {Button::create({.label = "Replace", .onTap = [&] { ++replaced; }})},
                                  .onDismiss = [&] { ++dismissed; }});
    root->appendChild(dialog);
    h.root->setContent(root);
    h.frame();
    EXPECT(dialog->semantics().hidden);
    dialog->open();
    h.advance(600);
    EXPECT(dialog->card().opacity() == 1);
    const auto *node = h.semantics(dialog->card());
    EXPECT(node->role == Role::dialog && node->label == "Replace file?");
    Node &actions = *dialog->card().children().back();
    const gfx::Vec2 c = h.center(*actions.children()[0]);
    h.tap(c.x, c.y);
    EXPECT(replaced == 1);
    EXPECT(dialog->handleBack() && !dialog->isOpen());
    h.advance(300);
    EXPECT(dismissed == 1 && dialog->card().opacity() == 0);
}

void testDialogRisesAboveKeyboard() {
    ComponentHarness h;
    auto root = Box::create();
    auto dialog = Dialog::create({.title = "Rename",
                                  .content = Box::create({.height = 160}),
                                  .actions = {Button::create({.label = "Cancel"}), Button::create({.label = "Rename"})}});
    root->appendChild(dialog);
    h.root->setContent(root);
    dialog->open(false);
    h.frame();
    const gfx::Rect card = dialog->card().frame();
    EXPECT(test::near(card.y + card.height / 2, 400));
    // The keyboard comes up: the card centers in the room above it.
    h.root->env().imeInsets.set({0, 0, 0, 600});  // px, at density 2.
    h.frame();
    gfx::Rect up = dialog->card().frame();
    EXPECT(test::near(up.height, card.height) && test::near(up.y + up.height / 2, (24 + 800 - 316) / 2.0f));
    // Little room, as on a phone turned sideways: the card shrinks to fit, its actions still on it.
    h.root->env().imeInsets.set({0, 0, 0, 1200});
    h.frame();
    up = dialog->card().frame();
    const gfx::Rect actions = dialog->card().children().back()->frame();
    EXPECT(up.y >= 24 - 0.5f && up.bottom() <= 800 - 616 + 0.5f && up.height < card.height);
    EXPECT(actions.bottom() <= up.height + 0.5f);
    h.root->env().imeInsets.set({});
    h.frame();
    up = dialog->card().frame();
    EXPECT(test::near(up.y, card.y) && test::near(up.height, card.height));
}

void testMenu() {
    ComponentHarness h;
    int renamed = 0;
    int dismissed = 0;
    auto root = Box::create();
    auto menu = Menu::create({.items = {{"edit", "Rename", [&] { ++renamed; }}, {"delete", "Delete", nullptr, true}},
                              .anchor = {340, 60, 48, 48},
                              .onDismiss = [&] { ++dismissed; }});
    root->appendChild(menu);
    h.root->setContent(root);
    h.frame();
    // Right-aligned to the anchor, which sits at the screen's edge, and just below it.
    const gfx::Rect card = menu->card().frame();
    EXPECT(test::near(card.right(), 388) && test::near(card.y, 112));
    const auto *list = h.semantics(menu->card());
    EXPECT(list->role == Role::menu);
    // Short enough: nothing to scroll.
    EXPECT(menu->scroll().maxOffset() == 0);
    Node &item = *menu->scroll().children()[0];
    EXPECT(h.semantics(item)->role == Role::menuItem && h.semantics(item)->label == "Rename");
    const gfx::Vec2 c = h.center(item);
    h.tap(c.x, c.y);
    EXPECT(renamed == 1 && dismissed == 1);

    // A menu near the bottom opens upward; a tap outside dismisses it.
    auto low = Menu::create({.items = {{"", "One", nullptr}, {"", "Two", nullptr}},
                             .anchor = {16, 740, 48, 48},
                             .onDismiss = [&] { ++dismissed; }});
    root->appendChild(low);
    h.frame();
    EXPECT(low->card().frame().bottom() <= 740);
    h.tap(300, 300);
    EXPECT(dismissed == 2);

    // Taller than the room below its anchor (and above): capped to the safe area less a margin,
    // and its items scroll.
    h.root->env().safeArea.set({0, 48, 0, 32});
    std::vector<MenuItem> many;
    int chosen = -1;
    for (int i = 0; i < 30; ++i) {
        many.push_back({"", "Item " + std::to_string(i), [&chosen, i] { chosen = i; }});
    }
    auto tall = Menu::create({.items = std::move(many), .anchor = {340, 60, 48, 48}, .onDismiss = [&] { ++dismissed; }});
    root->appendChild(tall);
    h.frame();
    const gfx::Rect capped = tall->card().frame();
    EXPECT(test::near(capped.y, 112) && test::near(capped.bottom(), 800 - 16 - 8));
    ScrollNode &scroll = tall->scroll();
    EXPECT(scroll.maxOffset() > 0 && scroll.offset() == 0);
    Node &lastItem = *scroll.children().back();
    // Keyboard: back from the first item wraps to the last, which scrolls into view.
    scroll.children().front()->requestFocus();
    h.root->dispatchKey({.keyCode = keys::kTab, .action = KeyAction::down, .modifiers = modifiers::kShift});
    h.advance(600);
    EXPECT(lastItem.focused() && test::near(scroll.offset(), scroll.maxOffset()));
    const gfx::Vec2 at = h.center(lastItem);
    EXPECT(at.y > capped.y && at.y < capped.bottom());
    h.tap(at.x, at.y);
    EXPECT(chosen == 29 && dismissed == 3);
    h.root->env().safeArea.set({});
}

void testSnackbarQueue() {
    ComponentHarness h;
    auto root = Box::create();
    auto host = SnackbarHost::create(&h.loop);
    root->appendChild(host);
    h.root->setContent(root);
    h.frame();
    int undone = 0;
    host->show({.text = "3 files moved to Trash", .action = "Undo", .onAction = [&] { ++undone; }});
    EXPECT(host->current() && host->current()->text == "3 files moved to Trash" && host->queued() == 0);
    const auto *node = h.semantics(*host);
    EXPECT((node->flags & semantics_flags::kLiveRegion) && node->label == "3 files moved to Trash");
    h.advance(600);
    // The action button runs it and dismisses it.
    Node &action = *host->children()[0]->children()[1]->children()[0];
    const gfx::Vec2 c = h.center(action);
    h.tap(c.x, c.y);
    EXPECT(undone == 1 && !host->current());
    h.advance(300);

    // A new message replaces the one showing, and anything waiting: the old one is out of date.
    host->show({.text = "Copied 3 items"});
    h.advance(300);
    host->show({.text = "Moved 2 items"});
    EXPECT(!host->current() && host->queued() == 1);
    h.advance(300);
    EXPECT(host->current() && host->current()->text == "Moved 2 items" && host->queued() == 0);
    // A toast goes sooner than a message with an action.
    h.advance(kToastMs + 100);
    EXPECT(!host->current());
    host->show({.text = "Deleted", .action = "Undo", .onAction = [] {}});
    h.advance(kToastMs + 100);
    EXPECT(host->current() && host->current()->text == "Deleted");
    h.advance(kSnackbarActionMs - kToastMs);
    EXPECT(!host->current());
    h.advance(300);

    // Held, what shows goes back to wait; only the newest is kept, and shows once released.
    host->show({.text = "Copied 3 items"});
    h.advance(300);
    host->setHeld(true);
    host->show({.text = "Moved 2 items"});
    h.advance(300);
    EXPECT(!host->current() && host->queued() == 1);
    host->setHeld(false);
    EXPECT(host->current() && host->current()->text == "Moved 2 items" && host->queued() == 0);
}

}  // namespace

int main() {
    testSheetDragBelowThresholdSpringsBack();
    testSheetDragPastThresholdDismisses();
    testSheetFlingDismisses();
    testSheetRisesAboveKeyboard();
    testSheetSemanticsFocusAndBack();
    testSheetScrimTapDismisses();
    testDialog();
    testDialogRisesAboveKeyboard();
    testMenu();
    testSnackbarQueue();
    return 0;
}
