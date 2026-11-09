// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/file_dialogs.hpp"

#include <archive.h>

#include <algorithm>
#include <array>
#include <memory>

#include "filepan/app/app.hpp"
#include "filepan/app/format.hpp"
#include "filepan/app/sheet_parts.hpp"
#include "filepan/cancellation.hpp"

namespace filepan::app {

using namespace ui;
using namespace ui::components;

namespace {

constexpr std::array<std::string_view, 6> kCompoundExtensions = {".tar.gz", ".tar.xz", ".tar.zst", ".tar.bz2",
                                                                 ".tar.lz4", ".tar.lzma"};

bool endsWithIgnoringCase(std::string_view text, std::string_view suffix) {
    if (text.size() < suffix.size()) {
        return false;
    }
    return std::ranges::equal(text.substr(text.size() - suffix.size()), suffix, [](char a, char b) {
        return (a >= 'A' && a <= 'Z' ? a + 32 : a) == (b >= 'A' && b <= 'Z' ? b + 32 : b);
    });
}

// A folder's names, and which of them are folders, each sorted.
struct FolderNames {
    std::vector<std::string> names;
    std::vector<std::string> directories;
};

// Filled in once the I/O pool has listed the folder.
Signal<FolderNames> listNames(runtime::AppServices &services, const vfs::Location &folder) {
    Signal<FolderNames> names{FolderNames{}};
    vfs::BackendResolver *backends = &services.backends;
    services.io.submit(
            [backends, folder] {
                FolderNames out;
                if (vfs::Backend *backend = backends->backendFor(folder)) {
                    const CancellationToken never;
                    (void)backend->list(folder, {.withStat = false}, [&](std::vector<vfs::Entry> batch) {
                        for (auto &entry : batch) {
                            if (entry.type == vfs::EntryType::kDirectory) {
                                out.directories.push_back(entry.name);
                            }
                            out.names.push_back(std::move(entry.name));
                        }
                    }, never);
                }
                std::ranges::sort(out.names);
                std::ranges::sort(out.directories);
                return out;
            },
            [names](FolderNames out) { names.set(std::move(out)); });
    return names;
}

std::string countLabel(std::size_t count, const char *one, const char *many) {
    return std::to_string(count) + " " + (count == 1 ? one : many);
}

NodePtr dialogButton(std::string label, ButtonVariant variant, std::function<void()> onTap, Prop<bool> enabled = true) {
    return Button::create({.label = std::move(label), .variant = variant, .enabled = std::move(enabled), .onTap = std::move(onTap)});
}

}  // namespace

std::string nameError(std::string_view name, const std::vector<std::string> &existing, std::string_view original) {
    if (name.empty()) {
        return "Enter a name";
    }
    if (name.find('/') != std::string_view::npos) {
        return "Names can't contain “/”";
    }
    if (name.find('\0') != std::string_view::npos) {
        return "Names can't contain a null character";
    }
    if (name == "." || name == "..") {
        return "“" + std::string(name) + "” isn't a usable name";
    }
    if (name.size() > 255) {
        return "That name is too long";
    }
    if (name != original && std::ranges::binary_search(existing, name)) {
        return "“" + std::string(name) + "” already exists";
    }
    return {};
}

std::string_view nameStem(std::string_view name, bool directory) {
    if (directory) {
        return name;
    }
    for (std::string_view compound : kCompoundExtensions) {
        if (name.size() > compound.size() && endsWithIgnoringCase(name, compound)) {
            return name.substr(0, name.size() - compound.size());
        }
    }
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0) {
        return name;
    }
    return name.substr(0, dot);
}

std::string extractFolderName(std::string_view archiveName) {
    std::string_view stem = nameStem(archiveName, false);
    return std::string(stem.empty() ? archiveName : stem);
}

