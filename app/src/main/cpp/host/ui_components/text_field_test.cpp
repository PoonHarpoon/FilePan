// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// TextField editing through a fake IME, hardware keys, taps and caret drags.

#include <string>
#include <vector>

#include "component_harness.hpp"
#include "filepan/ui/components/text_field.hpp"
#include "filepan/ui/ui.hpp"

using namespace filepan;
using namespace filepan::ui;
using namespace filepan::ui::components;
using filepan::test::ComponentHarness;

namespace {

class FakeIme final : public TextInputPlatform {
public:
    void show(const TextInputConfig &config) override {
        visible = true;
        shownConfig = config;
    }
    void hide() override { visible = false; }
    void updateEditingValue(const TextEditingValue &value) override { last = value; }

    bool visible = false;
    TextInputConfig shownConfig;
    TextEditingValue last;
};

struct Fixture {
    ComponentHarness h;
    FakeIme ime;
    std::vector<std::string> changes;
    int submits = 0;
    Signal<std::string> error{std::string()};
    std::shared_ptr<TextField> field;

    Fixture() {
        h.root->textInput().setPlatform(&ime);
        field = TextField::create({.label = "File name",
                                   .placeholder = "Name",
                                   .text = "hello",
                                   .helper = "Letters and numbers",
                                   .error = [this] { return error(); },
                                   .config = {.action = ImeAction::done},
                                   .onChange = [this](const std::string &text) { changes.push_back(text); },
                                   .onSubmit = [this] { ++submits; }});
        auto root = Box::create({.padding = Edges::all(16)});
        root->appendChild(field);
        h.root->setContent(root);
        h.frame();
    }

