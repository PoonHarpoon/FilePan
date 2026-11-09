// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// The file operations end to end on a temporary tree, through the UI where it matters: copy and
// cut with paste, delete, rename and its validation, conflicts and errors as sheets, cancelling,
// the data band, properties with a recursive count and checksums, extracting and compressing,
// sharing and opening files that aren't local, saving what other apps share, and the jobs
// notification's throttling.

#include <archive.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include "app_harness.hpp"
#include "filepan/app/file_dialogs.hpp"
#include "filepan/app/file_operations.hpp"
#include "filepan/app/format.hpp"
#include "filepan/app/text_screen.hpp"
#include "filepan/app/jobs_ui.hpp"
#include "filepan/app/properties_sheet.hpp"
#include "filepan/app/prompt_sheets.hpp"
#include "filepan/checksum.hpp"
#include "filepan/create_archive.hpp"
#include "filepan/runtime/shared_items.hpp"
#include "filepan/unique_fd.hpp"

using namespace filepan;
using namespace filepan::test;
namespace fs = std::filesystem;

namespace {

struct Tree {
    TempDir dir;
    fs::path home;
    fs::path files;

    Tree() {
        home = dir.path() / "storage";
        files = dir.path() / "files";
        fs::create_directories(files);
        makeFile(home / "Download" / "Papers" / "a.pdf", 2'400'000, 2026, 10, 12);
        makeFile(home / "Download" / "Papers" / "deep" / "b.txt", 1'000, 2026, 10, 12);
        makeFile(home / "Download" / "file1.txt", 10, 2026, 9, 1);
        makeFile(home / "Download" / "file2.txt", 300, 2026, 9, 3);
        makeFile(home / "Download" / "apple.md", 18'000, 2026, 10, 8);
        std::ofstream(home / "Download" / "notes.txt") << "The quick brown fox jumps over the lazy dog";
        makeFolder(home / "Music", 2026, 8, 1);
        makeFolder(home / "Download", 2026, 10, 3);
    }

    [[nodiscard]] std::string path(const std::string &relative = {}) const {
        return relative.empty() ? home.string() : (home / relative).string();
    }
};

vfs::Location local(const std::string &path) { return vfs::Location::local(path); }

bool contains(const std::vector<std::string> &names, std::string_view name) {
    return std::ranges::find(names, name) != names.end();
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

ui::Node *byLabel(ui::Node &root, std::string_view label) {
    return findNode(root, [&](ui::Node &n) { return n.semantics().label == label || n.semanticsText() == label; });
}

ui::Node *byName(ui::Node &root, std::string_view name) {
    return findNode(root, [&](ui::Node &n) { return n.debugName() == name; });
}

ui::Node *textContaining(ui::Node &root, std::string_view text) {
    return findNode(root, [&](ui::Node &n) {
        return n.children().empty() && n.semanticsText().find(text) != std::string_view::npos;
    });
}

app::FileOperations &ops(AppHarness &h) { return static_cast<app::FileOperations &>(h.app->context().actions); }

app::FileTargets targets(AppHarness &h, std::vector<std::string> names) {
    h.state().setSelection(std::move(names));
    return h.browse().selectionTargets();
}

void openFolder(AppHarness &h, const std::string &path) {
    h.state().navigate(local(path));
    h.settle(100);
}

ui::Node *findButton(ui::Node &root, std::string_view label) {
    return findNode(root, [&](ui::Node &n) { return n.debugName() == "Button" && n.semantics().label == label; });
}

// The name dialog's "Replace existing" switch, while it's shown.
ui::Node *replaceSwitch(ui::Node &root) {
    return findNode(root, [](ui::Node &n) { return n.debugName() == "Switch" && n.semantics().label == "Replace existing"; });
}


// Waits for a snackbar, skipping the ones queued before it.
bool waitSnackbar(AppHarness &h, std::string_view text) {
    return h.waitUntil([&] {
        auto &host = h.app->context().overlays.snackbars();
        if (host.current() && host.current()->text != text) {
            host.dismissCurrent();
        }
        return host.current() && host.current()->text == text;
    });
}

bool idle(AppHarness &h) {
    return std::ranges::none_of(ops(h).jobs().list.peek(),
                                [](const app::JobView &v) { return !jobs::isFinished(v.progress.state); });
}

// The top sheet once it has slid in.
ui::components::BottomSheet *sheet(AppHarness &h, std::string_view contentName = {}) {
    ui::components::BottomSheet *top = nullptr;
    h.waitUntil([&] {
        top = h.app->context().overlays.topSheet();
        return top != nullptr && (contentName.empty() || byName(top->panel(), contentName) != nullptr);
    });
    h.pump(450);
    return top;
}

ui::components::Dialog *dialog(AppHarness &h) {
    ui::Node *found = nullptr;
    h.waitUntil([&] {
        found = byName(*h.app->root(), "Dialog");
        return found != nullptr;
    });
    h.pump(300);
    return static_cast<ui::components::Dialog *>(found);
}

void type(ui::components::EditableText &editor, const std::string &text) {
    editor.updateEditingValue({.text = text, .selection = {static_cast<std::int32_t>(text.size()),
                                                           static_cast<std::int32_t>(text.size())}});
}

void testCopyPaste() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    openFolder(h, tree.path("Download"));
    auto &actions = ops(h);
    actions.copy(targets(h, {"Papers", "apple.md"}));
    h.pump(30);
    // The selection makes way for the clipboard, which the dock shows as a stamp.
    EXPECT(h.state().peekActive()->selection.peek().empty());
    EXPECT(actions.canPaste() && actions.clipboard.peek().items.size() == 2 && !actions.clipboard.peek().cut);
    ui::Node *stamp = byName(h.browse().dock().idleRow(), "ClipboardStamp");
    EXPECT(stamp != nullptr && byLabel(*stamp, "Paste 2 items here") != nullptr);
    // Paste in Music by tapping it.
    openFolder(h, tree.path("Music"));
    h.pump(400);
    h.tap(*byLabel(*stamp, "Paste 2 items here"));
    EXPECT(h.waitUntil([&] { return contains(h.names(), "Papers") && contains(h.names(), "apple.md"); }));
    EXPECT(h.waitUntil([&] { return idle(h); }));
    EXPECT(fs::file_size(tree.home / "Music" / "Papers" / "deep" / "b.txt") == 1'000);
    EXPECT(fs::exists(tree.home / "Download" / "apple.md"));
    EXPECT(waitSnackbar(h, "Copied 2 items"));
    // The next copy starting clears what was showing: the band shows that one now.
    h.app->context().overlays.showSnackbar({.text = "Old news"});
    h.pump(300);
    actions.paste(local(tree.path()));
    EXPECT(!h.app->context().overlays.snackbars().current());
    EXPECT(h.waitUntil([&] { return idle(h); }));
    // The service started and stopped.
    EXPECT(!h.platform.active.empty() && h.platform.active.front() && !h.platform.active.back());
    // A copy stays on the clipboard; the × clears it.
    EXPECT(actions.canPaste());
    h.tap(*byLabel(*stamp, "Clear the clipboard"));
    EXPECT(!actions.canPaste() && !stamp->style().display);
}

void testCutPaste() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    openFolder(h, tree.path("Download"));
    auto &actions = ops(h);
    actions.cut(targets(h, {"file1.txt", "file2.txt"}));
    EXPECT(actions.clipboard.peek().cut);
    openFolder(h, tree.path("Music"));
    actions.paste(local(tree.path("Music")));
    EXPECT(h.waitUntil([&] { return h.names() == std::vector<std::string>{"file1.txt", "file2.txt"}; }));
    EXPECT(h.waitUntil([&] { return idle(h); }));
    EXPECT(!fs::exists(tree.home / "Download" / "file1.txt") && fs::file_size(tree.home / "Music" / "file2.txt") == 300);
    // A move empties the clipboard.
    EXPECT(!actions.canPaste());
    // Back in Download, they're gone from the list.
    h.state().back();
    h.settle(100);
    EXPECT(!contains(h.names(), "file1.txt"));
    // A folder can't go into itself.
    actions.copy(targets(h, {"Papers"}));
    actions.paste(local(tree.path("Download/Papers/deep")));
    EXPECT(waitSnackbar(h, "Can't paste “Papers” into itself"));
}

void testDelete() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    openFolder(h, tree.path("Download"));
    ops(h).remove(targets(h, {"Papers", "apple.md"}));
    auto *confirm = sheet(h, "DeleteSheet");
    EXPECT(confirm != nullptr);
    // It names what goes.
    EXPECT(byLabel(confirm->panel(), "Delete 2 items?") != nullptr);
    EXPECT(textContaining(confirm->panel(), "Papers") != nullptr && textContaining(confirm->panel(), "apple.md") != nullptr);
    // Cancel keeps them.
    h.tap(*findButton(confirm->panel(), "Cancel"));
    EXPECT(h.waitUntil([&] { return h.app->context().overlays.topSheet() == nullptr; }));
    EXPECT(fs::exists(tree.home / "Download" / "apple.md"));
    ops(h).remove(targets(h, {"Papers", "apple.md"}));
    confirm = sheet(h, "DeleteSheet");
    h.tap(*byLabel(confirm->panel(), "Delete 2 items"));
    EXPECT(h.waitUntil([&] { return !contains(h.names(), "apple.md") && !contains(h.names(), "Papers"); }));
    EXPECT(!fs::exists(tree.home / "Download" / "Papers"));
    EXPECT(h.state().peekActive()->selection.peek().empty());
    EXPECT(waitSnackbar(h, "Deleted 2 items"));
}

// What another app shares: an open file, as the shell hands it over.
jobs::ImportItem sharedFile(const fs::path &path, std::string name) {
    jobs::ImportItem item;
    item.name = std::move(name);
    item.size = fs::file_size(path);
    item.fd = UniqueFd(open(path.c_str(), O_RDONLY | O_CLOEXEC));
    EXPECT(static_cast<bool>(item.fd));
    return item;
}

void testShareSave() {
    // Writing to a pipe nobody reads fails instead of killing the test.
    signal(SIGPIPE, SIG_IGN);
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    openFolder(h, tree.path("Download"));
    auto &actions = ops(h);
    runtime::SharedItems share;
    share.items.push_back(sharedFile(tree.home / "Download" / "notes.txt", "photo.jpg"));
    share.fromOtherApp = true;
    h.services->shares.deliver(std::move(share));
    h.pump(30);
    // The dock offers to save it, next to nothing else.
    EXPECT(actions.shared.peek() == std::vector<std::string>{"photo.jpg"});
    ui::Node *stamp = byName(h.browse().dock().idleRow(), "ShareStamp");
    EXPECT(stamp != nullptr && byLabel(*stamp, "Save “photo.jpg” here") != nullptr);
    EXPECT(byName(h.browse().dock().idleRow(), "ClipboardStamp") == nullptr);
    EXPECT(waitSnackbar(h, "“photo.jpg” to save · open a folder and save here"));

    // Not into a folder that can't be written.
    actions.saveShared(vfs::Location::archive(local(tree.path("Download/a.zip")), "/"));
    EXPECT(waitSnackbar(h, "Can't save here: this folder is read-only"));
    EXPECT(byName(*h.app->root(), "Dialog") == nullptr && !actions.shared.peek().empty());

    // Into Music, under a name of the user's choice.
    openFolder(h, tree.path("Music"));
    h.pump(400);
    h.tap(*byLabel(*stamp, "Save “photo.jpg” here"));
    auto *d = dialog(h);
    EXPECT(d != nullptr);
    auto &editor = static_cast<ui::components::TextField &>(*byName(d->card(), "TextField")).editor();
    EXPECT(editor.editingValue().text == "photo.jpg" && editor.editingValue().selection == (ui::TextRange{0, 5}));
    type(editor, "fox.txt");
    editor.performAction(ui::ImeAction::done);
    EXPECT(h.waitUntil([&] { return contains(h.names(), "fox.txt"); }));
    EXPECT(h.waitUntil([&] { return idle(h); }));
    std::ifstream saved(tree.home / "Music" / "fox.txt");
    EXPECT(std::string(std::istreambuf_iterator<char>(saved), {}) == "The quick brown fox jumps over the lazy dog");
    EXPECT(waitSnackbar(h, "Saved “fox.txt” to Music"));
    // Then back to the app that shared it.
    EXPECT(h.platform.finishes == 1);
    EXPECT(actions.shared.peek().empty() && !stamp->style().display);

    // Over a file that's there, with the dialog's switch.
    runtime::SharedItems over;
    over.items.push_back({.name = "fox.txt", .text = "a new fox"});
    h.services->shares.deliver(std::move(over));
    h.pump(30);
    h.tap(*byLabel(*stamp, "Save “fox.txt” here"));
    d = dialog(h);
    EXPECT(d != nullptr && h.waitUntil([&] { return replaceSwitch(d->card()) != nullptr; }));
    h.tap(*replaceSwitch(d->card()));
    h.pump(30);
    h.tap(*findButton(d->card(), "Save"));
    EXPECT(h.waitUntil([&] { return idle(h) && fs::file_size(tree.home / "Music" / "fox.txt") == 9; }));
    EXPECT(h.app->context().overlays.topSheet() == nullptr);

    // Several keep their names, and a name that's taken asks. Shared from FilePan itself, it stays.
    runtime::SharedItems several;
    several.items.push_back({.name = "fox.txt", .text = "a newer fox"});
    several.items.push_back({.name = "list.txt", .text = "eggs"});
    h.services->shares.deliver(std::move(several));
    h.pump(30);
    h.tap(*byLabel(*stamp, "Save 2 items here"));
    auto *conflict = sheet(h, "ConflictSheet");
    EXPECT(conflict != nullptr && textContaining(conflict->panel(), "is already in “Music”") != nullptr);
    h.tap(*findButton(conflict->panel(), "Keep both"));
    EXPECT(h.waitUntil([&] { return idle(h); }));
    EXPECT(fs::file_size(tree.home / "Music" / "fox (1).txt") == 11 && fs::file_size(tree.home / "Music" / "list.txt") == 4);
    EXPECT(waitSnackbar(h, "Saved 2 items to Music"));
    EXPECT(h.platform.finishes == 1);

    // × drops a share, closing its files.
    int ends[2];
    EXPECT(pipe2(ends, O_CLOEXEC) == 0);
    const UniqueFd writer(ends[1]);
    runtime::SharedItems dropped;
    dropped.items.push_back({.name = "stream.bin", .fd = UniqueFd(ends[0])});
    h.services->shares.deliver(std::move(dropped));
    h.pump(30);
    h.tap(*byLabel(*stamp, "Don't save the shared items"));
    EXPECT(actions.shared.peek().empty() && !stamp->style().display);
    EXPECT(write(writer.get(), "x", 1) == -1 && errno == EPIPE);

    // So does leaving FilePan with back, but not going back a folder.
    runtime::SharedItems left;
    left.items.push_back({.name = "left.txt", .text = "x"});
    h.services->shares.deliver(std::move(left));
    h.pump(30);
    EXPECT(h.root->dispatchBack());
    h.settle(100);
    EXPECT(actions.shared.peek() == std::vector<std::string>{"left.txt"});
    for (int i = 0; i < 10 && h.root->dispatchBack(); ++i) {
        h.settle(100);
    }
    EXPECT(actions.shared.peek().empty());
}

void testDocumentCreate() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    openFolder(h, tree.path("Download"));
    auto &actions = ops(h);
    auto &answers = h.platform.answers;
    const auto request = [&](std::string name) {
        h.services->documentRequests.deliver(
                {.kind = runtime::DocumentRequest::Kind::create, .name = std::move(name), .mimeTypes = {"application/pdf"}});
        h.pump(30);
    };
    // Another app asks for a document; the dock offers to save it, as it does a share.
    request("statement.pdf");
    EXPECT(actions.shared.peek() == std::vector<std::string>{"statement.pdf"});
    ui::Node *stamp = byName(h.browse().dock().idleRow(), "ShareStamp");
    EXPECT(stamp != nullptr && byLabel(*stamp, "Save “statement.pdf” here") != nullptr);
    EXPECT(waitSnackbar(h, "“statement.pdf” to save · open a folder and save here"));