void showNameDialog(runtime::AppServices &services, Overlays &overlays, NameDialog dialog) {
    const Signal<FolderNames> names = listNames(services, dialog.folder);
    const Signal<std::string> text{dialog.initial};
    const Signal<bool> replace{false};
    const std::string original = dialog.original;
    const bool canReplace = dialog.allowReplace && !dialog.directory;
    // Taken by a file, which the switch can replace; a taken folder stays an error.
    const auto replaceable = [names, text, original, canReplace] {
        if (!canReplace) {
            return false;
        }
        const std::string name = text();
        const FolderNames listed = names();
        return name != original && std::ranges::binary_search(listed.names, name)
               && !std::ranges::binary_search(listed.directories, name);
    };
    // Whether to replace, given what's typed now.
    const auto replacing = [replaceable, replace] { return replace() && replaceable(); };
    const auto error = [names, text, original, replacing] {
        return replacing() ? std::string() : nameError(text(), names().names, original);
    };
    auto holder = std::make_shared<Dialog *>(nullptr);
    auto submitted = std::make_shared<bool>(false);
    auto onSubmit = std::make_shared<std::function<void(const std::string &, bool)>>(std::move(dialog.onSubmit));
    const auto submit = [holder, text, error, replacing, original, onSubmit, submitted] {
        if (*submitted || !untrack(error).empty()) {
            return;
        }
        *submitted = true;
        const std::string name = text.peek();
        const bool replaceIt = untrack(replacing);
        if (*holder != nullptr) {
            (*holder)->dismiss();
        }
        if (name != original && *onSubmit) {
            (*onSubmit)(name, replaceIt);
        }
    };
    auto field = TextField::create({.label = dialog.label,
                                    .text = dialog.initial,
                                    .helper = [replacing] {
                                        return replacing() ? std::string("Replaces the existing file") : std::string();
                                    },
                                    .error = [error, text, original] {
                                        // Untouched, the original name isn't an error to point at.
                                        return text() == original && !original.empty() ? std::string() : error();
                                    },
                                    .config = {.action = ImeAction::done},
                                    .onChange = [text, replace](const std::string &value) {
                                        // A switch turned on for one name doesn't carry over to another.
                                        if (value != text.peek()) {
                                            replace.set(false);
                                        }
                                        text.set(value);
                                    },
                                    .onSubmit = submit});
    NodePtr content = field;
    if (canReplace) {
        auto column = Box::create({.gap = 4});
        column->appendChild(field);
        auto line = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 12, .minHeight = 56});
        auto label = parts::bodyText("Replace existing");
        label->updateStyle([](Style &s) {
            s.grow = 1;
            s.shrink = 1;
            s.basis = 0;
        });
        line->appendChild(label);
        line->appendChild(Switch::create({.checked = [replace] { return replace(); },
                                          .onChange = [replace](bool on) { replace.set(on); },
                                          .label = "Replace existing"}));
        line->bind([l = line.get(), replaceable] {
            const bool shown = replaceable();
            untrack([&] { l->updateStyle([&](Style &s) { s.display = shown; }); });
        });
        column->appendChild(line);
        content = column;
    }
    const std::size_t stem = nameStem(dialog.initial, dialog.directory).size();
    field->editor().setText(dialog.initial, {0, static_cast<std::int32_t>(stem)});
    EditableText *editor = &field->editor();
    std::vector<NodePtr> actions;
    actions.push_back(dialogButton("Cancel", ButtonVariant::text, [holder] {
        if (*holder != nullptr) {
            (*holder)->dismiss();
        }
    }));
    actions.push_back(dialogButton(dialog.action, ButtonVariant::primary, submit, [error] { return error().empty(); }));
    Dialog &shown = overlays.showDialog({.title = dialog.title, .content = content, .actions = std::move(actions)});
    *holder = &shown;
    editor->requestFocus();
}

