// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/text_screen.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

#include "filepan/app/format.hpp"
#include "filepan/app/jobs_ui.hpp"
#include "filepan/text/text_document.hpp"

namespace filepan::app {

using namespace ui;
using namespace ui::components;

namespace {

std::string lineCount(std::size_t lines) { return std::to_string(lines) + (lines == 1 ? " line" : " lines"); }

}  // namespace

bool isTextFile(const vfs::Entry &entry) {
    if (entry.targetType != vfs::EntryType::kFile) {
        return false;
    }
    const vfs::FileKind kind = entry.mimeType().kind;
    if (kind == vfs::FileKind::kText || kind == vfs::FileKind::kCode) {
        return true;
    }
    // Plain text the MIME table has no type for; anything else is one "Open as text" away.
    static constexpr std::string_view kExtensions[] = {"log", "conf", "cfg", "ini", "env", "properties", "toml", "nfo"};
    const std::string_view name = entry.name;
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0) {
        return false;
    }
    std::string extension(name.substr(dot + 1));
    std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return std::tolower(c); });
    return std::ranges::find(kExtensions, extension) != std::end(kExtensions);
}

struct TextScreen::Loaded {
    std::optional<text::TextDocument> document;
    vfs::Entry entry;
    bool writable = false;
    std::string error;
};

std::shared_ptr<TextScreen> TextScreen::create(AppContext &context, FileTargets file, std::function<void()> onClose) {
    auto screen = std::make_shared<TextScreen>(context, std::move(file), std::move(onClose));
    screen->construct();
    screen->load();
    return screen;
}

TextScreen::TextScreen(AppContext &context, FileTargets file, std::function<void()> onClose)
    : context_(context), file_(std::move(file)), onClose_(std::move(onClose)) {}

vfs::Location TextScreen::location() const { return file_.directory.child(file_.entries.front().name); }

