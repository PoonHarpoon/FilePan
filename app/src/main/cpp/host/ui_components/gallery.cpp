// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "gallery.hpp"

#include <string>
#include <vector>

#include "filepan/ui/components/components.hpp"
#include "filepan/ui/virtual_list.hpp"

namespace filepan::test {

using namespace ui;
using namespace ui::components;

namespace {

const char *presetLabel() {
    switch (currentTheme().preset) {
    case ThemePreset::ultraviolet:
        return "ULTRAVIOLET";
    case ThemePreset::clean:
        return currentTheme().colors.light ? "CLEAN LIGHT" : "CLEAN DARK";
    case ThemePreset::acid:
        return "ACID";
    }
    return "";
}

std::shared_ptr<Box> screen(BackgroundProps background = {}) {
    auto root = Box::create();
    root->appendChild(Background::create(background));
    return root;
}

// The scrolling column of a gallery page, below the status bar.
std::shared_ptr<Box> column(Node &root) {
    auto col = Box::create({.gap = 8, .padding = {16, 24 + 8, 16, 24}, .grow = 1});
    root.appendChild(col);
    return col;
}

NodePtr heading(const std::string &title, const std::string &subtitle) {
    const Theme &t = currentTheme();
    auto box = Box::create({.gap = 6, .padding = {4, 4, 4, 6}});
    auto display = Text::create(title, t.type.display, t.colors.ink);
    box->appendChild(display);
    auto sub = Text::create(subtitle + " · " + presetLabel(), t.type.label,
                            textInk(t, t.text.label, 11));
    box->appendChild(sub);
    return box;
}

NodePtr section(const std::string &label) {
    auto s = SectionLabel(label);
    s->updateStyle([](Style &st) { st.padding = {4, 6, 0, 0}; st.minHeight = 32; });
    return s;
}

NodePtr row(std::vector<NodePtr> children, float gap = 12, Justify justify = Justify::start) {
    auto r = Box::create({.direction = Direction::row, .justify = justify, .alignItems = Align::center, .wrap = true,
                          .gap = gap, .lineGap = gap});
    for (auto &c : children) {
        r->appendChild(std::move(c));
    }
    return r;
}

// A labelled settings line: text on the left, a control on the right.
NodePtr settingLine(const std::string &title, const std::string &detail, NodePtr control) {
    const Theme &t = currentTheme();
    auto line = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 12, .minHeight = 56});
    auto texts = Box::create({.gap = 2, .grow = 1, .shrink = 1, .basis = 0});
    texts->appendChild(Text::create(title, t.type.body, t.colors.ink));
    if (!detail.empty()) {
        texts->appendChild(Text::create(detail, t.type.label, textInk(t, t.text.meta, 11)));
    }
    line->appendChild(texts);
    line->appendChild(std::move(control));
    return line;
}

NodePtr divider() {
    return Box::create({.height = 1}, {.background = gfx::Paint{currentTheme().colors.ink.withAlpha(0.08f)}});
}

FileItem file(const std::string &name, const std::string &meta, FileKind kind, bool selected = false,
              bool selecting = false) {
    return {.name = name,
            .meta = meta,
            .icon = kindIcon(kind),
            .kindColor = kindColor(kind, currentTheme()),
            .selected = selected,
            .selecting = selecting};
}

NodePtr fileRow(const FileItem &item) {
    auto r = FileRow::create();
    r->setData(item);
    return r;
}

NodePtr volumeCard() {
    const Theme &t = currentTheme();
    auto card = Glass::create({.direction = Direction::row, .alignItems = Align::center, .gap = 18, .padding = Edges::all(20)},
                              {.radii = gfx::CornerRadii::uniform(t.radii.panel),
                               .cornerTicks = true});
    card->appendChild(MeterRing::create({.value = 0.62f, .label = "Internal storage used", .size = 112}));
    auto texts = Box::create({.alignItems = Align::start, .gap = 6, .grow = 1, .shrink = 1, .basis = 0});
    texts->appendChild(Stamp("Primary"));
    gfx::TextStyle title = t.type.title;
    texts->appendChild(Text::create("Internal", title, t.colors.ink));
    texts->appendChild(Text::create("79.2 GB free of 256 GB", t.type.label, textInk(t, t.text.label, 11)));
    texts->appendChild(Text::create("/storage/emulated/0", t.type.label, textInk(t, t.text.meta, 11)));
    card->appendChild(texts);
    return card;
}

}  // namespace