    // Only into a folder on this device: the app writes the file itself.
    actions.saveShared(vfs::Location::archive(local(tree.path("Download/a.zip")), "/"));
    EXPECT(waitSnackbar(h, "Can't save here: choose a folder on this device"));
    EXPECT(byName(*h.app->root(), "Dialog") == nullptr && answers.empty());

    // An empty file under the name chosen, handed to the app to write.
    h.tap(*byLabel(*stamp, "Save “statement.pdf” here"));
    auto *d = dialog(h);
    EXPECT(d != nullptr);
    h.tap(*findButton(d->card(), "Save"));
    const fs::path saved = tree.home / "Download" / "statement.pdf";
    EXPECT(h.waitUntil([&] { return idle(h) && answers.size() == 1; }));
    EXPECT(fs::is_regular_file(saved) && fs::file_size(saved) == 0);
    EXPECT(answers[0].paths == std::vector<std::string>{saved.string()} &&
           answers[0].mimeTypes == std::vector<std::string>{"application/pdf"} && answers[0].writable);
    EXPECT(actions.shared.peek().empty() && !stamp->style().display && h.platform.finishes == 0);

    // Over a file that's there, with the dialog's switch: emptied for the app to write.
    { std::ofstream(saved) << "last month"; }
    request("statement.pdf");
    h.tap(*byLabel(*stamp, "Save “statement.pdf” here"));
    d = dialog(h);
    EXPECT(d != nullptr && h.waitUntil([&] { return replaceSwitch(d->card()) != nullptr; }));
    h.tap(*replaceSwitch(d->card()));
    h.pump(30);
    h.tap(*findButton(d->card(), "Save"));
    EXPECT(h.waitUntil([&] { return idle(h) && answers.size() == 2; }));
    EXPECT(fs::file_size(saved) == 0 && answers[1].paths == std::vector<std::string>{saved.string()});