void TextScreen::construct() {
    TextScreen *self = this;
    const Theme &t = currentTheme();
    setDebugName("TextScreen");
    setStyle({.position = Position::absolute, .left = 0, .top = 0, .right = 0, .bottom = 0});
    setHitTestBehavior(HitTestBehavior::opaque);
    setPaintHook([](PaintContext &context) { context.list.fill(context.bounds(), currentTheme().colors.ground); });

    // Top: back, the name over what the file is, save, wrap, more.
    top_ = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 2});
    top_->setDebugName("TextTop");
    back_ = IconButton::create({.icon = "arrow_back",
                                .label = "Back",
                                .style = IconButtonStyle::plain,
                                .size = 40,
                                .onTap = [self] { self->back(); }});
    top_->appendChild(back_);
    auto names = Box::create({.gap = 2, .padding = {6, 0, 0, 0}, .grow = 1, .shrink = 1, .basis = 0});
    auto title = Text::create(file_.entries.front().name, withWeight(t.type.body, 600), t.colors.ink);
    title->setMaxLines(1);
    title->setOverflow(gfx::TextOverflow::ellipsisMiddle);
    title->updateSemantics([](Semantics &s) { s.role = Role::heading; });
    title->bind([title = title.get()] {
        const Theme &th = theme();
        untrack([&] { title->setColor(th.colors.ink); });
    });
    names->appendChild(title);
    subtitle_ = Text::create("Reading…", withSize(t.type.meta, 11), textInk(t, t.text.meta, 11));
    subtitle_->setMaxLines(1);
    names->appendChild(subtitle_);
    top_->appendChild(names);
    save_ = IconButton::create({.icon = "save",
                                .label = "Save",
                                .style = IconButtonStyle::plain,
                                .size = 40,
                                .enabled = [self] { return self->modified_() && !self->saving_(); },
                                .onTap = [self] { self->save(); }});
    top_->appendChild(save_);
    wrap_ = IconButton::create({.icon = "wrap_text",
                                .label = "Wrap lines",
                                .style = IconButtonStyle::plain,
                                .size = 40,
                                .enabled = [self] { return self->status_() == Status::ready; },
                                .selected = [self] { return self->wrapping_(); },
                                .onTap = [self] {
                                    if (self->editor_) {
                                        self->editor_->setWrap(!self->editor_->wrap());
                                        self->wrapping_.set(self->editor_->wrap());
                                    }
                                }});
    top_->appendChild(wrap_);
    more_ = IconButton::create({.icon = "more_vert",
                                .label = "More options",
                                .style = IconButtonStyle::plain,
                                .size = 40,
                                .onTap = [self] { self->showMore(); }});
    top_->appendChild(more_);
    appendChild(top_);

    body_ = Box::create({.grow = 1, .shrink = 1, .basis = 0});
    appendChild(body_);
    auto reading = Text::create("Reading…", t.type.body, ink(t, t.text.meta));
    reading->updateStyle([](Style &s) { s.margin = Edges::all(24); });
    body_->appendChild(reading);

    // Clear of the status bar, the navigation bar and the keyboard.
    bind([self] {
        const float density = environment().density();
        const Insets safe = environment().safeArea();
        const Insets ime = environment().imeInsets();
        untrack([&] {
            self->top_->updateStyle([&](Style &s) {
                s.padding = {4 + safe.left / density, 4 + safe.top / density, 4 + safe.right / density, 4};
            });
            self->body_->updateStyle([&](Style &s) {
                s.padding = {safe.left / density, 0, safe.right / density,
                             std::max(safe.bottom, ime.bottom) / density};
            });
        });
    });
    // What the file is, and whether it has changes.
    subtitle_->bind([self] {
        const Status status = self->status_();
        const bool modified = self->modified_();
        const bool saving = self->saving_();
        const Theme &th = theme();
        untrack([&] {
            std::string text;
            if (status == Status::loading) {
                text = "Reading…";
            } else if (status == Status::failed) {
                text = "Can't show this file";
            } else {
                const text::TextDocument &doc = self->editor_->document();
                text = std::string(text::encodingName(doc.encoding())) + " · " + text::lineEndingName(doc.lineEnding()) +
                       " · " + lineCount(doc.lineCount());
                if (saving) {
                    text += " · Saving…";
                } else if (modified) {
                    text += " · Modified";
                } else if (self->readOnly_) {
                    text += " · Read only";
                }
            }
            self->subtitle_->setText(text);
            self->subtitle_->setColor(textInk(th, th.text.meta, 11));
        });
    });
}

void TextScreen::load() {
    const vfs::Location file = location();
    vfs::Backend *backend = context_.services.backends.backendFor(file);
    if (backend == nullptr) {
        loaded({.error = "This file's storage isn't available."});
        return;
    }
    std::weak_ptr<const void> lifetime = context_.lifetime;
    auto weak = std::weak_ptr<TextScreen>(std::static_pointer_cast<TextScreen>(shared_from_this()));
    context_.services.io.submit(
            [backend, file]() -> Loaded {
                Loaded out;
                auto entry = backend->stat(file);
                if (!entry) {
                    out.error = describeError(entry.error());
                    return out;
                }
                out.entry = *entry;
                if (entry->size > kMaxTextOpen) {
                    out.error = "It's too large to open as text (" + formatSize(entry->size) + ").";
                    return out;
                }
                auto stream = backend->openRead(file);
                if (!stream) {
                    out.error = describeError(stream.error());
                    return out;
                }
                std::vector<std::uint8_t> bytes;
                bytes.reserve(static_cast<std::size_t>(entry->size));
                std::vector<std::uint8_t> chunk(1 << 20);
                while (true) {
                    auto read = (*stream)->read(chunk);
                    if (!read) {
                        out.error = describeError(read.error());
                        return out;
                    }
                    if (*read == 0) {
                        break;
                    }
                    bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*read));
                    if (bytes.size() > kMaxTextOpen) {
                        out.error = "It's too large to open as text.";
                        return out;
                    }
                }
                auto document = text::TextDocument::decode(bytes);
                if (!document) {
                    out.error = "It isn't a text file.";
                    return out;
                }
                out.document = std::move(*document);
                out.writable = backend->capabilities().has(vfs::Capability::kWrite) && bytes.size() <= kMaxTextEdit;
                return out;
            },
            [weak, lifetime](Loaded result) {
                if (lifetime.expired()) {
                    return;
                }
                if (auto self = weak.lock()) {
                    self->loaded(std::move(result));
                }
            });
}