    EditableText &editor() { return field->editor(); }
    [[nodiscard]] const TextEditingValue &value() { return field->editor().editingValue(); }
    // The x (window dp) of the caret before byte, with the fake's 7.5 dp per character.
    float xOf(int byte) { return h.center(editor()).x - editor().frame().width / 2 + 7.5f * static_cast<float>(byte); }
    float y() { return h.center(editor()).y; }
};

void testFocusAndIme() {
    Fixture f;
    EXPECT(f.field->well().frame().height == 48);
    EXPECT(!f.editor().focused() && !f.ime.visible);
    // A tap between "he" and "llo" focuses the field and puts the caret there.
    f.h.tap(f.xOf(2) + 1, f.y());
    EXPECT(f.editor().focused() && f.ime.visible && f.ime.shownConfig.action == ImeAction::done);
    EXPECT(f.h.root->textInput().client() == &f.editor());
    EXPECT(f.value().selection == (TextRange{2, 2}));

    TextInputClient &client = *f.h.root->textInput().client();
    client.commitText("y");
    EXPECT(f.value().text == "heyllo" && f.value().selection == (TextRange{3, 3}));
    EXPECT(f.changes.size() == 1 && f.changes.back() == "heyllo");
    client.setComposingText("wo");
    EXPECT(f.value().text == "heywollo" && f.value().composing == (TextRange{3, 5}));
    client.setComposingText("world");
    client.finishComposingText();
    EXPECT(f.value().text == "heyworldllo" && !f.value().composing.valid());
    client.deleteSurroundingText(5, 0);
    EXPECT(f.value().text == "heyllo");
    client.performAction(ImeAction::done);
    EXPECT(f.submits == 1);

    // The caret rect for the IME's cursor anchor is in window px, at the caret.
    const gfx::Rect caret = client.caretRect();
    EXPECT(test::near(caret.x, f.xOf(3) * ComponentHarness::kDensity, 1) && caret.height > 0);

    // Tapping the well's padding focuses too, with the caret at the end.
    f.editor().clearFocus();
    f.h.frame();
    const gfx::Vec2 well = f.h.center(f.field->well());
    f.h.tap(well.x - f.field->well().frame().width / 2 + 4, well.y);
    EXPECT(f.editor().focused() && f.value().selection == (TextRange{6, 6}));
}

// What changes on this side (a tap that moves the caret, a hardware key) reaches the IME with the
// next frame, so its next edit lands where the caret now is.
void testAppEditsReachTheIme() {
    Fixture f;
    f.h.tap(f.xOf(5), f.y());
    f.h.frame();
    EXPECT(f.ime.last.text == "hello" && f.ime.last.selection == (TextRange{5, 5}));
    f.h.tap(f.xOf(2) + 1, f.y());
    f.h.frame();
    EXPECT(f.ime.last.selection == (TextRange{2, 2}));
    f.h.root->dispatchKey({.keyCode = keys::kDel, .action = KeyAction::down});
    f.h.frame();
    EXPECT(f.ime.last.text == "hllo" && f.ime.last.selection == (TextRange{1, 1}));
}

void testHardwareKeys() {
    Fixture f;
    f.editor().requestFocus();
    f.editor().setSelection(5, 5);
    const auto key = [&](std::int32_t code, std::uint32_t modifiers = 0, std::uint32_t codePoint = 0) {
        f.h.root->dispatchKey({.keyCode = code, .action = KeyAction::down, .modifiers = modifiers, .codePoint = codePoint});
        f.h.root->dispatchKey({.keyCode = code, .action = KeyAction::up, .modifiers = modifiers});
    };
    key(keys::kDel);
    EXPECT(f.value().text == "hell" && f.value().selection == (TextRange{4, 4}));
    key(keys::kDpadLeft);
    key(keys::kDpadLeft, modifiers::kShift);
    EXPECT(f.value().selection == (TextRange{3, 2}));
    key(0, 0, 'A');
    EXPECT(f.value().text == "heAl" && f.value().selection == (TextRange{3, 3}));
    key(keys::kMoveHome);
    key(keys::kForwardDel);
    EXPECT(f.value().text == "eAl" && f.value().selection == (TextRange{0, 0}));
    // Multi-byte characters move and delete whole.
    f.editor().setText("a\xC3\xA9");
    key(keys::kDel);
    EXPECT(f.value().text == "a");
    key(keys::kEnter);
    EXPECT(f.submits == 1);
}

void testCaretDragAndSemantics() {
    Fixture f;
    f.h.tap(f.xOf(1) + 1, f.y());
    EXPECT(f.value().selection == (TextRange{1, 1}));
    // Dragging along the line moves the caret with the finger.
    f.h.drag(f.xOf(1) + 1, f.y(), f.xOf(4) + 1, f.y(), 120);
    EXPECT(f.value().selection == (TextRange{4, 4}));

    const auto *node = f.h.semantics(f.editor());
    EXPECT(node != nullptr && node->role == Role::textField && node->label == "File name");
    EXPECT(node->value == "hello" && node->hint == "Letters and numbers");
    EXPECT(node->actions & static_cast<std::uint32_t>(Action::setText));
    ActionArgs args;
    args.text = "report.pdf";
    EXPECT(f.h.root->performAccessibilityAction(f.editor().id(), Action::setText, args));
    EXPECT(f.value().text == "report.pdf" && f.changes.back() == "report.pdf");

    // An error replaces the helper, in the hint too.
    f.error.set("Name taken");
    f.h.frame();
    node = f.h.semantics(f.editor());
    EXPECT(node->hint == "Name taken" && node->value == "report.pdf");
}

void testLongTextScrollsToCaret() {
    Fixture f;
    f.editor().requestFocus();
    f.editor().setText(std::string(80, 'x'));  // 600 dp of text in a ~336 dp field.
    f.h.frame();
    // The caret at the end is kept in view: its window x is inside the editor.
    const gfx::Rect caret = f.editor().caretRect();
    const float right = (f.h.center(f.editor()).x + f.editor().frame().width / 2) * ComponentHarness::kDensity;
    EXPECT(caret.x <= right + 0.5f && caret.x > right - 20);
}

// A password field draws and speaks a bullet per character, and its caret still lands on the
// real text's offsets.
void testPasswordIsMasked() {
    ComponentHarness h;
    FakeIme ime;
    h.root->textInput().setPlatform(&ime);
    auto field = TextField::create(
            {.label = "Password", .text = "p\xC3\xA9w", .config = {.keyboard = KeyboardType::password}});
    auto root = Box::create({.padding = Edges::all(16)});
    root->appendChild(field);
    h.root->setContent(root);
    h.frame();
    EditableText &editor = field->editor();
    EXPECT(editor.obscured() && editor.shaped() != nullptr);
    const auto *node = h.semantics(editor);
    EXPECT(node != nullptr && node->value == "\u2022\u2022\u2022");
    // A tap past the second bullet lands after "p\xC3\xA9", never mid-character.
    const float left = h.center(editor).x - editor.frame().width / 2;
    const float bullet = (editor.shaped()->caretRect(3).x - editor.shaped()->caretRect(0).x) / ComponentHarness::kDensity;
    h.tap(left + bullet * 2 + 0.2f, h.center(editor).y);
    EXPECT(editor.editingValue().selection == (TextRange{3, 3}));
    editor.commitText("!");
    EXPECT(editor.editingValue().text == "p\xC3\xA9!w");
}

// A multi-line field wraps, takes Enter as a newline, grows with its text up to its lines and
// then scrolls to keep the caret in view.
void testMultiline() {
    ComponentHarness h;
    h.text.wrapLines = true;
    FakeIme ime;
    h.root->textInput().setPlatform(&ime);
    int submits = 0;
    std::vector<std::string> changes;
    auto field = TextField::create({.label = "Private key",
                                    .placeholder = "-----BEGIN OPENSSH PRIVATE KEY-----",
                                    .config = {.action = ImeAction::done},
                                    .onChange = [&](const std::string &text) { changes.push_back(text); },
                                    .onSubmit = [&] { ++submits; },
                                    .lines = 4});
    auto root = Box::create({.padding = Edges::all(16)});
    root->appendChild(field);
    h.root->setContent(root);
    h.frame();
    EditableText &editor = field->editor();
    EXPECT(editor.multiline());
    // Empty, it's as tall as a single-line field.
    EXPECT(field->well().frame().height == 48);
    const float line = editor.frame().height;
    EXPECT(line > 10 && line < 30);

    // The IME is told it's multi-line, with no action key; Enter is a newline.
    editor.requestFocus();
    h.frame();
    EXPECT(ime.visible && ime.shownConfig.multiline && ime.shownConfig.action == ImeAction::none);
    editor.commitText("hello");
    h.root->dispatchKey({.keyCode = keys::kEnter, .action = KeyAction::down});
    h.root->dispatchKey({.keyCode = keys::kEnter, .action = KeyAction::up});
    editor.commitText("world");
    EXPECT(editor.editingValue().text == "hello\nworld" && submits == 0);
    EXPECT(changes.back() == "hello\nworld");
    h.frame();
    // Two lines: the well grew by one.
    EXPECT(test::near(editor.frame().height, 2 * line, 0.5f));
    EXPECT(field->well().frame().height > 48);

    // A tap on the second line puts the caret there.
    const auto topLeft = [&] {
        const gfx::Vec2 c = h.center(editor);
        return gfx::Vec2{c.x - editor.frame().width / 2, c.y - editor.frame().height / 2};
    };
    gfx::Vec2 origin = topLeft();
    h.tap(origin.x + 7.5f * 3 + 1, origin.y + line * 1.5f);
    EXPECT(editor.editingValue().selection == (TextRange{9, 9}));

    // A long unbroken line (a key's base64) wraps; past four lines the field stops growing and
    // keeps the caret in view.
    editor.setText(std::string(400, 'A'));
    h.frame();
    EXPECT(editor.shaped() != nullptr && editor.shaped()->lines.size() > 4);
    EXPECT(test::near(editor.frame().height, 4 * line, 0.5f));
    const gfx::Rect caret = editor.caretRect();
    origin = topLeft();
    const float top = origin.y * ComponentHarness::kDensity;
    const float bottom = (origin.y + editor.frame().height) * ComponentHarness::kDensity;
    EXPECT(caret.y >= top - 0.5f && caret.y + caret.height <= bottom + 0.5f);
    // And a tap at the top lands on the line scrolled into view there, not the first one.
    h.tap(origin.x + 1, origin.y + line * 0.5f);
    const auto byte = editor.editingValue().selection.end;
    EXPECT(byte > 44 && byte % 44 == 0);
}

}  // namespace

int main() {
    testFocusAndIme();
    testAppEditsReachTheIme();
    testHardwareKeys();
    testCaretDragAndSemantics();
    testLongTextScrollsToCaret();
    testPasswordIsMasked();
    testMultiline();
    return 0;
}