    // × tells the app it was cancelled; so does a share, which takes the stamp.
    request("a.pdf");
    h.tap(*byLabel(*stamp, "Don't save the shared items"));
    EXPECT(answers.size() == 3 && answers[2].paths.empty() && !answers[2].writable);
    EXPECT(actions.shared.peek().empty());
    request("b.pdf");
    runtime::SharedItems share;
    share.items.push_back({.name = "x.txt", .text = "x"});
    h.services->shares.deliver(std::move(share));
    h.pump(30);
    EXPECT(answers.size() == 4 && answers[3].paths.empty());
    EXPECT(actions.shared.peek() == std::vector<std::string>{"x.txt"});

    // An app that goes away takes its request with it, and hears nothing back.
    request("c.pdf");
    h.services->documentRequests.deliver({.kind = runtime::DocumentRequest::Kind::ended});
    h.pump(30);
    EXPECT(actions.shared.peek().empty() && answers.size() == 4);
}

void testDocumentPick() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    openFolder(h, tree.path("Download"));
    auto &answers = h.platform.answers;
    const auto request = [&](std::vector<std::string> types, bool multiple) {
        h.services->documentRequests.deliver(
                {.kind = runtime::DocumentRequest::Kind::pick, .mimeTypes = std::move(types), .multiple = multiple});
        h.pump(30);
    };
    // Another app asks for a text file: the dock says so, and a tap on one picks it instead of
    // opening it.
    request({"text/*"}, false);
    ui::Node *stamp = byName(h.browse().dock().idleRow(), "PickStamp");
    EXPECT(stamp != nullptr && stamp->style().display && byLabel(*stamp, "Choose files to attach") != nullptr);
    EXPECT(waitSnackbar(h, "Choose a file to attach"));
    h.tap(*h.rowFor("notes.txt"));
    EXPECT(h.waitUntil([&] { return answers.size() == 1; }));
    EXPECT(answers[0].paths == std::vector<std::string>{tree.path("Download/notes.txt")} &&
           answers[0].mimeTypes == std::vector<std::string>{"text/plain"} && !answers[0].writable);
    h.pump(30);
    EXPECT(!ops(h).picking.peek() && !stamp->style().display && h.app->textScreen() == nullptr);

    // Not a file of a type it doesn't take.
    request({"application/pdf"}, false);
    h.tap(*h.rowFor("notes.txt"));
    EXPECT(waitSnackbar(h, "“notes.txt” isn't a type the app takes"));
    EXPECT(answers.size() == 1 && ops(h).picking.peek().has_value());

    // An archive opens as a folder to pick from, copied out of it first; unless the app asked for
    // archives.
    ops(h).compress(targets(h, {"notes.txt"}));
    h.tap(*findButton(sheet(h, "CompressSheet")->panel(), "Compress"));
    EXPECT(h.waitUntil([&] { return contains(h.names(), "notes.zip") && idle(h); }));
    h.state().setSelection({});
    request({}, false);
    h.tap(*h.rowFor("notes.zip"));
    EXPECT(h.waitUntil([&] { return h.names() == std::vector<std::string>{"notes.txt"}; }));
    h.settle(100);
    h.tap(*h.rowFor("notes.txt"));
    EXPECT(h.waitUntil([&] { return answers.size() == 2 && idle(h); }));
    EXPECT(answers[1].paths.size() == 1 && answers[1].paths[0].starts_with(tree.files.string() + "/cache/") &&
           fs::file_size(answers[1].paths[0]) == 43);
    openFolder(h, tree.path("Download"));
    request({"application/zip"}, false);
    h.tap(*h.rowFor("notes.zip"));
    EXPECT(h.waitUntil([&] { return answers.size() == 3; }));
    EXPECT(answers[2].paths == std::vector<std::string>{tree.path("Download/notes.zip")});