void TextScreen::loaded(Loaded result) {
    const Theme &t = currentTheme();
    body_->clearChildren();
    if (!result.document) {
        error_ = result.error;
        auto panel = Box::create({.justify = Justify::center, .alignItems = Align::center, .gap = 14,
                                  .padding = Edges::all(32), .grow = 1});
        panel->appendChild(Icon::create("description", 40, ink(t, t.text.meta)));
        auto text = Text::create(error_, t.type.body, ink(t, t.text.body));
        text->setAlign(gfx::TextAlign::center);
        text->setMaxLines(4);
        panel->appendChild(text);
        panel->appendChild(Button::create({.label = "Open with",
                                           .variant = ButtonVariant::glass,
                                           .icon = "open_in_new",
                                           .onTap = [this] { context_.actions.openWith(file_); }}));
        body_->appendChild(panel);
        status_.set(Status::failed);
        return;
    }
    readOnly_ = !result.writable;
    stamp_ = vfs::FileStamp::of(result.entry);
    TextScreen *self = this;
    editor_ = TextEditor::create(
            std::move(*result.document),
            {.readOnly = readOnly_,
             .onCopy = [self](const std::string &text) { self->context_.services.platform.setClipboardText(text); },
             .onPaste = [self] { return self->context_.services.platform.clipboardText(); },
             .onSave = [self] {
                 if (self->modified_.peek()) {
                     self->save();
                 }
             }});
    editor_->updateSemantics([&](Semantics &s) { s.label = file_.entries.front().name; });
    savedRevision_ = editor_->revision().peek();
    editor_->bind([self] {
        const std::uint64_t revision = self->editor_->revision()();
        untrack([&] { self->modified_.set(revision != self->savedRevision_); });
    });
    body_->appendChild(editor_);
    status_.set(Status::ready);
}

void TextScreen::save(std::function<void()> then) {
    if (!editor_ || readOnly_ || saving_.peek()) {
        return;
    }
    text::TextDocument document = editor_->document();
    if (!document.canEncode()) {
        // What was typed doesn't fit the file's old 8-bit encoding: UTF-8 holds everything.
        document.setEncoding(text::TextEncoding::utf8);
        context_.notify("Saved as UTF-8, which holds every character");
    }
    write(document.encode(), stamp_, editor_->revision().peek(), std::move(then));
}