void showDeleteSheet(Overlays &overlays, const std::vector<vfs::Entry> &entries, std::function<void()> onConfirm) {
    constexpr std::size_t kShown = 4;
    auto column = parts::sheetColumn(2);
    column->setDebugName("DeleteSheet");
    const std::size_t folders = std::ranges::count_if(entries, [](const vfs::Entry &e) { return e.isDirectoryLike(); });
    std::string message = "Deleted files don't go to a trash; this can't be undone.";
    if (folders > 0) {
        message = "Folders go with everything in them. " + message;
    }
    auto text = parts::bodyText(message);
    text->updateStyle([](Style &s) { s.margin = {0, 0, 0, 6}; });
    column->appendChild(text);
    for (std::size_t i = 0; i < entries.size() && i < kShown; ++i) {
        column->appendChild(parts::fileRow(entries[i]));
    }
    if (entries.size() > kShown) {
        auto more = parts::monoText("+ " + std::to_string(entries.size() - kShown) + " more");
        more->updateStyle([](Style &s) { s.padding = {56, 4, 0, 4}; });
        column->appendChild(more);
    }
    auto holder = std::make_shared<BottomSheet *>(nullptr);
    auto cancel = dialogButton("Cancel", ButtonVariant::glass, [holder] { (*holder)->dismiss(); });
    auto remove = Button::create({.label = "Delete",
                                  .variant = ButtonVariant::danger,
                                  .icon = "delete",
                                  .onTap =
                                          [holder, onConfirm = std::move(onConfirm)] {
                                              (*holder)->dismiss();
                                              onConfirm();
                                          },
                                  .accessibilityLabel = "Delete " + countLabel(entries.size(), "item", "items")});
    auto buttons = parts::buttonRow({cancel, remove});
    buttons->updateStyle([](Style &s) { s.margin = {0, 10, 0, 0}; });
    column->appendChild(buttons);
    const std::string title = entries.size() == 1 ? "Delete “" + entries.front().name + "”?"
                                                  : "Delete " + countLabel(entries.size(), "item", "items") + "?";
    *holder = &overlays.showSheet(title, column);
}

void showExtractSheet(Overlays &overlays, const std::vector<vfs::Entry> &archives, const vfs::Location &folder,
                      std::function<void(const ExtractChoice &)> onExtract) {
    auto column = parts::sheetColumn(4);
    column->setDebugName("ExtractSheet");
    for (const vfs::Entry &entry : archives) {
        column->appendChild(parts::fileRow(entry));
    }
    const Signal<bool> newFolder{true};
    const std::string here = folder.isRoot() ? std::string("/") : std::string(folder.name());
    const std::string inside = archives.size() == 1 ? extractFolderName(archives.front().name) : std::string("one per archive");
    column->appendChild(parts::section("Destination"));
    auto line = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 12, .minHeight = 56});
    auto texts = Box::create({.gap = 2, .grow = 1, .shrink = 1, .basis = 0});
    texts->appendChild(parts::bodyText("New folder"));
    auto where = parts::monoText({}, 1, false);
    where->bind([w = where.get(), newFolder, here, inside] {
        const bool on = newFolder();
        untrack([&] { w->setText(on ? here + " / " + inside : here); });
    });
    texts->appendChild(where);
    line->appendChild(texts);
    line->appendChild(Switch::create(
            {.checked = [newFolder] { return newFolder(); }, .onChange = [newFolder](bool on) { newFolder.set(on); },
             .label = "Extract into a new folder"}));
    column->appendChild(line);
    // An encrypted archive asks for its password once the extraction meets it.
    auto holder = std::make_shared<BottomSheet *>(nullptr);
    auto cancel = dialogButton("Cancel", ButtonVariant::glass, [holder] { (*holder)->dismiss(); });
    auto extract = Button::create({.label = "Extract",
                                   .variant = ButtonVariant::primary,
                                   .icon = "unarchive",
                                   .onTap = [holder, newFolder, onExtract = std::move(onExtract)] {
                                       (*holder)->dismiss();
                                       onExtract({.newFolder = newFolder.peek()});
                                   }});
    auto buttons = parts::buttonRow({cancel, extract});
    buttons->updateStyle([](Style &s) { s.margin = {0, 12, 0, 0}; });
    column->appendChild(buttons);
    *holder = &overlays.showSheet(archives.size() == 1 ? "Extract" : "Extract " + std::to_string(archives.size()) + " archives",
                                  parts::scrolling(column));
}

