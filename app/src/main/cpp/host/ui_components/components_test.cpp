// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Behavior of the component kit: input, state, animation and semantics, without a GPU.

#include <cmath>
#include <string>
#include <variant>
#include <vector>

#include "component_harness.hpp"
#include "filepan/ui/components/components.hpp"
#include "filepan/ui/ui.hpp"

using namespace filepan;
using namespace filepan::ui;
using namespace filepan::ui::components;
using filepan::test::ComponentHarness;

namespace {

// A column at the top left holding the nodes under test.
std::shared_ptr<Box> stage(ComponentHarness &h, std::vector<NodePtr> children) {
    auto root = Box::create({.alignItems = Align::start, .gap = 8, .padding = Edges::all(16)});
    for (auto &child : children) {
        root->appendChild(std::move(child));
    }
    h.root->setContent(root);
    h.frame();
    return root;
}

void testSwitchTogglesOnTap() {
    ComponentHarness h;
    Signal<bool> checked(false);
    int haptics = 0;
    setHapticHandler([&](Haptic kind) { haptics += kind == Haptic::toggle ? 1 : 0; });
    auto toggle = Switch::create({.checked = [checked] { return checked(); },
                                  .onChange = [checked](bool on) { checked.set(on); },
                                  .label = "Show hidden files"});
    stage(h, {toggle});
    EXPECT(toggle->frame().width == 52 && toggle->frame().height == 32);
    EXPECT(toggle->knobPosition() == 0);

    const gfx::Vec2 c = h.center(*toggle);
    h.tap(c.x, c.y);
    EXPECT(checked.peek() && haptics == 1);
    // The knob springs over with the overshooting curve: past 1, then settles on it.
    float peak = 0;
    for (int i = 0; i < 40; ++i) {
        h.advance(8);
        peak = std::max(peak, toggle->knobPosition());
    }
    EXPECT(peak > 1.0f && toggle->knobPosition() == 1);

    const auto *node = h.semantics(*toggle);
    EXPECT(node != nullptr && node->role == Role::toggleSwitch && node->label == "Show hidden files");
    EXPECT((node->flags & semantics_flags::kChecked) && (node->flags & semantics_flags::kCheckable));
    EXPECT(node->actions & static_cast<std::uint32_t>(Action::click));

    // TalkBack's double tap toggles it back; under reduced motion the knob snaps.
    h.root->env().reducedMotion.set(true);
    h.frame();
    EXPECT(h.root->performAccessibilityAction(toggle->id(), Action::click));
    h.frame();
    EXPECT(!checked.peek() && toggle->knobPosition() == 0);
    // A 48 dp hit target although it's 32 dp tall: 6 dp above its top still hits.
    h.tap(c.x, c.y - 21);
    EXPECT(checked.peek());
    setHapticHandler(nullptr);
}

void testSwitchDisabled() {
    ComponentHarness h;
    Signal<bool> checked(false);
    auto toggle = Switch::create({.checked = [checked] { return checked(); },
                                  .onChange = [checked](bool on) { checked.set(on); },
                                  .label = "Thumbnails",
                                  .enabled = false});
    stage(h, {toggle});
    const gfx::Vec2 c = h.center(*toggle);
    h.tap(c.x, c.y);
    EXPECT(!checked.peek());
    const auto *node = h.semantics(*toggle);
    EXPECT(node->flags & semantics_flags::kDisabled);
}

void testButtons() {
    ComponentHarness h;
    int taps = 0;
    Signal<bool> enabled(true);
    auto primary = Button::create({.label = "Extract",
                                   .enabled = [enabled] { return enabled(); },
                                   .onTap = [&] { ++taps; },
                                   .accessibilityLabel = "Extract 3 files"});
    auto icon = IconButton::create({.icon = "search", .label = "Search", .size = 40, .onTap = [&] { taps += 10; }});
    stage(h, {primary, icon});
    EXPECT(primary->frame().height == 44);

    const gfx::Vec2 c = h.center(*primary);
    h.down(c.x, c.y);
    h.advance(150);  // Past the press delay: pressed, 1 dp down.
    EXPECT(primary->pressable().pressed.peek() && primary->transform().ty == 1);
    h.up(c.x, c.y);
    h.frame();
    EXPECT(taps == 1 && !primary->pressable().pressed.peek() && primary->transform().ty == 0);

    // The 40 dp icon button takes taps 4 dp outside its edge (48 dp target).
    const gfx::Vec2 ic = h.center(*icon);
    h.tap(ic.x, ic.y + 23);
    EXPECT(taps == 11);

    const auto *node = h.semantics(*primary);
    EXPECT(node->role == Role::button && node->label == "Extract 3 files");
    EXPECT((node->flags & semantics_flags::kFocusable) != 0);
    const auto *iconNode = h.semantics(*icon);
    EXPECT(iconNode->role == Role::button && iconNode->label == "Search");
    // Icon glyph names never leak into semantics.
    EXPECT(iconNode->children.empty());

    enabled.set(false);
    h.frame();
    h.tap(c.x, c.y);
    EXPECT(taps == 11);
    node = h.semantics(*primary);
    EXPECT((node->flags & semantics_flags::kDisabled) && !(node->actions & static_cast<std::uint32_t>(Action::click)));

    // Keyboard: focus and Enter.
    enabled.set(true);
    h.frame();
    primary->requestFocus();
    EXPECT(primary->focused());
    h.root->dispatchKey({.keyCode = keys::kEnter, .action = KeyAction::down});
    h.root->dispatchKey({.keyCode = keys::kEnter, .action = KeyAction::up});
    EXPECT(taps == 12);
}

void testSegmentedSelection() {
    ComponentHarness h;
    Signal<std::size_t> selected(0);
    auto segmented = SegmentedControl::create({.options = {"List", "Grid", "Gallery"},
                                               .selected = [selected] { return selected(); },
                                               .onSelect = [selected](std::size_t i) { selected.set(i); },
                                               .label = "View"});
    segmented->updateStyle([](Style &s) { s.width = 300; });
    stage(h, {segmented});
    const gfx::Vec2 grid = h.center(segmented->segment(2));
    h.tap(grid.x, grid.y);
    EXPECT(selected.peek() == 2);
    h.advance(600);
    const auto *chosen = h.semantics(segmented->segment(2));
    const auto *other = h.semantics(segmented->segment(0));
    EXPECT(chosen->role == Role::button && chosen->label == "Gallery");
    EXPECT((chosen->flags & semantics_flags::kChecked) && !(other->flags & semantics_flags::kChecked));
    EXPECT(other->flags & semantics_flags::kCheckable);
}

void testSliderAndCheckbox() {
    ComponentHarness h;
    Signal<float> value(0.5f);
    Signal<bool> checked(false);
    auto slider = Slider::create({.value = [value] { return value(); },
                                  .onChange = [value](float v) { value.set(v); },
                                  .label = "Text size"});
    slider->updateStyle([](Style &s) { s.width = 228; });  // 200 dp of travel.
    auto box = Checkbox::create({.checked = [checked] { return checked(); },
                                 .onChange = [checked](bool on) { checked.set(on); },
                                 .text = "Apply to all"});
    stage(h, {slider, box});
    const gfx::Vec2 c = h.center(*slider);
    // Tap near the start: about 10%.
    h.tap(c.x - 100 + 20, c.y);
    EXPECT(test::near(value.peek(), 0.1f, 0.02f));
    // Drag from the middle 50 dp right.
    h.drag(c.x, c.y, c.x + 50, c.y, 160);
    EXPECT(test::near(value.peek(), 0.75f, 0.03f));
    const auto *node = h.semantics(*slider);
    EXPECT(node->role == Role::slider && node->value == "75%");
    EXPECT(h.root->performAccessibilityAction(slider->id(), Action::scrollForward));
    EXPECT(test::near(value.peek(), 0.8f, 0.03f));

    const gfx::Vec2 b = h.center(*box);
    h.tap(b.x, b.y);
    EXPECT(checked.peek());
    node = h.semantics(*box);
    EXPECT(node->role == Role::checkbox && node->label == "Apply to all" && (node->flags & semantics_flags::kChecked));
    EXPECT(box->frame().height >= 48);
}

void testMarksAndRowsSemantics() {
    ComponentHarness h;
    auto meter = MeterRing::create({.value = 0.62f, .label = "Internal storage used"});
    auto label = SectionLabel("Folders");
    auto band = DataBand::create({.text = "Copying 12 of 40", .rate = "38 MB/s", .progress = 0.3f, .label = "Copying files"});
    band->updateStyle([](Style &s) { s.width = 300; });
    auto stamp = Stamp("Online");
    int opened = 0;
    int renamed = 0;
    auto row = FileRow::create({.onTap = [&] { ++opened; },
                                .customActions = {{1, "Rename"}},
                                .onCustomAction = [&](std::uint32_t id) { renamed += id == 1 ? 1 : 0; }});
    row->updateStyle([](Style &s) { s.width = 360; });
    row->setData({.name = "report.pdf", .meta = "2.4 MB", .icon = "picture_as_pdf", .selected = true});
    stage(h, {meter, label, band, stamp, row});

    const auto *m = h.semantics(*meter);
    EXPECT(m->role == Role::progress && m->value == "62%" && m->label == "Internal storage used");
    EXPECT(h.semantics(*label)->role == Role::heading && h.semantics(*label)->label == "Folders");
    const auto *b = h.semantics(*band);
    EXPECT(b->role == Role::progress && b->value == "30%" && (b->flags & semantics_flags::kLiveRegion));
    EXPECT(b->label == "Copying files, Copying 12 of 40, 38 MB/s");
    EXPECT(h.semantics(*stamp)->label == "Online");

    const auto *r = h.semantics(*row);
    EXPECT(r->role == Role::listItem && (r->flags & semantics_flags::kSelected));
    EXPECT(r->label == "report.pdf, 2.4 MB");
    EXPECT(r->customActions.size() == 1 && r->customActions[0].label == "Rename");
    ActionArgs args;
    args.customId = 1;
    EXPECT(h.root->performAccessibilityAction(row->id(), Action::custom, args) && renamed == 1);
    const gfx::Vec2 c = h.center(*row);
    h.tap(c.x, c.y);
    EXPECT(opened == 1 && row->frame().height == 60);

    // A spoken label replaces the visible text rather than adding to it.
    row->setData({.name = "report.pdf", .meta = "2.4 MB", .spokenLabel = "report.pdf, PDF document, 2.4 MB"});
    h.frame();
    EXPECT(h.semantics(*row)->label == "report.pdf, PDF document, 2.4 MB");
}

void testBreadcrumb() {
    ComponentHarness h;
    std::size_t chosen = 99;
    auto crumbs = Breadcrumb::create({.segments = std::vector<std::string>{"Internal", "Download", "Papers"},
                                      .onSelect = [&](std::size_t i) { chosen = i; }});
    crumbs->updateStyle([](Style &s) { s.width = 360; });
    stage(h, {crumbs});
    EXPECT(crumbs->segmentNodes().size() == 3);
    const gfx::Vec2 c = h.center(*crumbs->segmentNodes()[1]);
    h.tap(c.x, c.y);
    EXPECT(chosen == 1);
    const auto *last = h.semantics(*crumbs->segmentNodes()[2]);
    EXPECT(last->role == Role::button && last->label == "Papers" && (last->flags & semantics_flags::kSelected));
    EXPECT(crumbs->hiddenCount() == 0);

    // Too long: whole segments go from the start, so the current folder stays in view.
    auto deep = Breadcrumb::create({.segments = std::vector<std::string>{"nas.home", "srv", "media", "photos", "2026",
                                                                         "october-coast-trip"}});
    deep->updateStyle([](Style &s) { s.width = 260; });
    stage(h, {deep});
    const std::size_t hidden = deep->hiddenCount();
    EXPECT(hidden > 0 && hidden < 6);
    const gfx::Rect current = deep->segmentNodes().back()->frame();
    EXPECT(current.x >= 0 && current.right() <= 260.5f);
    EXPECT(deep->segmentNodes()[0]->frame().right() < 0);
    // Even a lone folder too long for the line keeps its start in view.
    auto lone = Breadcrumb::create({.segments = std::vector<std::string>{"Internal",
                                                                         "a-folder-name-far-too-long-for-the-line"}});
    lone->updateStyle([](Style &s) { s.width = 160; });
    stage(h, {lone});
    EXPECT(lone->hiddenCount() == 1);
    const gfx::Rect cut = lone->segmentNodes().back()->frame();
    EXPECT(cut.x > 0 && cut.right() <= 160.5f);
}

void testDockModeChange() {
    ComponentHarness h;
    Signal<bool> selecting(false);
    Signal<int> count(0);
    int copied = 0;
    auto dock = BottomDock::create({.idle = {{"add", "New", nullptr}, {"checklist", "Select", nullptr}},
                                    .selection = {{"content_copy", "Copy", [&] { ++copied; }},
                                                  {"delete", "Delete", nullptr, true}},
                                    .selecting = [selecting] { return selecting(); },
                                    .selectionCount = [count] { return count(); }});
    auto root = Box::create();
    root->appendChild(dock);
    h.root->setContent(root);
    h.frame();
    EXPECT(dock->frame().height == 64 && dock->frame().x == 16 && test::near(dock->frame().bottom(), 800 - 16));
    EXPECT(dock->modeProgress() == 0);
    EXPECT(dock->idleRow().hitTestBehavior() != HitTestBehavior::none);
    EXPECT(dock->selectionRow().semantics().hidden);

    batch([&] {
        selecting.set(true);
        count.set(12);
    });
    h.frame();
    // The idle row leaves input and semantics at once; the switch animates.
    EXPECT(dock->idleRow().hitTestBehavior() == HitTestBehavior::none && dock->idleRow().semantics().hidden);
    EXPECT(!dock->selectionRow().semantics().hidden);
    h.advance(32);
    EXPECT(dock->modeProgress() > 0 && dock->modeProgress() < 1);
    h.advance(800);
    EXPECT(dock->modeProgress() == 1 && dock->selectionRow().opacity() == 1 && dock->idleRow().opacity() == 0);

    // The stamp reads the count; the copy button works.
    h.root->setAccessibilityEnabled(true);
    h.frame();
    bool foundStamp = false;
    for (const auto &node : h.root->semantics().nodes()) {
        foundStamp = foundStamp || node.label == "12 selected";
    }
    EXPECT(foundStamp);
    Node &actions = *dock->selectionRow().children()[1];
    const gfx::Vec2 copy = h.center(*actions.children()[0]);
    h.tap(copy.x, copy.y);
    EXPECT(copied == 1);

    // Under reduced motion the switch back is a short crossfade, with nothing moving.
    h.root->env().reducedMotion.set(true);
    selecting.set(false);
    h.frame();
    h.advance(48);
    EXPECT(dock->modeProgress() > 0 && dock->modeProgress() < 1 && dock->idleRow().transform().ty == 0);
    h.advance(200);
    EXPECT(dock->modeProgress() == 0);
}

void testTopBarHidesOnScroll() {
    ComponentHarness h;
    auto list = ScrollNode::create();
    list->setStyle({.position = Position::absolute, .left = 0, .top = 0, .right = 0, .bottom = 0});
    list->appendChild(Box::create({.height = 4000}));
    auto bar = TopBar::create({.content = Box::create(), .scrollOffset = list->offsetSignal()});
    auto root = Box::create();
    root->appendChild(list);
    root->appendChild(bar);
    h.root->setContent(root);
    h.frame();
    EXPECT(bar->visibility() == 1 && test::near(bar->height(), 6 + 48));
    list->scrollTo(200);
    h.advance(600);
    EXPECT(bar->visibility() == 0 && bar->opacity() == 0);
    list->scrollTo(150);
    h.advance(600);
    EXPECT(bar->visibility() == 1);
}

void testPanelsDontBlur() {
    ComponentHarness h;
    auto panel = GlassPanel({.width = 100, .height = 100});
    stage(h, {panel});
    for (const auto &command : panel->composite()->commands()) {
        EXPECT(!std::holds_alternative<gfx::BackdropBlurCommand>(command));
    }
}

// Every command of a frame, sublists played back in place.
void flatten(const gfx::DisplayList &list, std::vector<gfx::DisplayCommand> &out) {
    for (const auto &command : list.commands()) {
        if (const auto *sub = std::get_if<gfx::DisplayListCommand>(&command)) {
            if (sub->list) {
                flatten(*sub->list, out);
            }
        } else {
            out.push_back(command);
        }
    }
}

std::vector<gfx::DisplayCommand> paintedCommands(ComponentHarness &h) {
    h.now += 16'000'000;
    h.loop.runPending();
    auto frame = h.root->frame(h.now);
    std::vector<gfx::DisplayCommand> out;
    EXPECT(frame.has_value());
    flatten(*frame->displayList, out);
    return out;
}

const gfx::ImageCommand *findImage(const std::vector<gfx::DisplayCommand> &commands, gfx::ImageId id) {
    for (const auto &command : commands) {
        if (const auto *image = std::get_if<gfx::ImageCommand>(&command); image != nullptr && image->image == id) {
            return image;
        }
    }
    return nullptr;
}

// The dark backdrop and light frame a picture thumbnail gets in a row.
bool hasPictureBackdrop(const std::vector<gfx::DisplayCommand> &commands) {
    bool backdrop = false;
    bool frame = false;
    for (const auto &command : commands) {
        if (const auto *fill = std::get_if<gfx::FillCommand>(&command)) {
            const auto *color = std::get_if<gfx::Color>(&fill->paint);
            backdrop = backdrop || (color != nullptr && color->r == 0 && color->g == 0 && color->b == 0 &&
                                    std::abs(color->a - 0.3f) < 0.01f);
        } else if (const auto *stroke = std::get_if<gfx::StrokeCommand>(&command)) {
            const auto *color = std::get_if<gfx::Color>(&stroke->paint);
            frame = frame || (color != nullptr && color->r == 1 && std::abs(color->a - 0.12f) < 0.01f);
        }
    }
    return backdrop || frame;
}

// A translucent icon thumbnail (an APK's) is drawn whole: no dark square behind it, no frame, no
// rounded corners cut from it; in the grid, centered on the kind tile.
void testIconThumbnails() {
    constexpr gfx::ImageId kPicture = 7;
    constexpr gfx::ImageId kIcon = 8;
    ComponentHarness h;
    auto row = FileRow::create();
    row->updateStyle([](Style &s) { s.width = 360; });
    stage(h, {row});
    row->setData({.name = "photo.jpg", .thumbnail = kPicture});
    auto commands = paintedCommands(h);
    const gfx::ImageCommand *image = findImage(commands, kPicture);
    EXPECT(image != nullptr && image->radii.topLeft > 0 && hasPictureBackdrop(commands));

    row->setData({.name = "app.apk", .thumbnail = kIcon, .iconThumbnail = true});
    commands = paintedCommands(h);
    image = findImage(commands, kIcon);
    EXPECT(image != nullptr && image->radii.topLeft == 0 && !hasPictureBackdrop(commands));
    EXPECT(image->destination.width > 0 && image->destination.width == image->destination.height);

    ComponentHarness g;
    auto tile = GridTile::create();
    tile->updateStyle([](Style &s) { s.width = 120; });
    stage(g, {tile});
    tile->setData({.name = "photo.jpg", .thumbnail = kPicture});
    commands = paintedCommands(g);
    const gfx::ImageCommand *picture = findImage(commands, kPicture);
    EXPECT(picture != nullptr);
    const gfx::Rect full = picture->destination;
    tile->setData({.name = "app.apk", .thumbnail = kIcon, .iconThumbnail = true});
    commands = paintedCommands(g);
    image = findImage(commands, kIcon);
    EXPECT(image != nullptr && image->radii.topLeft == 0);
    const gfx::Rect icon = image->destination;
    EXPECT(std::abs(icon.width - full.width * 0.52f) < 2 && std::abs(icon.height - icon.width) < 1);
    // Centered in the tile box (frames are in dp, relative to the parent).
    const Node &box = *tile->children()[0];
    const gfx::Rect at = box.children()[1]->frame();
    const gfx::Rect boxFrame = box.frame();
    EXPECT(std::abs((at.x + at.width / 2) - boxFrame.width / 2) < 0.5f);
    EXPECT(std::abs((at.y + at.height / 2) - boxFrame.height / 2) < 0.5f);
}

}  // namespace

int main() {
    testIconThumbnails();
    testSwitchTogglesOnTap();
    testSwitchDisabled();
    testButtons();
    testSegmentedSelection();
    testSliderAndCheckbox();
    testMarksAndRowsSemantics();
    testBreadcrumb();
    testDockModeChange();
    testTopBarHidesOnScroll();
    testPanelsDontBlur();
    return 0;
}