void TextScreen::write(std::vector<std::uint8_t> bytes, std::optional<vfs::FileStamp> expected, std::uint64_t revision,
                       std::function<void()> then) {
    const vfs::Location file = location();
    vfs::Backend *backend = context_.services.backends.backendFor(file);
    if (backend == nullptr) {
        context_.notify("Couldn't save: this file's storage isn't available");
        return;
    }
    saving_.set(true);
    std::weak_ptr<const void> lifetime = context_.lifetime;
    auto weak = std::weak_ptr<TextScreen>(std::static_pointer_cast<TextScreen>(shared_from_this()));
    auto shared = std::make_shared<std::vector<std::uint8_t>>(std::move(bytes));
    context_.services.io.submit(
            [backend, file, shared, expected] { return vfs::saveFile(*backend, file, *shared, expected); },
            [weak, lifetime, shared, revision, then = std::move(then)](Result<vfs::Entry> saved) {
                if (lifetime.expired()) {
                    return;
                }
                auto self = weak.lock();
                if (!self) {
                    return;
                }
                self->saving_.set(false);
                if (saved) {
                    self->stamp_ = vfs::FileStamp::of(*saved);
                    self->savedRevision_ = revision;
                    self->modified_.set(self->editor_ && self->editor_->revision().peek() != revision);
                    self->context_.notify("Saved");
                    if (then) {
                        then();
                    }
                    return;
                }
                if (saved.error().code != vfs::kSaveConflict) {
                    self->context_.notify("Couldn't save: " + describeError(saved.error()));
                    return;
                }
                // Changed by something else since it was read: overwrite only if asked to.
                auto holder = std::make_shared<Dialog *>(nullptr);
                std::vector<NodePtr> actions;
                actions.push_back(Button::create({.label = "Cancel", .variant = ButtonVariant::text, .onTap = [holder] {
                                                      if (*holder != nullptr) {
                                                          (*holder)->dismiss();
                                                      }
                                                  }}));
                actions.push_back(Button::create(
                        {.label = "Overwrite", .variant = ButtonVariant::danger, .onTap = [weak, holder, shared, revision, then] {
                             if (*holder != nullptr) {
                                 (*holder)->dismiss();
                             }
                             if (auto self = weak.lock()) {
                                 self->write(std::move(*shared), std::nullopt, revision, then);
                             }
                         }}));
                *holder = &self->context_.overlays.showDialog(
                        {.title = "File changed",
                         .message = "“" + self->file_.entries.front().name +
                                    "” changed since it was opened. Overwrite it with your version?",
                         .actions = std::move(actions)});
            });
}

void TextScreen::back() {
    if (!modified_.peek()) {
        close();
        return;
    }
    auto holder = std::make_shared<Dialog *>(nullptr);
    const auto dismiss = [holder] {
        if (*holder != nullptr) {
            (*holder)->dismiss();
        }
    };
    TextScreen *self = this;
    std::vector<NodePtr> actions;
    actions.push_back(Button::create({.label = "Discard", .variant = ButtonVariant::text, .onTap = [self, dismiss] {
                                          dismiss();
                                          self->close();
                                      }}));
    actions.push_back(Button::create({.label = "Cancel", .variant = ButtonVariant::text, .onTap = dismiss}));
    actions.push_back(Button::create({.label = "Save", .variant = ButtonVariant::primary, .onTap = [self, dismiss] {
                                          dismiss();
                                          self->save([self] { self->close(); });
                                      }}));
    *holder = &context_.overlays.showDialog(
            {.title = "Save changes?",
             .message = "“" + file_.entries.front().name + "” has changes that aren't saved.",
             .actions = std::move(actions)});
}

void TextScreen::close() {
    if (onClose_) {
        onClose_();
    }
}

void TextScreen::showMore() {
    std::vector<MenuItem> items;
    TextScreen *self = this;
    if (editor_) {
        const bool selected = !editor_->peekSelection().collapsed();
        items.push_back({"select_all", "Select all", [self] { self->editor_->selectAll(); }});
        if (selected) {
            items.push_back({"content_copy", "Copy", [self] { self->editor_->copy(); }});
        }
        if (selected && !readOnly_) {
            items.push_back({"content_cut", "Cut", [self] { self->editor_->cut(); }});
        }
        if (!readOnly_) {
            items.push_back({"content_paste", "Paste", [self] { self->editor_->paste(); }});
        }
    }
    const FileTargets here = file_;
    AppContext &context = context_;
    items.push_back({"open_in_new", "Open with", [&context, here] { context.actions.openWith(here); }});
    items.push_back({"info", "Properties", [&context, here] { context.actions.properties(here); }});
    context_.overlays.showMenu(std::move(items), *more_);
}

}  // namespace filepan::app