    // Several, chosen with the selection: Attach takes Share's place in the dock.
    request({}, true);
    h.state().setSelection({"file1.txt", "file2.txt"});
    h.settle(300);
    ui::Node &row = h.browse().dock().selectionRow();
    EXPECT(byLabel(row, "Attach") != nullptr && byLabel(row, "Share") == nullptr);
    h.tap(*byLabel(row, "Attach"));
    EXPECT(h.waitUntil([&] { return answers.size() == 4; }));
    EXPECT((answers[3].paths ==
            std::vector<std::string>{tree.path("Download/file1.txt"), tree.path("Download/file2.txt")}));
    h.settle(300);
    EXPECT(byLabel(row, "Share") != nullptr && byLabel(row, "Attach") == nullptr);

    // × tells the app nothing was chosen; an app that goes away hears nothing back.
    request({}, false);
    h.tap(*byLabel(*stamp, "Don't attach anything"));
    EXPECT(answers.size() == 5 && answers[4].paths.empty() && !ops(h).picking.peek());
    request({}, false);
    h.services->documentRequests.deliver({.kind = runtime::DocumentRequest::Kind::ended});
    h.pump(30);
    EXPECT(answers.size() == 5 && !ops(h).picking.peek());
}

void testRename() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    openFolder(h, tree.path("Download"));
    EXPECT(app::nameStem("data.tar.zst", false) == "data" && app::nameStem(".profile", false) == ".profile" &&
           app::nameStem("v1.2", true) == "v1.2");
    ops(h).rename(targets(h, {"apple.md"}));
    auto *d = dialog(h);
    EXPECT(d != nullptr);
    auto &field = static_cast<ui::components::TextField &>(*byName(d->card(), "TextField"));
    auto &editor = field.editor();
    // The name is there with its stem selected, ready to type over.
    EXPECT(editor.editingValue().text == "apple.md");
    EXPECT(editor.editingValue().selection == (ui::TextRange{0, 5}));
    auto *button = static_cast<ui::components::Button *>(findButton(d->card(), "Rename"));
    EXPECT(button != nullptr);
    const auto error = [&] {
        ui::Node *helper = findNode(field, [](ui::Node &n) {
            return n.children().empty() && n.debugName() != "EditableText" && !n.semanticsText().empty() &&
                   n.semanticsText() != "Name";
        });
        return helper ? std::string(helper->semanticsText()) : std::string();
    };
    type(editor, "");
    h.pump(30);
    EXPECT(error() == "Enter a name" && (button->semantics().flags & ui::semantics_flags::kDisabled));
    type(editor, "a/b");
    h.pump(30);
    EXPECT(error() == "Names can't contain “/”");
    // Taken names, once the folder is listed.
    type(editor, "file1.txt");
    EXPECT(h.waitUntil([&] { return error() == "“file1.txt” already exists"; }));
    // Submitting a bad name does nothing.
    editor.performAction(ui::ImeAction::done);
    h.pump(30);
    EXPECT(byName(*h.app->root(), "Dialog") != nullptr);
    // A taken file can be replaced with the switch, which another name turns back off.
    ui::Node *replace = replaceSwitch(d->card());
    EXPECT(replace != nullptr && (button->semantics().flags & ui::semantics_flags::kDisabled));
    h.tap(*replace);
    h.pump(30);
    EXPECT(error() == "Replaces the existing file" && !(button->semantics().flags & ui::semantics_flags::kDisabled));
    type(editor, "file2.txt");
    h.pump(30);
    EXPECT(error() == "“file2.txt” already exists" && !(replace->semantics().flags & ui::semantics_flags::kChecked));
    // Not a folder.
    type(editor, "Papers");
    h.pump(30);
    EXPECT(error() == "“Papers” already exists" && replaceSwitch(d->card()) == nullptr);
    type(editor, "pear.md");
    h.pump(30);
    EXPECT(error().empty() && !(button->semantics().flags & ui::semantics_flags::kDisabled));
    h.tap(*button);
    EXPECT(h.waitUntil([&] { return contains(h.names(), "pear.md") && !contains(h.names(), "apple.md"); }));
    EXPECT(h.waitUntil([&] { return byName(*h.app->root(), "Dialog") == nullptr; }));
    h.pump(400);

    // Onto a taken name, replacing it without another question.
    ops(h).rename(targets(h, {"pear.md"}));
    d = dialog(h);
    auto &replaceEditor = static_cast<ui::components::TextField &>(*byName(d->card(), "TextField")).editor();
    type(replaceEditor, "file1.txt");
    EXPECT(h.waitUntil([&] { return replaceSwitch(d->card()) != nullptr; }));
    h.tap(*replaceSwitch(d->card()));
    h.pump(30);
    h.tap(*findButton(d->card(), "Rename"));
    EXPECT(h.waitUntil([&] { return idle(h) && !contains(h.names(), "pear.md"); }));
    EXPECT(fs::file_size(tree.home / "Download" / "file1.txt") == 18'000);
    EXPECT(h.app->context().overlays.topSheet() == nullptr);
    h.pump(400);

    // New folder and file.
    ops(h).newFolder(local(tree.path("Download")));
    d = dialog(h);
    auto &folderEditor = static_cast<ui::components::TextField &>(*byName(d->card(), "TextField")).editor();
    EXPECT(folderEditor.editingValue().selection == (ui::TextRange{0, 10}));
    type(folderEditor, "Fresh");
    folderEditor.performAction(ui::ImeAction::done);
    EXPECT(h.waitUntil([&] { return contains(h.names(), "Fresh"); }));
    EXPECT(fs::is_directory(tree.home / "Download" / "Fresh"));
    h.pump(400);
    ops(h).newFile(local(tree.path("Download/Fresh")));
    d = dialog(h);
    auto &fileEditor = static_cast<ui::components::TextField &>(*byName(d->card(), "TextField")).editor();
    EXPECT(fileEditor.editingValue().selection == (ui::TextRange{0, 8}));
    fileEditor.performAction(ui::ImeAction::done);
    EXPECT(h.waitUntil([&] { return fs::is_regular_file(tree.home / "Download" / "Fresh" / "untitled.txt"); }));
}

void testConflictKeepBothApplyToAll() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    openFolder(h, tree.path("Download"));
    // Music has both names already.
    makeFile(tree.home / "Music" / "file1.txt", 5, 2026, 1, 1);
    makeFile(tree.home / "Music" / "file2.txt", 5, 2026, 1, 1);
    ops(h).copy(targets(h, {"file1.txt", "file2.txt"}));
    ops(h).paste(local(tree.path("Music")));
    auto *conflict = sheet(h, "ConflictSheet");
    EXPECT(conflict != nullptr);
    EXPECT(byLabel(conflict->panel(), "File exists") != nullptr);
    EXPECT(textContaining(conflict->panel(), "is already in “Music”") != nullptr);
    // The band says it's waiting.
    EXPECT(app::bandText(ops(h).jobs().list.peek()) == "Copying · waiting for you");
    h.tap(*byLabel(conflict->panel(), "Apply to all"));
    EXPECT(byLabel(conflict->panel(), "Apply to all")->semantics().flags & ui::semantics_flags::kChecked);
    h.tap(*findButton(conflict->panel(), "Keep both"));
    EXPECT(h.waitUntil([&] { return idle(h); }));
    // One question answered both.
    openFolder(h, tree.path("Music"));
    EXPECT(h.waitUntil([&] { return contains(h.names(), "file1 (1).txt") && contains(h.names(), "file2 (1).txt"); }));
    h.pump(400);
    EXPECT(h.services->decisions.pending().empty() && h.app->context().overlays.topSheet() == nullptr);
    EXPECT(fs::file_size(tree.home / "Music" / "file2 (1).txt") == 300 && fs::file_size(tree.home / "Music" / "file2.txt") == 5);
}

