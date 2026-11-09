// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "component_harness.hpp"
#include "filepan/ui/components/text_editor.hpp"
#include "filepan/ui/input.hpp"
#include "test_util.hpp"

using namespace filepan;
using namespace filepan::ui;
using namespace filepan::ui::components;
using filepan::test::ComponentHarness;

namespace {

class FakeIme final : public TextInputPlatform {
public:
    void show(const TextInputConfig &config) override {
        visible = true;
        shown = config;
    }
    void hide() override { visible = false; }
    void updateEditingValue(const TextEditingValue &value) override { last = value; }

    bool visible = false;
    TextInputConfig shown;
    TextEditingValue last;
};

// The fake text system: 13 sp at density 2 is 6.5 dp a character and 18.85 dp a line; the
// gutter is 40 dp.
constexpr float kChar = 6.5f;
constexpr float kLine = 18.85f;
constexpr float kGutter = 40;

struct Fixture {
    ComponentHarness h;
    FakeIme ime;
    std::vector<std::string> copied;
    std::optional<std::string> clipboard;
    int changes = 0;
    int saves = 0;
    std::shared_ptr<TextEditor> editor;

    explicit Fixture(std::string text, bool readOnly = false, bool wrap = true) {
        h.root->textInput().setPlatform(&ime);
        editor = TextEditor::create(text::TextDocument(std::move(text)),
                                    {.readOnly = readOnly,
                                     .wrap = wrap,
                                     .onChange = [this] { ++changes; },
                                     .onCopy = [this](const std::string &s) { copied.push_back(s); },
                                     .onPaste = [this] { return clipboard; },
                                     .onSave = [this] { ++saves; }});
        auto root = Box::create();
        root->appendChild(editor);
        h.root->setContent(root);
        h.frame();
    }