Page galleryControls() {
    auto root = screen();
    auto col = column(*root);
    col->appendChild(heading("Component kit", "01 / Buttons, controls, input"));

    col->appendChild(section("Buttons"));
    auto buttons = GlassPanel({.gap = 12, .padding = Edges::all(16)});
    buttons->appendChild(row({Button::create({.label = "Extract", .variant = ButtonVariant::primary}),
                              Button::create({.label = "Cancel", .variant = ButtonVariant::glass}),
                              Button::create({.label = "Details", .variant = ButtonVariant::outline})}));
    buttons->appendChild(row({Button::create({.label = "Delete", .variant = ButtonVariant::danger, .icon = "delete"}),
                              Button::create({.label = "Skip", .variant = ButtonVariant::text}),
                              Button::create({.label = "Paste", .variant = ButtonVariant::primary, .enabled = false})}));
    buttons->appendChild(row({IconButton::create({.icon = "search", .label = "Search"}),
                              IconButton::create({.icon = "add", .label = "New", .style = IconButtonStyle::filled}),
                              IconButton::create({.icon = "grid_view", .label = "Grid", .style = IconButtonStyle::plain,
                                                  .selected = true}),
                              IconButton::create({.icon = "sort", .label = "Sort", .style = IconButtonStyle::plain}),
                              IconButton::create({.icon = "delete", .label = "Delete", .style = IconButtonStyle::plain,
                                                  .destructive = true}),
                              IconButton::create({.icon = "share", .label = "Share", .enabled = false})},
                             8));
    col->appendChild(buttons);

    col->appendChild(section("Controls"));
    auto controls = GlassPanel({.padding = Edges::symmetric(18, 6)});
    controls->appendChild(settingLine("Show hidden files", "Dotfiles and .nomedia", Switch::create({.checked = true, .label = "Show hidden files"})));
    controls->appendChild(divider());
    controls->appendChild(settingLine("Thumbnails", "Off on metered networks", Switch::create({.checked = false, .label = "Thumbnails"})));
    controls->appendChild(divider());
    auto segmented = SegmentedControl::create({.options = {"List", "Grid", "Gallery"}, .selected = std::size_t{1},
                                               .label = "View"});
    segmented->updateStyle([](Style &s) { s.margin = {0, 12, 0, 8}; });
    controls->appendChild(segmented);
    auto sliderLine = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 12});
    sliderLine->appendChild(Icon::create("text_fields", 20, ink(currentTheme(), 0.7f)));
    auto slider = Slider::create({.value = 0.62f, .label = "Text size"});
    slider->updateStyle([](Style &s) {
        s.grow = 1;
        s.shrink = 1;
    });
    sliderLine->appendChild(slider);
    sliderLine->appendChild(Text::create("62%", currentTheme().type.label, textInk(currentTheme(), 0.74f, 11)));
    controls->appendChild(sliderLine);
    col->appendChild(controls);

    col->appendChild(section("Input"));
    auto name = TextField::create({.label = "File name",
                                   .text = "holiday-2024-final.jpg",
                                   .icon = "edit",
                                   .helper = "Letters, numbers, - and _"});
    col->appendChild(name);
    auto host = TextField::create({.label = "Host",
                                   .text = "nas.local:2222",
                                   .icon = "dns",
                                   .error = "Connection refused"});
    col->appendChild(host);
    EditableText *editor = &name->editor();
    return {root, [editor] {
                editor->requestFocus();
                // "holiday" selected, as after a double tap.
                editor->setSelection(0, 7);
            }};
}