void showArchiveProblemsSheet(Overlays &overlays, const std::string &archiveName, std::uint64_t entries,
                              const std::vector<ArchiveProblem> &problems) {
    constexpr std::size_t kShown = 50;
    auto column = parts::sheetColumn(2);
    column->setDebugName("ArchiveProblemsSheet");
    std::string message = problems.size() >= kMaxArchiveProblems
                                  ? "At least " + countLabel(problems.size(), "problem", "problems") + "; the test stopped there."
                                  : countLabel(problems.size(), "problem", "problems") + " in " + formatCount(entries) + ".";
    message += " What's listed may extract damaged, or not at all.";
    auto text = parts::bodyText(message);
    text->updateStyle([](Style &s) { s.margin = {0, 0, 0, 6}; });
    column->appendChild(text);
    for (std::size_t i = 0; i < problems.size() && i < kShown; ++i) {
        auto item = Box::create({.gap = 2, .padding = {0, 6, 0, 6}});
        item->appendChild(parts::bodyText(problems[i].path.empty() ? "The archive as a whole" : problems[i].path));
        item->appendChild(parts::monoText(problems[i].message, 0, false));
        column->appendChild(item);
    }
    if (problems.size() > kShown) {
        column->appendChild(parts::monoText("+ " + std::to_string(problems.size() - kShown) + " more"));
    }
    auto holder = std::make_shared<BottomSheet *>(nullptr);
    auto close = dialogButton("Close", ButtonVariant::glass, [holder] { (*holder)->dismiss(); });
    auto buttons = parts::buttonRow({close});
    buttons->updateStyle([](Style &s) { s.margin = {0, 10, 0, 0}; });
    column->appendChild(buttons);
    *holder = &overlays.showSheet("“" + archiveName + "” is damaged", parts::scrolling(column));
}

const std::vector<ArchiveFormat> &archiveFormats() {
    static const std::vector<ArchiveFormat> formats = {
            {"ZIP", ".zip", ARCHIVE_FORMAT_ZIP, ARCHIVE_FILTER_NONE, {1, 6, 9}},
            {"TAR.ZST", ".tar.zst", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_ZSTD, {1, 3, 19}},
            {"TAR.XZ", ".tar.xz", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_XZ, {1, 6, 9}},
            {"7Z", ".7z", ARCHIVE_FORMAT_7ZIP, ARCHIVE_FILTER_NONE, {1, 5, 9}},
    };
    return formats;
}