    [[nodiscard]] const std::string &text() const { return editor->document().text(); }
    [[nodiscard]] TextRange selection() const { return editor->peekSelection(); }
    // The middle of the character before column on line, in window dp.
    void tapAt(std::size_t line, float column) {
        h.tap(kGutter + kChar * column + 1, kLine * static_cast<float>(line) + kLine / 2);
    }
    void key(std::int32_t code, std::uint32_t modifiers = 0, std::uint32_t codePoint = 0) {
        h.root->dispatchKey({.keyCode = code, .action = KeyAction::down, .modifiers = modifiers, .codePoint = codePoint});
        h.root->dispatchKey({.keyCode = code, .action = KeyAction::up, .modifiers = modifiers});
        h.frame();
    }
};

void testTapAndType() {
    Fixture f("hello\nworld\n\tend");
    EXPECT(f.editor->list().itemCount() == 3);
    // Tapping into "world" puts the caret there, and brings up the keyboard.
    f.tapAt(1, 2);
    EXPECT(f.editor->focused() && f.ime.visible && f.ime.shown.multiline);
    EXPECT(f.selection() == (TextRange{8, 8}));
    // The IME types through the window, which here is the whole text.
    TextInputClient &client = *f.h.root->textInput().client();
    EXPECT(client.editingValue().text == f.text() && f.editor->windowStart() == 0);
    client.commitText("XY");
    EXPECT(f.text() == "hello\nwoXYrld\n\tend" && f.selection() == (TextRange{10, 10}) && f.changes == 1);
    client.setComposingText("ab");
    client.finishComposingText();
    EXPECT(f.text() == "hello\nwoXYabrld\n\tend");
    // The next frame tells the IME where things are.
    f.h.frame();
    EXPECT(f.ime.last.text == f.text() && f.ime.last.selection == (TextRange{12, 12}));
}

void testKeys() {
    Fixture f("one\ntwo\nthree");
    f.editor->requestFocus();
    f.editor->select(6, 6);  // tw|o
    f.key(keys::kEnter);
    EXPECT(f.text() == "one\ntw\no\nthree" && f.editor->list().itemCount() == 4 && f.selection() == (TextRange{7, 7}));
    f.key(keys::kDel);
    EXPECT(f.text() == "one\ntwo\nthree" && f.editor->list().itemCount() == 3);
    // Up keeps the column; down past a shorter line comes back to it.
    f.key(keys::kDpadUp);
    EXPECT(f.selection() == (TextRange{2, 2}));
    f.key(keys::kDpadDown);
    f.key(keys::kDpadDown);
    EXPECT(f.selection() == (TextRange{10, 10}));
    f.key(keys::kMoveHome);
    f.key(keys::kMoveEnd, modifiers::kShift);
    EXPECT(f.selection() == (TextRange{8, 13}));
    f.key(0, 0, 'X');
    EXPECT(f.text() == "one\ntwo\nX");
    f.key(keys::kTab);
    EXPECT(f.text() == "one\ntwo\nX\t");
    // Shortcuts: all, copy, paste over it, save.
    f.key(keys::kA, modifiers::kCtrl);
    f.key(keys::kC, modifiers::kCtrl);
    EXPECT(f.copied.size() == 1 && f.copied.back() == "one\ntwo\nX\t");
    f.clipboard = "pasted\r\nlines";
    f.key(keys::kV, modifiers::kCtrl);
    EXPECT(f.text() == "pasted\nlines" && f.editor->list().itemCount() == 2);
    f.key(keys::kS, modifiers::kCtrl);
    EXPECT(f.saves == 1);
    // Multi-byte characters delete whole.
    f.editor->replaceSelection("\xC3\xA9");
    f.key(keys::kDel);
    EXPECT(f.text() == "pasted\nlines");
}

void testLongPressSelectsAWord() {
    Fixture f("alpha beta_gamma delta");
    f.h.down(kGutter + kChar * 8, kLine / 2);
    f.h.advance(700);
    EXPECT(f.selection() == (TextRange{6, 16}));
    // Dragging on extends it.
    f.h.move(kGutter + kChar * 20 + 1, kLine / 2);
    f.h.frame();
    EXPECT(f.selection() == (TextRange{6, 20}));
    f.h.up(kGutter + kChar * 20 + 1, kLine / 2);
    f.editor->copy();
    EXPECT(f.copied.back() == "beta_gamma del");
}

void testReadOnly() {
    Fixture f("look\nonly", true);
    f.tapAt(1, 1);
    EXPECT(f.editor->focused() && !f.ime.visible && f.h.root->textInput().client() == nullptr);
    f.key(0, 0, 'X');
    f.key(keys::kDel);
    EXPECT(f.text() == "look\nonly");
    f.key(keys::kA, modifiers::kCtrl);
    f.key(keys::kX, modifiers::kCtrl);
    EXPECT(f.text() == "look\nonly" && f.copied.empty());
    f.key(keys::kC, modifiers::kCtrl);
    EXPECT(f.copied.size() == 1 && f.copied.back() == "look\nonly");
}

// A long file: the IME sees a window around the caret, and its edits land in the right place.
void testWindow() {
    std::string text;
    for (int i = 0; i < 20000; ++i) {
        text += "line " + std::to_string(i) + "\n";
    }
    Fixture f(text);
    EXPECT(f.editor->list().itemCount() == 20001);
    f.editor->requestFocus();
    const std::uint32_t middle = f.editor->document().lineStart(10000);
    f.editor->select(middle, middle);
    TextInputClient &client = *f.h.root->textInput().client();
    EXPECT(client.editingValue().text.size() <= 8200 && f.editor->windowStart() > 0);
    EXPECT(client.editingValue().text.substr(static_cast<std::size_t>(client.editingValue().selection.start), 10) ==
           "line 10000");
    client.commitText(">");
    EXPECT(f.editor->document().line(10000) == ">line 10000" && f.editor->document().lineCount() == 20001);
    client.deleteSurroundingText(1, 0);
    EXPECT(f.editor->document().line(10000) == "line 10000");
    // Walking far from the window moves it, and the IME hears.
    f.editor->select(10, 10);
    f.h.frame();
    EXPECT(f.editor->windowStart() == 0 && f.ime.last.selection == (TextRange{10, 10}));
    // The caret's line comes into view.
    f.editor->revealOffset(middle);
    f.h.frame();
    EXPECT(f.editor->list().boundRange().first <= 10000 && 10000 < f.editor->list().boundRange().second);
}

void testWrap() {
    Fixture f(std::string(100, 'w') + "\nshort");
    f.h.text.wrapLines = true;
    f.editor->setWrap(false);
    f.editor->setWrap(true);
    f.h.frame();
    // 100 characters at 6.5 dp in about 350 dp wrap onto two lines.
    Node *row = f.editor->list().nodeFor(0);
    EXPECT(row != nullptr && row->frame().height > kLine * 1.5f);
    // Tapping on the second visual line lands past the first one's characters.
    f.h.tap(kGutter + kChar * 2 + 1, kLine * 1.5f);
    EXPECT(f.selection().end > 40 && f.selection().end < 100);
}

}  // namespace

int main() {
    testTapAndType();
    testKeys();
    testLongPressSelectsAWord();
    testReadOnly();
    testWindow();
    testWrap();
    return 0;
}