Page galleryMarks() {
    auto root = screen(BackgroundProps{.grid = true});
    auto col = column(*root);
    col->appendChild(heading("Signal marks", "02 / Tags, marks, rows"));

    col->appendChild(section("Tags"));
    col->appendChild(row({Stamp("Online"), Stamp("2 open", StampTone::secondary), Stamp("SIG-0x7F3A", StampTone::ink),
                          Stamp("Failed", StampTone::destructive, "error"), OutlineTag("Read only")},
                         8));
    col->appendChild(row({GlassChip({.text = "Live", .dot = true}),
                          GlassChip({.text = "PDF", .icon = "picture_as_pdf", .selected = true}),
                          GlassChip({.text = "Images", .icon = "image"}), BarcodeTag("SFTP-7F3A")},
                         8));

    col->appendChild(section("Storage"));
    col->appendChild(volumeCard());

    col->appendChild(section("Files"));
    auto rows = Box::create({.margin = {-16, 0, -16, 0}});
    rows->appendChild(fileRow(file("Papers", "14 items · 2 Oct", FileKind::folder)));
    rows->appendChild(fileRow(file("quarterly-report-2026-q3-final-revised.pdf", "2.4 MB · 12 Oct",
                                   FileKind::pdf, true, true)));
    rows->appendChild(fileRow(file("IMG_20261012_184455.jpg", "4.1 MB · 12 Oct", FileKind::image,
                                   false, true)));
    col->appendChild(rows);

    col->appendChild(section("Marks"));
    col->appendChild(row({HazardStripes(56, 24), Barcode("FILEPAN-0x7F3A", 96, 24),
                          Text::create("N 47.61 / W 122.33  Δ 0.0031", currentTheme().type.label,
                                       textInk(currentTheme(), 0.74f, 11))},
                         14));
    col->appendChild(DataBand::create({.text = "Copying 12 of 40", .rate = "38 MB/s", .icon = "content_copy", .progress = 0.38f, .label = "Copying files"}));
    col->appendChild(DataBand::create({.text = "Archive verified · SHA-256 OK", .icon = "verified"}));
    return {root, {}};
}

Page galleryOverlays() {
    auto root = screen();
    auto col = column(*root);
    col->appendChild(heading("Containers", "03 / Hero, sheet"));
    auto empty = EmptyState({.path = "/storage/emulated/0/Download/Papers", .message = "Nothing in this folder yet"});
    empty->updateStyle([](Style &s) { s.padding.top = 32; });
    col->appendChild(empty);

    auto content = Box::create({.gap = 6, .padding = Edges::symmetric(24, 4)});
    auto label = SectionLabel("Sort by");
    label->updateStyle([](Style &s) { s.padding = {0, 4, 0, 4}; });
    content->appendChild(label);
    content->appendChild(SegmentedControl::create({.options = {"Name", "Size", "Date", "Type"}, .selected = std::size_t{2},
                                                   .label = "Sort by"}));
    content->appendChild(settingLine("Folders first", "", Switch::create({.checked = true, .label = "Folders first"})));
    content->appendChild(row({Checkbox::create({.checked = false, .text = "Descending"}), Checkbox::create({.checked = true, .text = "Group by type"})}, 16));
    auto actions = Box::create({.direction = Direction::row, .justify = Justify::end, .gap = 8, .margin = {0, 8, 0, 0}});
    actions->appendChild(Button::create({.label = "Reset", .variant = ButtonVariant::glass}));
    actions->appendChild(Button::create({.label = "Apply", .variant = ButtonVariant::primary}));
    content->appendChild(actions);
    auto sheet = BottomSheet::create({.title = "Sort", .content = content});
    root->appendChild(sheet);
    BottomSheet *raw = sheet.get();
    return {root, [raw] { raw->open(false); }};
}

