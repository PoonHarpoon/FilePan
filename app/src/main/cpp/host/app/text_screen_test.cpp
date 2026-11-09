// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// The text editor in the app: opening a text file from the list, editing and saving it, back with
// unsaved changes, a file changed behind its back, binary and read-only files, and "Open as text".

#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>

#include "app_harness.hpp"
#include "filepan/app/text_screen.hpp"

using namespace filepan;
using namespace filepan::test;
namespace fs = std::filesystem;

namespace {

// Docs/: notes.txt, crlf.txt (Windows line ends), data.bin (not text), big.log (over the edit limit).
struct Tree {
    TempDir dir;
    fs::path home;
    fs::path files;

    Tree() {
        home = dir.path() / "storage";
        files = dir.path() / "files";
        fs::create_directories(home / "Docs");
        fs::create_directories(files);
        write("notes.txt", "first line\nsecond line\n");
        write("crlf.txt", "a\r\nb\r\n");
        write("data.bin", std::string("\x7f" "ELF\0\0\0\0", 8));
        // Over the edit limit, in lines of 100.
        std::string big;
        while (big.size() <= app::kMaxTextEdit) {
            big += std::string(99, 'x') + "\n";
        }
        write("big.log", big);
    }

    void write(const std::string &name, const std::string &content) const {
        std::ofstream(home / "Docs" / name, std::ios::binary) << content;
    }
    [[nodiscard]] std::string read(const std::string &name) const {
        std::ifstream in(home / "Docs" / name, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), {}};
    }
    [[nodiscard]] std::string path(const std::string &relative = {}) const {
        return relative.empty() ? home.string() : (home / relative).string();
    }
};

app::TextScreen &open(AppHarness &h, const std::string &name) {
    h.state().navigate(vfs::Location::local(h.app->context().services.homePath + "/Docs"));
    h.settle(100);
    ui::Node *row = h.rowFor(name);
    EXPECT(row != nullptr);
    h.tap(*row);
    EXPECT(h.waitUntil([&] {
        return h.app->textScreen() != nullptr && h.app->textScreen()->status() != app::TextScreen::Status::loading;
    }));
    return *h.app->textScreen();
}

ui::Node *findNode(ui::Node &root, const std::function<bool(ui::Node &)> &pred) {
    if (!root.style().display) {
        return nullptr;
    }
    if (pred(root)) {
        return &root;
    }
    for (const auto &child : root.children()) {
        if (ui::Node *found = findNode(*child, pred)) {
            return found;
        }
    }
    return nullptr;
}

ui::components::Dialog *dialog(AppHarness &h) {
    ui::Node *found = findNode(*h.app->root(), [](ui::Node &n) { return n.debugName() == "Dialog"; });
    return static_cast<ui::components::Dialog *>(found);
}

ui::Node *button(ui::Node &root, std::string_view label) {
    return findNode(root, [&](ui::Node &n) { return n.debugName() == "Button" && n.semantics().label == label; });
}

void testEditAndSave() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    app::TextScreen &screen = open(h, "notes.txt");
    EXPECT(screen.status() == app::TextScreen::Status::ready && h.platform.opened.empty());
    auto *editor = screen.editor();
    EXPECT(editor != nullptr && editor->document().text() == "first line\nsecond line\n" && !editor->readOnly());
    EXPECT(screen.subtitle().text() == "UTF-8 · LF · 3 lines");
    // Typing marks it modified; save writes it and clears that.
    editor->select(5, 5);
    editor->replaceSelection(",");
    h.pump(30);
    EXPECT(screen.modified() && screen.subtitle().text().ends_with("Modified"));
    screen.save();
    EXPECT(h.waitUntil([&] { return !screen.saving(); }));
    EXPECT(tree.read("notes.txt") == "first, line\nsecond line\n" && !screen.modified());
    // Back with nothing unsaved closes it.
    EXPECT(h.app->back());
    h.pump(30);
    EXPECT(h.app->textScreen() == nullptr);
}

void testBackAsksToSave() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    app::TextScreen &screen = open(h, "crlf.txt");
    EXPECT(screen.subtitle().text() == "UTF-8 · CRLF · 3 lines");
    screen.editor()->select(1, 1);
    screen.editor()->replaceSelection("!");
    h.pump(30);
    EXPECT(h.app->back());
    h.pump(300);
    EXPECT(h.app->textScreen() != nullptr && dialog(h) != nullptr);
    // Save, and it closes once saved, with the line ends it came with.
    h.tap(*button(dialog(h)->card(), "Save"));
    EXPECT(h.waitUntil([&] { return h.app->textScreen() == nullptr; }));
    EXPECT(tree.read("crlf.txt") == "a!\r\nb\r\n");

    // Discard closes without writing.
    app::TextScreen &again = open(h, "crlf.txt");
    again.editor()->replaceSelection("?");
    h.pump(30);
    h.app->back();
    h.pump(300);
    h.tap(*button(dialog(h)->card(), "Discard"));
    EXPECT(h.waitUntil([&] { return h.app->textScreen() == nullptr; }));
    EXPECT(tree.read("crlf.txt") == "a!\r\nb\r\n");
}

void testConflict() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    app::TextScreen &screen = open(h, "notes.txt");
    screen.editor()->replaceSelection("mine ");
    // Something else writes the file meanwhile.
    tree.write("notes.txt", "theirs, which is longer\n");
    screen.save();
    EXPECT(h.waitUntil([&] { return !screen.saving() && dialog(h) != nullptr; }));
    EXPECT(tree.read("notes.txt") == "theirs, which is longer\n" && screen.modified());
    h.pump(300);
    h.tap(*button(dialog(h)->card(), "Overwrite"));
    EXPECT(h.waitUntil([&] { return !screen.saving() && !screen.modified(); }));
    EXPECT(tree.read("notes.txt") == "mine first line\nsecond line\n");
}

void testBinaryAndReadOnly() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    // Not text, opened as text through the menu: it says so.
    h.state().navigate(vfs::Location::local(tree.path("Docs")));
    h.settle(100);
    app::FileTargets bin{vfs::Location::local(tree.path("Docs")), {}};
    for (const auto &entry : h.browse().model().visible()) {
        if (entry.name == "data.bin") {
            bin.entries.push_back(entry);
        }
    }
    h.app->context().openText(bin);
    EXPECT(h.waitUntil([&] { return h.app->textScreen()->status() == app::TextScreen::Status::failed; }));
    EXPECT(h.app->textScreen()->errorMessage() == "It isn't a text file.");
    h.app->back();
    h.pump(30);
    // Too big to edit: read-only, no keyboard.
    app::TextScreen &big = open(h, "big.log");
    EXPECT(big.status() == app::TextScreen::Status::ready && big.editor()->readOnly());
    EXPECT(big.subtitle().text().ends_with("Read only"));
}

}  // namespace

int main() {
    testEditAndSave();
    testBackAsksToSave();
    testConflict();
    testBinaryAndReadOnly();
    return 0;
}