void testCancelWhileWaiting() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    openFolder(h, tree.path("Download"));
    makeFile(tree.home / "Music" / "apple.md", 5, 2026, 1, 1);
    ops(h).copy(targets(h, {"apple.md"}));
    ops(h).paste(local(tree.path("Music")));
    auto *conflict = sheet(h, "ConflictSheet");
    EXPECT(conflict != nullptr);
    // Dismissed, it waits; the band brings it back.
    conflict->dismiss();
    h.pump(500);
    EXPECT(h.app->context().overlays.topSheet() == nullptr && h.services->decisions.pending().size() == 1);
    ops(h).onBandTap();
    conflict = sheet(h, "ConflictSheet");
    EXPECT(conflict != nullptr);
    conflict->dismiss();
    h.pump(500);
    // The jobs sheet cancels it, and the question goes with it.
    ops(h).showJobs();
    auto *jobsSheet = sheet(h, "JobsSheet");
    EXPECT(jobsSheet != nullptr);
    EXPECT(byLabel(jobsSheet->panel(), "Decide what to do") != nullptr);
    ui::Node *cancel = findNode(jobsSheet->panel(), [](ui::Node &n) { return n.semantics().label.starts_with("Cancel Copy"); });
    EXPECT(cancel != nullptr);
    h.tap(*cancel);
    EXPECT(h.waitUntil([&] { return idle(h); }));
    EXPECT(ops(h).jobs().list.peek().back().progress.state == jobs::JobState::kCancelled);
    EXPECT(h.services->decisions.pending().empty());
    // Its snackbar waits while the sheet is up. Finished jobs stay in it until dismissed.
    EXPECT(h.app->context().overlays.snackbars().held() && !h.app->context().overlays.snackbars().current());
    h.app->context().overlays.snackbars().dismissCurrent();
    h.pump(300);
    EXPECT(textContaining(jobsSheet->panel(), "Cancelled") != nullptr);
    h.tap(*byLabel(jobsSheet->panel(), "Clear finished"));
    EXPECT(ops(h).jobs().list.peek().empty());
}

void testErrorRetry() {
    if (geteuid() == 0) {
        std::puts("file_operations_test: running as root, skipping the permission error");
        return;
    }
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    openFolder(h, tree.path("Download"));
    fs::permissions(tree.home / "Music", fs::perms::owner_read | fs::perms::owner_exec);
    ops(h).copy(targets(h, {"notes.txt"}));
    ops(h).paste(local(tree.path("Music")));
    auto *error = sheet(h, "ErrorSheet");
    EXPECT(error != nullptr);
    EXPECT(textContaining(error->panel(), "Permission denied") != nullptr);
    // Fixed, retried.
    fs::permissions(tree.home / "Music", fs::perms::owner_all);
    h.tap(*byLabel(error->panel(), "Retry"));
    EXPECT(h.waitUntil([&] { return idle(h); }));
    EXPECT(ops(h).jobs().list.peek().back().progress.state == jobs::JobState::kSucceeded);
    EXPECT(fs::file_size(tree.home / "Music" / "notes.txt") == 43);
}

void testBand() {
    using jobs::JobState;
    const auto view = [](jobs::JobId id, JobState state, std::uint64_t done, std::uint64_t total, double rate) {
        return app::JobView{.id = id,
                            .title = "Copy",
                            .progress = {.id = id,
                                         .kind = jobs::JobKind::kCopy,
                                         .state = state,
                                         .bytesDone = done * 1'000'000,
                                         .bytesTotal = total * 1'000'000,
                                         .itemsDone = done,
                                         .itemsTotal = total,
                                         .bytesPerSecond = rate}};
    };
    EXPECT(app::bandText({view(1, JobState::kRunning, 12, 40, 38'000'000)}) == "Copying 11 MiB / 38 MiB");
    EXPECT(app::bandRate({view(1, JobState::kRunning, 12, 40, 38'000'000)}) == "36 MiB/s");
    EXPECT(app::bandRate({view(1, JobState::kRunning, 12, 40, 0)}).empty());
    EXPECT(app::bandIcon({view(1, JobState::kRunning, 12, 40, 0)}) == "content_copy");
    EXPECT(std::abs(app::bandProgress({view(1, JobState::kRunning, 12, 40, 0)}) - 0.3f) < 1e-4f);
    // Several: summed, finished ones left out.
    EXPECT(app::bandText({view(1, JobState::kRunning, 12, 40, 38'000'000), view(2, JobState::kQueued, 0, 10, 0),
                          view(3, JobState::kSucceeded, 5, 5, 0)}) == "2 jobs · 11 MiB / 48 MiB");
    EXPECT(app::bandText({view(1, JobState::kSucceeded, 1, 1, 0)}).empty());
    EXPECT(app::formatEta(std::chrono::seconds(42)) == "42 s left" && app::formatEta(std::chrono::seconds(3700)) ==
                                                                                 "1 h 2 min left");
    auto detail = view(1, JobState::kRunning, 12, 40, 38'000'000).progress;
    detail.eta = std::chrono::seconds(42);
    EXPECT(app::jobDetail(detail) == "11 MiB / 38 MiB\n12/40 items · 36 MiB/s · 42 s left");
    // Jobs that move no bytes count items.
    auto deleting = view(1, JobState::kRunning, 0, 0, 0);
    deleting.progress.kind = jobs::JobKind::kDelete;
    deleting.progress.itemsDone = 12;
    deleting.progress.itemsTotal = 40;
    EXPECT(app::bandText({deleting}) == "Deleting 12 of 40" && app::jobDetail(deleting.progress) == "12/40 items");

    // The band follows progress as it arrives.
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    openFolder(h, tree.path("Download"));
    auto *band = static_cast<ui::Node *>(byName(h.browse(), "DataBand"));
    EXPECT(band == nullptr);
    jobs::JobProgress progress = view(77, JobState::kRunning, 12, 40, 38'000'000).progress;
    h.services->jobEvents.publish(progress);
    h.pump(30);
    band = byName(h.browse(), "DataBand");
    EXPECT(band != nullptr && band->semantics().label == "File jobs, Copying 11 MiB / 38 MiB, 36 MiB/s" &&
           band->semantics().value == "30%");
    // The list clears it.
    EXPECT(h.browse().list().style().padding.bottom > 150);
    progress.state = JobState::kSucceeded;
    h.services->jobEvents.publish(progress);
    h.pump(30);
    EXPECT(byName(h.browse(), "DataBand") == nullptr);
}