void showCompressSheet(runtime::AppServices &services, Overlays &overlays, const std::vector<vfs::Entry> &entries,
                       const vfs::Location &folder, std::function<void(const CompressChoice &)> onCompress) {
    const Signal<FolderNames> names = listNames(services, folder);
    std::string initial;
    if (entries.size() == 1) {
        initial = std::string(nameStem(entries.front().name, entries.front().isDirectoryLike()));
    } else {
        initial = folder.isRoot() ? std::string("Archive") : std::string(folder.name());
    }
    const Signal<std::string> stem{initial};
    const Signal<std::size_t> format{std::size_t{0}};
    const Signal<std::size_t> level{std::size_t{1}};
    const Signal<std::string> password{std::string()};
    const Signal<bool> deleteSources{false};
    const auto fullName = [stem, format] { return stem() + archiveFormats()[format()].extension; };
    const auto error = [stem, names, fullName] {
        if (stem().empty()) {
            return std::string("Enter a name");
        }
        return nameError(fullName(), names().names);
    };

    auto column = parts::sheetColumn(6);
    column->setDebugName("CompressSheet");
    const std::string what = entries.size() == 1 ? "“" + entries.front().name + "”" : countLabel(entries.size(), "item", "items");
    auto intro = parts::bodyText("Packs " + what + " into a new archive here.");
    intro->updateStyle([](Style &s) { s.margin = {0, 0, 0, 8}; });
    column->appendChild(intro);
    auto field = TextField::create({.label = "Archive name",
                                    .text = initial,
                                    .icon = "folder_zip",
                                    .error = error,
                                    .config = {.action = ImeAction::done},
                                    .onChange = [stem](const std::string &value) { stem.set(value); }});
    column->appendChild(field);
    // The name it'll get, in its own case (the field's helper line is caps).
    auto saved = parts::monoText({}, 1, false);
    saved->updateStyle([](Style &s) { s.margin = {0, 2, 0, 0}; });
    saved->bind([s = saved.get(), fullName, error] {
        const std::string name = fullName();
        const bool shown = error().empty();
        untrack([&] {
            s->setText("Saved as " + name);
            s->updateStyle([&](Style &st) { st.display = shown; });
        });
    });
    column->appendChild(saved);
    column->appendChild(parts::section("Format"));
    std::vector<std::string> labels;
    for (const ArchiveFormat &f : archiveFormats()) {
        labels.emplace_back(f.label);
    }
    column->appendChild(SegmentedControl::create({.options = labels,
                                                  .selected = [format] { return format(); },
                                                  .onSelect = [format](std::size_t i) { format.set(i); },
                                                  .label = "Format"}));
    column->appendChild(parts::section("Compression"));
    column->appendChild(SegmentedControl::create({.options = {"Fast", "Normal", "Best"},
                                                  .selected = [level] { return level(); },
                                                  .onSelect = [level](std::size_t i) { level.set(i); },
                                                  .label = "Compression level"}));
    auto passwordBox = Box::create({.gap = 6});
    passwordBox->appendChild(parts::section("Encryption"));
    passwordBox->appendChild(TextField::create({.label = "Password",
                                                .placeholder = "None",
                                                .icon = "key",
                                                .helper = "ZIP with AES-256; leave empty for none",
                                                .config = {.keyboard = KeyboardType::password,
                                                           .action = ImeAction::done},
                                                .onChange = [password](const std::string &value) { password.set(value); }}));
    passwordBox->bind([box = passwordBox.get(), format] {
        const bool zip = archiveFormats()[format()].format == ARCHIVE_FORMAT_ZIP;
        untrack([&] { box->updateStyle([&](Style &s) { s.display = zip; }); });
    });
    column->appendChild(passwordBox);
    column->appendChild(parts::section("Originals"));
    auto deleteLine = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 12, .minHeight = 56});
    auto deleteTexts = Box::create({.gap = 2, .grow = 1, .shrink = 1, .basis = 0});
    deleteTexts->appendChild(parts::bodyText("Delete originals after compressing"));
    deleteTexts->appendChild(parts::monoText("Only if every item was archived", 1, false));
    deleteLine->appendChild(deleteTexts);
    deleteLine->appendChild(Switch::create({.checked = [deleteSources] { return deleteSources(); },
                                            .onChange = [deleteSources](bool on) { deleteSources.set(on); },
                                            .label = "Delete originals after compressing"}));
    column->appendChild(deleteLine);

    auto holder = std::make_shared<BottomSheet *>(nullptr);
    auto cancel = dialogButton("Cancel", ButtonVariant::glass, [holder] { (*holder)->dismiss(); });
    auto compress = Button::create(
            {.label = "Compress",
             .variant = ButtonVariant::primary,
             .icon = "archive",
             .enabled = [error] { return error().empty(); },
             .onTap = [holder, error, fullName, format, level, password, deleteSources, onCompress = std::move(onCompress)] {
                 if (!untrack(error).empty()) {
                     return;
                 }
                 const ArchiveFormat &f = archiveFormats()[format.peek()];
                 CompressChoice choice{.name = untrack(fullName),
                                       .options = {.format = f.format, .filter = f.filter}};
                 choice.options.level = f.levels[level.peek()];
                 choice.deleteSources = deleteSources.peek();
                 if (f.format == ARCHIVE_FORMAT_ZIP) {
                     choice.options.password = password.peek();
                 }
                 (*holder)->dismiss();
                 onCompress(choice);
             }});
    auto buttons = parts::buttonRow({cancel, compress});
    buttons->updateStyle([](Style &s) { s.margin = {0, 12, 0, 0}; });
    column->appendChild(buttons);
    *holder = &overlays.showSheet("Compress", parts::scrolling(column));
}

}  // namespace filepan::app