Page galleryMenus() {
    auto root = screen();
    auto col = column(*root);
    col->appendChild(heading("Grid view", "04 / Tiles, menu, snackbar"));
    col->appendChild(section("Camera"));
    // The grid view as the app builds it: a three-column VirtualList of tiles.
    auto items = std::make_shared<std::vector<FileItem>>(std::vector<FileItem>{
            file("Screenshots", "", FileKind::folder), file("talk-2026.mp4", "", FileKind::video, true, true),
            file("mix-03.flac", "", FileKind::audio, false, true), file("IMG_4410.HEIC", "", FileKind::image, true, true),
            file("scan-receipt.pdf", "", FileKind::pdf, false, true), file("notes.md", "", FileKind::document, false, true),
            file("photos-2025.zip", "", FileKind::archive, false, true), file("main.cpp", "", FileKind::code, false, true),
            file("filepan.apk", "", FileKind::apk, false, true)});
    auto grid = VirtualList::create();
    grid->setStyle({.margin = {-6, 0, -6, 0}, .grow = 1, .shrink = 1, .basis = 0});
    grid->setColumns(3);
    grid->setEstimatedItemExtent(170);
    grid->setCreate([](std::uint32_t) -> NodePtr { return GridTile::create(); });
    grid->setBind([items](std::size_t i, Node &node) { static_cast<GridTile &>(node).setData((*items)[i]); });
    grid->setItemCount(items->size());
    col->appendChild(grid);

    auto menu = Menu::create({.items = {{"drive_file_rename_outline", "Rename", nullptr},
                                        {"content_copy", "Copy to…", nullptr},
                                        {"info", "Properties", nullptr},
                                        {"delete", "Delete", nullptr, true}},
                              .anchor = {300, 148, 48, 48}});
    root->appendChild(menu);
    auto snackbars = SnackbarHost::create(nullptr, 24);
    root->appendChild(snackbars);
    SnackbarHost *rawSnack = snackbars.get();
    return {root, [rawSnack] { rawSnack->show({.text = "3 files moved to Trash", .action = "Undo"}); }};
}

Page galleryDialog() {
    auto root = screen();
    // A list behind, so the scrim and blur have something to work on.
    auto list = Box::create({.padding = {0, 40, 0, 0}});
    auto title = heading("Dialog", "05 / Conflict");
    title->updateStyle([](Style &s) { s.padding.left = 20; });
    list->appendChild(title);
    list->appendChild(SectionLabel("Files"));
    const FileKind kinds[] = {FileKind::pdf, FileKind::image, FileKind::code, FileKind::apk, FileKind::document,
                              FileKind::video, FileKind::audio, FileKind::archive, FileKind::image, FileKind::other};
    const char *names[] = {"invoice-0931.pdf", "IMG_4410.HEIC", "main.cpp", "filepan-0.4.apk", "notes.md",
                           "trailer.mkv", "voice-memo.m4a", "photos-2025.zip", "screenshot.png", "LICENSE"};
    for (int i = 0; i < 10; ++i) {
        list->appendChild(fileRow(file(names[i], "18 KB · 1 Oct", kinds[i])));
    }
    root->appendChild(list);

    auto dialog = Dialog::create({.title = "Replace file?",
                                  .message = "“notes.md” already exists in Documents. Replacing it overwrites "
                                             "its contents.",
                                  .content = row({Checkbox::create({.checked = true, .text = "Apply to all 3 conflicts"})}),
                                  .actions = {Button::create({.label = "Skip", .variant = ButtonVariant::text}),
                                              Button::create({.label = "Replace", .variant = ButtonVariant::danger})}});
    root->appendChild(dialog);
    Dialog *rawDialog = dialog.get();
    return {root, [rawDialog] { rawDialog->open(false); }};
}