void testProperties() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    openFolder(h, tree.path("Download"));
    ops(h).properties(targets(h, {"Papers"}));
    auto *props = sheet(h, "PropertiesSheet");
    EXPECT(props != nullptr);
    // Counted on the pool: a.pdf and deep/b.txt.
    EXPECT(h.waitUntil([&] { return textContaining(props->panel(), "2.3 MiB · 2,401,000 bytes") != nullptr; }));
    EXPECT(textContaining(props->panel(), "2 files, 1 folder") != nullptr);
    EXPECT(textContaining(props->panel(), "Internal / Download") != nullptr);
    EXPECT(textContaining(props->panel(), "rwx") != nullptr);
    // Tapping the location copies it.
    h.tap(*findNode(props->panel(), [](ui::Node &n) { return n.semantics().label.starts_with("Location, "); }));
    EXPECT(h.platform.clipboard == tree.path("Download"));
    props->dismiss();
    h.pump(500);

    // A file's checksums, on request.
    ops(h).properties(targets(h, {"notes.txt"}));
    props = sheet(h, "PropertiesSheet");
    EXPECT(textContaining(props->panel(), "Text · text/plain") != nullptr);
    h.tap(*byLabel(props->panel(), "Compute checksums"));
    const std::string sha256 = "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592";
    EXPECT(h.waitUntil([&] { return textContaining(props->panel(), sha256) != nullptr; }));
    // The last "Copied" snackbar would be over the row, which is further down the sheet.
    h.app->context().overlays.snackbars().dismissCurrent();
    auto *scroller = static_cast<ui::ScrollNode *>(
            findNode(props->panel(), [](ui::Node &n) { return n.semantics().role == ui::Role::scroll; }));
    EXPECT(scroller != nullptr);
    scroller->scrollTo(scroller->maxOffset());
    h.pump(200);
    h.tap(*findNode(props->panel(), [](ui::Node &n) { return n.semantics().label.starts_with("SHA-256, "); }));
    EXPECT(h.platform.clipboard == sha256);
    // Hashing was quiet: no band, no snackbar of its own.
    EXPECT(ops(h).jobs().list.peek().empty());
    EXPECT(app::formatPermissions(0755) == "rwxr-xr-x" && app::formatPermissions(04755) == "rwsr-xr-x");
}

void testCompressAndExtract() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    openFolder(h, tree.path("Download"));
    // Papers and notes.txt into Download.tar.zst.
    ops(h).compress(targets(h, {"Papers", "notes.txt"}));
    auto *compress = sheet(h, "CompressSheet");
    EXPECT(compress != nullptr);
    auto &format = static_cast<ui::components::SegmentedControl &>(*byName(compress->panel(), "SegmentedControl"));
    h.tap(format.segment(1));
    EXPECT(h.waitUntil([&] { return textContaining(compress->panel(), "Download.tar.zst") != nullptr; }));
    h.tap(*findButton(compress->panel(), "Compress"));
    EXPECT(h.waitUntil([&] { return contains(h.names(), "Download.tar.zst"); }));
    EXPECT(h.waitUntil([&] { return idle(h); }));
    EXPECT(ops(h).jobs().list.peek().back().progress.state == jobs::JobState::kSucceeded);

    // Back out, into a new folder named after it.
    h.state().setSelection({});
    ops(h).extract(targets(h, {"Download.tar.zst"}));
    auto *extract = sheet(h, "ExtractSheet");
    EXPECT(extract != nullptr && textContaining(extract->panel(), "Download / Download") != nullptr);
    h.tap(*findButton(extract->panel(), "Extract"));
    EXPECT(h.waitUntil([&] { return contains(h.names(), "Download") && idle(h); }));
    const fs::path out = tree.home / "Download" / "Download";
    EXPECT(fs::file_size(out / "Papers" / "deep" / "b.txt") == 1'000 && fs::file_size(out / "Papers" / "a.pdf") == 2'400'000);
    std::ifstream text(out / "notes.txt");
    std::string line;
    std::getline(text, line);
    EXPECT(line == "The quick brown fox jumps over the lazy dog");

    // An encrypted zip: the extraction stops at the encrypted entry and the archive password
    // sheet asks, again when the answer is wrong, with no error sheet or snackbar on the way.
    h.pump(500);
    ops(h).compress(targets(h, {"notes.txt"}));
    compress = sheet(h, "CompressSheet");
    auto *password = findNode(compress->panel(), [](ui::Node &n) {
        return n.debugName() == "TextField" && byLabel(n, "Password") != nullptr;
    });
    EXPECT(password != nullptr);
    auto &secret = static_cast<ui::components::TextField *>(password)->editor();
    EXPECT(secret.obscured());
    type(secret, "hunter2");
    h.tap(*findButton(compress->panel(), "Compress"));
    EXPECT(h.waitUntil([&] { return contains(h.names(), "notes.zip") && idle(h); }));
    fs::remove(tree.home / "Download" / "notes.txt");
    h.pump(400);
    ops(h).extract(targets(h, {"notes.zip"}));
    extract = sheet(h, "ExtractSheet");
    // Straight into Download this time.
    h.tap(*byLabel(extract->panel(), "Extract into a new folder"));
    h.tap(*findButton(extract->panel(), "Extract"));
    auto &prompts = h.app->prompts();
    const auto asking = [&](bool retry) {
        return h.waitUntil([&] {
            const auto *request = prompts.current() ? std::get_if<app::PromptSheets::ArchivePassword>(prompts.current())
                                                    : nullptr;
            return request != nullptr && request->request.retry == retry;
        });
    };
    EXPECT(asking(false));
    EXPECT(byName(*h.app->root(), "ErrorSheet") == nullptr);
    h.pump(400);
    prompts.answerSecret("nope");
    EXPECT(asking(true));
    h.pump(400);
    prompts.answerSecret("hunter2");
    EXPECT(h.waitUntil([&] { return contains(h.names(), "notes.txt") && idle(h); }));
    EXPECT(waitSnackbar(h, "Extracted “notes.zip”"));
    EXPECT(fs::file_size(tree.home / "Download" / "notes.txt") == 43);
}

void testVerifyArchive() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    struct Quiet final : CreateArchiveCallbacks {
        bool onProgress(std::uint64_t, std::uint64_t, std::string_view) override { return true; }
        bool onSourceError(std::string_view, int) override { return false; }
    } quiet;
    const fs::path download = tree.home / "Download";
    EXPECT(createArchive((download / "papers.tar.gz").string(), {.format = ARCHIVE_FORMAT_TAR, .filter = ARCHIVE_FILTER_GZIP},
                         {{tree.path("Download/Papers"), "Papers"}}, quiet)
                   .error == 0);
    EXPECT(createArchive((download / "locked.zip").string(),
                         {.format = ARCHIVE_FORMAT_ZIP, .filter = ARCHIVE_FILTER_NONE, .password = "pw"},
                         {{tree.path("Download/notes.txt"), "notes.txt"}}, quiet)
                   .error == 0);
    // The same tar.gz with a wrong CRC-32 in its gzip trailer.
    std::ifstream in(download / "papers.tar.gz", std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    bytes[bytes.size() - 8] = static_cast<char>(bytes[bytes.size() - 8] ^ 1);
    std::ofstream(download / "damaged.tar.gz", std::ios::binary) << bytes;
    openFolder(h, tree.path("Download"));
    EXPECT(h.waitUntil([&] { return contains(h.names(), "damaged.tar.gz"); }));

    // Papers/, a.pdf, deep/ and b.txt.
    ops(h).verify(targets(h, {"papers.tar.gz"}));
    EXPECT(waitSnackbar(h, "“papers.tar.gz” is intact · 4 items"));
    EXPECT(h.state().peekActive()->selection.peek().empty());

    // Damage: the details list it.
    ops(h).verify(targets(h, {"damaged.tar.gz"}));
    EXPECT(waitSnackbar(h, "“damaged.tar.gz” is damaged · 1 problem"));
    h.app->context().overlays.snackbars().current()->onAction();
    auto *details = sheet(h, "ArchiveProblemsSheet");
    EXPECT(details != nullptr && textContaining(details->panel(), "The archive as a whole") != nullptr);
    EXPECT(textContaining(details->panel(), "gzip") != nullptr);
    h.tap(*findButton(details->panel(), "Close"));
    h.pump(500);

    // Encrypted: the archive password sheet asks, as for extracting, then the test runs again.
    ops(h).verify(targets(h, {"locked.zip"}));
    auto &prompts = h.app->prompts();
    EXPECT(h.waitUntil([&] {
        return prompts.current() && std::get_if<app::PromptSheets::ArchivePassword>(prompts.current()) != nullptr;
    }));
    EXPECT(byName(*h.app->root(), "ErrorSheet") == nullptr);
    h.pump(400);
    prompts.answerSecret("pw");
    EXPECT(waitSnackbar(h, "“locked.zip” is intact · 1 item"));

    // Nothing to test.
    ops(h).verify(targets(h, {"notes.txt"}));
    EXPECT(waitSnackbar(h, "Nothing here is an archive"));
}