Page browsingMock() {
    const Theme &t = currentTheme();
    auto root = screen();

    struct Entry {
        bool header = false;
        int section = 0;
        std::string label;
        FileItem item;
    };
    auto entries = std::make_shared<std::vector<Entry>>();
    entries->push_back({true, 1, "Folders", {}});
    entries->push_back({false, 0, {}, file("Conferences", "8 items · 3 Oct", FileKind::folder)});
    entries->push_back({false, 0, {}, file("Papers", "14 items · 2 Oct", FileKind::folder)});
    entries->push_back({false, 0, {}, file("Receipts", "112 items · 28 Sep", FileKind::folder, true, true)});
    entries->push_back({true, 2, "Files", {}});
    entries->push_back({false, 0, {}, file("quarterly-report-2026-q3-final-revised.pdf",
                                           "2.4 MB · 12 Oct", FileKind::pdf, true, true)});
    entries->push_back({false, 0, {}, file("IMG_20261012_184455.jpg", "4.1 MB · 12 Oct",
                                           FileKind::image, false, true)});
    entries->push_back({false, 0, {}, file("keynote-recording.mp4", "812 MB · 11 Oct",
                                           FileKind::video, true, true)});
    entries->push_back({false, 0, {}, file("dataset-v4.tar.zst", "1.2 GB · 9 Oct",
                                           FileKind::archive, false, true)});
    entries->push_back({false, 0, {}, file("interview-notes.md", "18 KB · 8 Oct",
                                           FileKind::document, false, true)});
    entries->push_back({false, 0, {}, file("filepan-0.4.0-arm64.apk", "24.6 MB · 6 Oct",
                                           FileKind::apk, false, true)});
    entries->push_back({false, 0, {}, file("podcast-ep12.flac", "96 MB · 2 Oct",
                                           FileKind::audio, false, true)});
    entries->push_back({false, 0, {}, file("render.cpp", "42 KB · 1 Oct", FileKind::code,
                                           false, true)});

    auto list = VirtualList::create();
    list->setStyle({.position = Position::absolute, .left = 0, .top = 0, .right = 0, .bottom = 0});
    list->setEstimatedItemExtent(t.spacing.listRow);
    list->setItemType([entries](std::size_t i) { return (*entries)[i].header ? 1u : 0u; });
    list->setCreate([](std::uint32_t type) -> NodePtr {
        if (type == 1) {
            return Box::create({.minHeight = 40});
        }
        return FileRow::create();
    });
    list->setBind([entries](std::size_t i, Node &node) {
        const Entry &e = (*entries)[i];
        if (e.header) {
            node.clearChildren();
            node.appendChild(SectionLabel(e.label));
        } else {
            static_cast<FileRow &>(node).setData(e.item);
        }
    });
    list->setItemCount(entries->size());
    root->appendChild(list);

    auto second = Box::create({.direction = Direction::row, .alignItems = Align::center, .padding = {16, 0, 4, 0}});
    auto crumbs = Breadcrumb::create({.segments = std::vector<std::string>{"Internal", "Download", "Papers"}});
    crumbs->updateStyle([](Style &s) {
        s.grow = 1;
        s.shrink = 1;
        s.basis = 0;
    });
    second->appendChild(crumbs);
    second->appendChild(IconButton::create({.icon = "more_vert", .label = "More", .style = IconButtonStyle::plain, .size = 40}));
    auto bar = TopBar::create({.content = second, .scrollOffset = list->offsetSignal()});
    root->appendChild(bar);
    const float density = environment().density.peek();
    const float bottomInset = environment().safeArea.peek().bottom / density;
    list->updateStyle([&](Style &s) {
        s.padding = {0, bar->height() + 6, 0, bottomInset + t.spacing.dock + t.spacing.dockInset + 40 + 24};
    });

    root->appendChild(BottomFade(176, 112));
    auto band = DataBand::create({.text = "Copying 12 of 40", .rate = "38 MB/s", .icon = "content_copy", .progress = 0.38f, .label = "Copying files"});
    band->updateStyle([&](Style &s) {
        s.position = Position::absolute;
        s.left = 16;
        s.right = 16;
        s.bottom = bottomInset + t.spacing.dockInset + t.spacing.dock + 10;
    });
    root->appendChild(band);

    auto dock = BottomDock::create({.idle = {{"add", "New", nullptr},
                                             {"search", "Search", nullptr},
                                             {"star", "Favorites", nullptr},
                                             {"checklist", "Select", nullptr}},
                                    .selection = {{"content_copy", "Copy", nullptr},
                                                  {"drive_file_move", "Move", nullptr},
                                                  {"delete", "Delete", nullptr, true},
                                                  {"share", "Share", nullptr},
                                                  {"more_vert", "More", nullptr}},
                                    .selecting = true,
                                    .selectionCount = 3});
    root->appendChild(dock);
    return {root, {}};
}

}  // namespace filepan::test