void testCompressDeletesOriginals() {
    jobs::JobProgress p;
    p.state = jobs::JobState::kSucceeded;
    EXPECT(app::shouldDeleteSourcesAfterCompress(p));
    p.itemsSkipped = 1;
    EXPECT(!app::shouldDeleteSourcesAfterCompress(p));
    p.itemsSkipped = 0;
    for (const auto state : {jobs::JobState::kFailed, jobs::JobState::kCancelled, jobs::JobState::kRunning}) {
        p.state = state;
        EXPECT(!app::shouldDeleteSourcesAfterCompress(p));
    }

    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    openFolder(h, tree.path("Download"));
    // Off by default: the originals stay.
    ops(h).compress(targets(h, {"file1.txt"}));
    auto *first = sheet(h, "CompressSheet");
    EXPECT(first != nullptr);
    h.tap(*findButton(first->panel(), "Compress"));
    EXPECT(h.waitUntil([&] { return contains(h.names(), "file1.zip") && idle(h); }));
    EXPECT(fs::exists(tree.home / "Download" / "file1.txt"));

    // On: the originals go once the archive is done.
    ops(h).compress(targets(h, {"file2.txt", "apple.md"}));
    auto *second = sheet(h, "CompressSheet");
    EXPECT(second != nullptr);
    h.tap(*findNode(second->panel(), [](ui::Node &n) {
        return n.semantics().role == ui::Role::toggleSwitch && n.semantics().label == "Delete originals after compressing";
    }));
    h.tap(*findButton(second->panel(), "Compress"));
    EXPECT(h.waitUntil([&] { return !contains(h.names(), "file2.txt") && !contains(h.names(), "apple.md") && idle(h); }));
    EXPECT(!fs::exists(tree.home / "Download" / "file2.txt") && !fs::exists(tree.home / "Download" / "apple.md"));
    EXPECT(fs::exists(tree.home / "Download" / "Download.zip"));
}

void testShareAndOpen() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    openFolder(h, tree.path("Download"));
    // Local files go straight out; folders can't be shared.
    ops(h).share(targets(h, {"apple.md", "notes.txt", "Papers"}));
    EXPECT(h.platform.shared.size() == 1 && h.platform.shared[0].size() == 2 &&
           h.platform.shared[0][0] == tree.path("Download/apple.md"));
    ops(h).openWith(targets(h, {"notes.txt"}));
    EXPECT(h.platform.opened.back() == std::make_pair(tree.path("Download/notes.txt"), std::string("*/*")));

    // From an archive: copied into the cache first, with a job.
    ops(h).compress(targets(h, {"notes.txt"}));
    auto *compress = sheet(h, "CompressSheet");
    h.tap(*findButton(compress->panel(), "Compress"));
    EXPECT(h.waitUntil([&] { return contains(h.names(), "notes.zip") && idle(h); }));
    h.state().setSelection({});
    h.state().navigate(vfs::Location::archive(local(tree.path("Download/notes.zip"))));
    EXPECT(h.waitUntil([&] { return h.names() == std::vector<std::string>{"notes.txt"}; }));
    h.settle(100);
    const std::size_t before = h.platform.opened.size();
    // Tapped, it opens in the editor, read-only; other apps get a copy.
    h.tap(*h.rowFor("notes.txt"));
    EXPECT(h.waitUntil([&] {
        return h.app->textScreen() != nullptr && h.app->textScreen()->status() == app::TextScreen::Status::ready;
    }));
    EXPECT(h.app->textScreen()->editor()->readOnly() && h.platform.opened.size() == before);
    h.app->back();
    h.pump(30);
    ops(h).open(targets(h, {"notes.txt"}));
    EXPECT(h.waitUntil([&] { return h.platform.opened.size() == before + 1; }));
    const auto [path, mime] = h.platform.opened.back();
    EXPECT(path.starts_with(tree.files.string() + "/cache/open/") && path.ends_with("/notes.txt") && mime == "text/plain");
    EXPECT(fs::file_size(path) == 43);
    // Again: from the cache, no job.
    const std::size_t jobsBefore = ops(h).jobs().list.peek().size();
    ops(h).share(targets(h, {"notes.txt"}));
    EXPECT(h.waitUntil([&] { return h.platform.shared.size() == 2; }));
    EXPECT(h.platform.shared.back() == std::vector<std::string>{path} && ops(h).jobs().list.peek().size() == jobsBefore);
}

std::string readText(const fs::path &path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Document trees (experimental): added through the folder picker onto the home, browsed, copied
// into and out of, a file in one opened, and removed again.
void testDocumentTrees() {
    Tree tree;
    // Stands in for a provider's folder; the fake provider's tree URI is its path.
    const fs::path shared = tree.dir.path() / "shared";
    fs::create_directories(shared / "Albums");
    std::ofstream(shared / "Albums" / "song.txt") << "la la";
    AppHarness h(tree.path(), tree.files.string());
    constexpr std::string_view kAdd = "Add storage, a folder from the system's picker, experimental";
    // Off by default.
    h.state().navigateHome();
    h.settle(100);
    EXPECT(byLabel(*h.app->root(), kAdd) == nullptr);
    h.state().updateSettings([](app::Settings &s) { s.documentTrees = true; });
    h.settle(100);
    ui::Node *add = byLabel(*h.app->root(), kAdd);
    EXPECT(add != nullptr);
    h.tap(*add);
    EXPECT(h.platform.treePicks.size() == 1);
    // Backing out adds nothing; a pick is saved under its name and opened.
    h.treePicker.deliver(h.platform.treePicks.back(), {.status = runtime::PickedTree::Status::cancelled});
    EXPECT(h.state().settings.peek().trees.empty());
    h.tap(*byLabel(*h.app->root(), kAdd));
    h.treePicker.deliver(h.platform.treePicks.back(),
                         {.status = runtime::PickedTree::Status::picked, .uri = shared.string(), .name = "Shared"});
    h.settle(100);
    EXPECT((h.state().settings.peek().trees == std::vector<app::DocumentTree>{{"Shared", shared.string()}}));
    EXPECT(h.waitUntil([&] { return h.names() == std::vector<std::string>{"Albums"}; }));
    EXPECT(h.app->breadcrumb().segmentNodes()[0]->semantics().label == "Shared");

    // A local file copied in, and one copied out.
    openFolder(h, tree.path("Download"));
    ops(h).copy(targets(h, {"notes.txt"}));
    const vfs::Location albums = vfs::Location::document("Shared", "/Albums");
    h.state().navigate(albums);
    h.settle(100);
    ops(h).paste(albums);
    EXPECT(h.waitUntil([&] { return idle(h) && contains(h.names(), "notes.txt"); }));
    EXPECT(readText(shared / "Albums" / "notes.txt") == "The quick brown fox jumps over the lazy dog");
    ops(h).copy(targets(h, {"song.txt"}));
    ops(h).paste(local(tree.path("Music")));
    EXPECT(h.waitUntil([&] { return idle(h) && fs::exists(tree.home / "Music" / "song.txt"); }));
    EXPECT(readText(tree.home / "Music" / "song.txt") == "la la");
    // A move out copies, then deletes what it copied.
    ops(h).cut(targets(h, {"notes.txt"}));
    ops(h).paste(local(tree.path("Music")));
    EXPECT(h.waitUntil([&] { return idle(h) && fs::exists(tree.home / "Music" / "notes.txt"); }));
    EXPECT(!fs::exists(shared / "Albums" / "notes.txt"));

    h.state().navigate(albums);
    EXPECT(h.waitUntil([&] { return h.names() == std::vector<std::string>{"song.txt"}; }));
    h.settle(100);
    const std::size_t before = h.platform.opened.size();
    // A text file opens in the editor, editable; opening it in another app copies it out first,
    // as from a server.
    h.tap(*h.rowFor("song.txt"));
    EXPECT(h.waitUntil([&] {
        return h.app->textScreen() != nullptr && h.app->textScreen()->status() == app::TextScreen::Status::ready;
    }));
    EXPECT(!h.app->textScreen()->editor()->readOnly() && h.app->textScreen()->editor()->document().text() == "la la");
    h.app->back();
    h.pump(30);
    ops(h).open(targets(h, {"song.txt"}));
    EXPECT(h.waitUntil([&] { return h.platform.opened.size() == before + 1; }));
    EXPECT(h.platform.opened.back().first.starts_with(tree.files.string() + "/cache/open/"));

    // Removing it from the home gives up the grant, and leaves the folder alone.
    h.state().navigateHome();
    h.settle(100);
    ui::Node *row = byLabel(*h.app->root(), "Shared, system storage");
    EXPECT(row != nullptr);
    h.longPress(h.center(*row));
    h.settle(100);
    EXPECT(h.state().settings.peek().trees.empty());
    EXPECT((h.platform.releasedTrees == std::vector<std::string>{shared.string()}));
    EXPECT(fs::exists(shared / "Albums" / "song.txt"));
    EXPECT(byLabel(*h.app->root(), "Shared, system storage") == nullptr);
}

void testNotificationThrottle() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    auto &notifier = *h.notifier;
    jobs::JobProgress p{.id = 500, .kind = jobs::JobKind::kCopy, .state = jobs::JobState::kRunning, .itemsTotal = 1000};
    // Updates every 10 ms for 1.2 s: at most one notification per 500 ms (and the first at once).
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1200)) {
        ++p.itemsDone;
        notifier.onProgress(p);
        h.pump(10);
    }
    h.pump(600);
    EXPECT(h.platform.active == std::vector<bool>{true});
    EXPECT(h.platform.notifications.size() >= 3 && h.platform.notifications.size() <= 4);
    EXPECT(h.platform.notifications.front().title == "Copying 1000 items");
    // The last one is current.
    EXPECT(h.platform.notifications.back().progressPermille ==
           static_cast<std::int32_t>(p.itemsDone * 1000 / p.itemsTotal));
    p.state = jobs::JobState::kSucceeded;
    notifier.onProgress(p);
    EXPECT(h.platform.active == (std::vector<bool>{true, false}));
}

}  // namespace

// Favorites (and hidden items) follow FilePan's own renames, moves and deletes.
void testUserItemsFollow() {
    Tree tree;
    AppHarness h(tree.path(), tree.files.string());
    openFolder(h, tree.path("Download"));
    using Kind = app::Favorite::Kind;
    auto &state = h.state();
    state.addFavorite({"Papers", local(tree.path("Download/Papers"))});
    state.addFavorite({"b.txt", local(tree.path("Download/Papers/deep/b.txt")), Kind::file});
    state.addFavorite({"Notes", local(tree.path("Download/apple.md")), Kind::file});
    state.addFavorite({"file1.txt", local(tree.path("Download/file1.txt")), Kind::file});
    state.setHidden({local(tree.path("Download/Papers/deep")), local(tree.path("Download/file1.txt"))}, true);
    // Shown, so the selection can take it.
    state.updateSettings([](app::Settings &s) { s.showHidden = true; });
    const auto at = [&](std::size_t i) { return state.settings.peek().favorites[i].location; };

    // Rename: the folder and what's inside it follow; its name follows too.
    ops(h).rename(targets(h, {"Papers"}));
    auto *d = dialog(h);
    auto &editor = static_cast<ui::components::TextField &>(*byName(d->card(), "TextField")).editor();
    type(editor, "Docs");
    editor.performAction(ui::ImeAction::done);
    EXPECT(h.waitUntil([&] { return at(0) == local(tree.path("Download/Docs")); }));
    EXPECT(state.settings.peek().favorites[0].name == "Docs");
    EXPECT(at(1) == local(tree.path("Download/Docs/deep/b.txt")));
    EXPECT(state.settings.peek().hiddenItems.front() == local(tree.path("Download/Docs/deep")));
    EXPECT(h.waitUntil([&] { return idle(h) && byName(*h.app->root(), "Dialog") == nullptr; }));

    // Move: it goes where the file went; the user's own name stays.
    ops(h).cut(targets(h, {"apple.md"}));
    ops(h).paste(local(tree.path("Music")));
    EXPECT(h.waitUntil([&] { return at(2) == local(tree.path("Music/apple.md")); }));
    EXPECT(state.settings.peek().favorites[2].name == "Notes");
    EXPECT(h.waitUntil([&] { return idle(h); }));

    // Delete: it goes.
    ops(h).remove(targets(h, {"file1.txt"}));
    auto *confirm = sheet(h, "DeleteSheet");
    h.tap(*byLabel(confirm->panel(), "Delete 1 item"));
    EXPECT(h.waitUntil([&] { return state.settings.peek().favorites.size() == 3; }));
    EXPECT(h.waitUntil([&] { return state.settings.peek().hiddenItems.size() == 1; }));

    // A copy changes nothing.
    ops(h).copy(targets(h, {"Docs"}));
    ops(h).paste(local(tree.path("Music")));
    EXPECT(h.waitUntil([&] { return idle(h) && fs::exists(tree.home / "Music" / "Docs"); }));
    h.pump(100);
    EXPECT(at(0) == local(tree.path("Download/Docs")));
}

int main() {
    testUserItemsFollow();
    testBand();
    testNotificationThrottle();
    testCopyPaste();
    testCutPaste();
    testDelete();
    testRename();
    testShareSave();
    testDocumentCreate();
    testDocumentPick();
    testConflictKeepBothApplyToAll();
    testCancelWhileWaiting();
    testErrorRetry();
    testProperties();
    testCompressAndExtract();
    testVerifyArchive();
    testCompressDeletesOriginals();
    testShareAndOpen();
    testDocumentTrees();
    std::puts("file_operations_test: OK");
    return 0;
}
