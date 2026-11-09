// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/about_screen.hpp"

#include <cctype>
#include <cstring>

#include "filepan/app/format.hpp"
#include "filepan/app/thumbnails.hpp"
#include "filepan/ui/components/recipes.hpp"

namespace filepan::app {

namespace {

using namespace ui;
using namespace ui::components;

constexpr const char *kFilePanLicense = "GNU Affero General Public License 3.0";

const char *abi() {
#if defined(__aarch64__)
    return "ARM64-V8A";
#elif defined(__x86_64__)
    return "X86_64";
#else
    return "OTHER";
#endif
}

// The app icon, art/icon/filepan.svg rendered to a 256 px PNG in the assets
// (rsvg-convert -w 256 -h 256). Read and decoded once per About screen on the I/O pool; the
// upload goes with the screen. Hosts have no PNG decoder, so there it stays an empty square.
constexpr const char *kIconAsset = "icon/filepan.png";

struct AppIcon {
    gfx::vulkan::ImageStore *store = nullptr;
    Signal<gfx::ImageId> image{0};
    ~AppIcon() {
        if (const gfx::ImageId id = image.peek(); id != 0) {
            store->remove(id);
        }
    }
};

std::shared_ptr<AppIcon> loadAppIcon(AppContext &context) {
    auto icon = std::make_shared<AppIcon>();
    icon->store = &context.services.images;
    const runtime::AssetSource *assets = context.services.assets;
    if (assets == nullptr) {
        return icon;
    }
    std::weak_ptr<AppIcon> weak = icon;
    context.services.io.submit(
            [assets]() -> std::optional<DecodedImage> {
                auto bytes = assets->read(kIconAsset);
                if (!bytes) {
                    return std::nullopt;
                }
                return decodeThumbnailBuffer(
                        {reinterpret_cast<const std::uint8_t *>(bytes->data()), bytes->size()}, 256);
            },
            [weak](std::optional<DecodedImage> decoded) {
                auto icon = weak.lock();
                if (!icon || !decoded) {
                    return;
                }
                icon->image.set(icon->store->add(decoded->width, decoded->height, std::move(decoded->rgba)));
            });
    return icon;
}

NodePtr appIconNode(const std::shared_ptr<AppIcon> &icon, float size) {
    auto node = Image::create();
    node->setStyle({.width = size, .height = size});
    node->updateSemantics([](Semantics &s) { s.hidden = true; });
    node->bind([n = node.get(), icon] {
        const gfx::ImageId id = icon->image();
        untrack([&] { n->setImage(id); });
    });
    return node;
}

// A link row: icon, title and a mono detail, a trailing arrow.
NodePtr linkRow(const std::string &icon, const std::string &title, const std::string &detail, bool external,
                std::function<void()> onTap) {
    const Theme &t = currentTheme();
    auto row = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 14,
                            .padding = Edges::symmetric(18, 10), .minHeight = 64});
    auto iconNode = Icon::create(icon, 22, legible(t.colors.accent, t.colors.ground));
    row->appendChild(iconNode);
    auto texts = Box::create({.gap = 3, .grow = 1, .shrink = 1, .basis = 0});
    texts->appendChild(Text::create(title, t.type.body, t.colors.ink));
    auto sub = Text::create(detail, t.type.label, textInk(t, t.text.meta, 11));
    sub->setMaxLines(1);
    sub->setOverflow(gfx::TextOverflow::ellipsisMiddle);
    texts->appendChild(sub);
    for (const auto &child : texts->children()) {
        child->updateSemantics([](Semantics &s) { s.hidden = true; });
    }
    row->appendChild(texts);
    row->appendChild(Icon::create(external ? "north_east" : "chevron_right", 20, ink(t, 0.6f)));
    auto pressable = makePressable(*row, {.onTap = std::move(onTap)});
    Box *raw = row.get();
    row->setOverlayHook([pressable](PaintContext &context) {
        if (pressable->pressed.peek()) {
            context.list.fill(gfx::RoundedRect{context.bounds(), gfx::CornerRadii::uniform(context.px(currentTheme().corner(20)))},
                              currentTheme().colors.ink.withAlpha(0.06f));
        }
    });
    row->bind([raw, pressable] {
        (void)pressable->pressed();
        untrack([&] { raw->markPaintDirty(); });
    });
    row->updateSemantics([&](Semantics &s) {
        s.role = external ? Role::button : Role::button;
        s.label = title + ", " + detail;
    });
    return row;
}

// One "KEY  value" coordinate line.
NodePtr coordinate(const std::string &key, const std::string &value) {
    const Theme &t = currentTheme();
    auto row = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 12});
    auto k = Text::create(key, t.type.label, textInk(t, t.text.meta, 11));
    k->updateStyle([](Style &s) { s.width = 44; });
    row->appendChild(k);
    gfx::TextStyle style = t.type.label;
    style.size = 13;
    style.weight = 600;
    auto v = Text::create(value, style, t.colors.ink);
    v->setMaxLines(1);
    v->updateStyle([](Style &s) {
        s.grow = 1;
        s.shrink = 1;
        s.basis = 0;
    });
    row->appendChild(v);
    row->updateSemantics([&](Semantics &s) { s.label = key + " " + value; });
    k->updateSemantics([](Semantics &s) { s.hidden = true; });
    v->updateSemantics([](Semantics &s) { s.hidden = true; });
    return row;
}

NodePtr buildAbout(AppContext &context, std::shared_ptr<std::optional<LicenseChoice>> choice) {
    auto page = scrollPage(context, 12);
    Node *column = page.column.get();
    AppContext *ctx = &context;
    page.screen->setDebugName("AboutScreen");
    auto icon = loadAppIcon(context);
    const auto rebuild = [column, ctx, choice, icon] {
        column->clearChildren();
        const Theme &t = currentTheme();
        const std::string version = ctx->services.version.empty() ? std::string("dev") : ctx->services.version;
        const std::string build = ctx->services.build.empty() ? std::string("local") : ctx->services.build;

        // The hero: the app icon, the wordmark, the line under it.
        auto hero = Glass::create({.gap = 16, .padding = {22, 26, 22, 22}, .margin = {0, 10, 0, 0}},
                                  {.radii = gfx::CornerRadii::uniform(t.radii.panel),
                                   .cornerTicks = true});
        auto top = Box::create({.direction = Direction::row, .justify = Justify::spaceBetween, .alignItems = Align::start});
        // The utilitarian style leaves out the hazard band's art, and sets the name plainly.
        const bool plain = t.style.utilitarian;
        top->appendChild(appIconNode(icon, 68));
        auto stamps = Box::create({.alignItems = Align::end, .gap = 6});
        stamps->appendChild(Stamp("v" + version));
        stamps->appendChild(Stamp("AGPL-3.0+", StampTone::ink));
        top->appendChild(stamps);
        hero->appendChild(top);
        const gfx::TextStyle word = plain ? t.type.display : withSize(t.type.display, 64);
        auto wordmark = Text::create("FilePan", word, t.colors.ink);
        wordmark->updateSemantics([](Semantics &s) { s.role = Role::heading; });
        hero->appendChild(wordmark);
        hero->appendChild(Text::create("A fast, native file manager. Local storage, archives and SFTP, drawn on one "
                                       "Vulkan surface.",
                                       t.type.bodySmall, ink(t, t.text.body)));
        // The hazard band: stripes, a mono line, the barcode.
        auto band = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 12,
                                 .margin = {0, 6, 0, 0}, .height = 28});
        if (!plain) {
            band->appendChild(HazardStripes(64, 26));
        }
        auto bandText = Text::create("Build " + build + " · " + abi(), t.type.label, textInk(t, t.text.label, 11));
        bandText->updateStyle([](Style &s) {
            s.grow = 1;
            s.shrink = 1;
            s.basis = 0;
        });
        bandText->setMaxLines(1);
        band->appendChild(bandText);
        if (!plain) {
            band->appendChild(Barcode("filepan-" + version + "-" + build, 96, 26));
        }
        hero->appendChild(band);
        column->appendChild(hero);

        // Coordinates
        column->appendChild(pageSection("Coordinates"));
        auto coords = GlassPanel({.gap = 10, .padding = Edges::all(20)});
        coords->appendChild(coordinate("VER", version));
        coords->appendChild(coordinate("BLD", build));
        coords->appendChild(coordinate("ABI", abi()));
        coords->appendChild(coordinate("ID", "app.phsp.filepan"));
        coords->appendChild(coordinate("SIG", "FP-" + shortCode(version + build).substr(2) + " / " + shortCode(abi()).substr(2)));
        column->appendChild(coords);

        // Links
        column->appendChild(pageSection("Links"));
        auto links = GlassPanel({.padding = Edges::symmetric(0, 6)});
        runtime::Platform *platform = &ctx->services.platform;
        links->appendChild(linkRow("code", "Source code", "github.com/PoonHarpoon/FilePan", true,
                                   [platform] { platform->openUrl(kSourceUrl); }));
        links->appendChild(divider());
        AppState *state = &ctx->state;
        links->appendChild(linkRow("gavel", "License", "GNU Affero GPL 3.0 or later", false, [state, choice] {
            *choice = LicenseChoice{.title = "FilePan",
                                    .license = kFilePanLicense,
                                    .url = kSourceUrl,
                                    .copyright = "Copyright 2026 FilePan contributors"};
            state->openScreen(kLicenseScreen);
        }));
        links->appendChild(divider());
        links->appendChild(linkRow("receipt_long", "Open source licenses", "Notices for the code FilePan ships", false,
                                   [state] { state->openScreen(kLicensesScreen); }));
        column->appendChild(links);

        auto footer = Text::create("© 2026 FilePan contributors\n"
                                   "Based on the work done on Material Files by Hai Zhang, unassociated with this project",
                                   t.type.label, textInk(t, t.text.meta, 11));
        footer->updateStyle([](Style &s) { s.margin = {6, 10, 6, 0}; });
        column->appendChild(footer);
    };
    page.screen->build([&] {
        page.screen->bind([rebuild] {
            (void)theme();
            untrack(rebuild);
        });
    });
    return page.screen;
}

// The text split at blank lines, so each paragraph lays out (and is culled) on its own. License
// files are hard-wrapped near 70 columns; their lines are joined so the text rewraps to the
// screen.
std::vector<std::string> paragraphs(std::string_view text) {
    std::vector<std::string> out;
    std::string current;
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t end = std::min(text.find('\n', at), text.size());
        std::string_view line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const bool blank = line.find_first_not_of(" \t\f") == std::string_view::npos;
        if (blank) {
            if (!current.empty()) {
                out.push_back(std::move(current));
                current.clear();
            }
        } else {
            const std::size_t start = line.find_first_not_of(" \t");
            line.remove_prefix(start);
            if (!current.empty()) {
                current += ' ';
            }
            current += line;
        }
        at = end + 1;
    }
    if (!current.empty()) {
        out.push_back(std::move(current));
    }
    return out;
}

}  // namespace

Result<Licenses> loadLicenses(const runtime::AssetSource *assets) {
    if (assets == nullptr) {
        return fail(ENOENT, "no bundled assets");
    }
    auto xml = assets->read(kLicensesFile);
    if (!xml) {
        return std::unexpected(xml.error());
    }
    return parseLicenses(*xml);
}

// --- Licenses ---

void LicensesScreen::construct(std::shared_ptr<std::optional<LicenseChoice>> choice) {
    setDebugName("LicensesScreen");
    setStyle({.position = Position::absolute, .left = 0, .top = 0, .right = 0, .bottom = 0});
    choice_ = std::move(choice);
    page_ = scrollPage(context_, 12);
    appendChild(page_.screen);
    LicensesScreen *self = this;
    build([&] {
        bind([self] {
            (void)self->licenses_();
            (void)theme();
            untrack([&] { self->rebuild(); });
        });
    });
    const runtime::AssetSource *assets = context_.services.assets;
    std::weak_ptr<Node> weak = weak_from_this();
    context_.services.io.submit([assets] { return loadLicenses(assets); }, [weak, self](Result<Licenses> loaded) {
        if (weak.expired()) {
            return;
        }
        Loaded<Licenses> value{.done = true};
        if (loaded) {
            value.value = std::move(*loaded);
        } else {
            value.error = loaded.error().message.empty() ? std::strerror(loaded.error().code) : loaded.error().message;
        }
        self->licenses_.set(std::move(value));
    });
}

void LicensesScreen::open(const Notice &notice) {
    *choice_ = LicenseChoice{.title = notice.name, .license = notice.license, .url = notice.url, .copyright = notice.copyright};
    context_.state.openScreen(kLicenseScreen);
}

void LicensesScreen::rebuild() {
    Node &column = *page_.column;
    column.clearChildren();
    rows_.clear();
    const Theme &t = currentTheme();
    const Loaded<Licenses> &loaded = licenses_.peek();

    auto header = Box::create({.gap = 6, .padding = {4, 8, 4, 4}});
    auto title = Text::create("Licenses", t.type.display, t.colors.ink);
    title->updateSemantics([](Semantics &s) { s.role = Role::heading; });
    header->appendChild(title);
    const std::string count = loaded.done && loaded.error.empty()
                                      ? std::to_string(loaded.value.notices.size()) + " components · " +
                                                std::to_string(loaded.value.texts.size()) + " licenses"
                                      : std::string("Open source notices");
    header->appendChild(Text::create(count, t.type.label, textInk(t, t.text.label, 11)));
    column.appendChild(header);

    if (!loaded.done) {
        return;
    }
    if (!loaded.error.empty()) {
        auto panel = GlassPanel({.alignItems = Align::start, .gap = 10, .padding = Edges::all(20)});
        panel->appendChild(Stamp("Unavailable", StampTone::destructive));
        panel->appendChild(Text::create(loaded.error, monoText(t, 12), ink(t, t.text.body)));
        column.appendChild(panel);
        return;
    }
    column.appendChild(pageSection("Components"));
    auto list = GlassPanel({.padding = Edges::symmetric(0, 6)});
    LicensesScreen *self = this;
    const auto &notices = loaded.value.notices;
    for (std::size_t i = 0; i < notices.size(); ++i) {
        const Notice notice = notices[i];
        if (i > 0) {
            list->appendChild(divider());
        }
        auto row = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 12,
                                .padding = Edges::symmetric(18, 12)});
        auto texts = Box::create({.alignItems = Align::start, .gap = 5, .grow = 1, .shrink = 1, .basis = 0});
        texts->appendChild(Text::create(notice.name, withWeight(t.type.body, 600), t.colors.ink));
        auto copyright = Text::create(notice.copyright, t.type.bodySmall, ink(t, t.text.label));
        copyright->setMaxLines(2);
        texts->appendChild(copyright);
        texts->appendChild(OutlineTag(notice.license));
        for (const auto &child : texts->children()) {
            child->updateSemantics([](Semantics &s) { s.hidden = true; });
        }
        row->appendChild(texts);
        row->appendChild(Icon::create("chevron_right", 20, ink(t, 0.6f)));
        auto pressable = makePressable(*row, {.onTap = [self, notice] { self->open(notice); }});
        Box *raw = row.get();
        row->setOverlayHook([pressable](PaintContext &context) {
            if (pressable->pressed.peek()) {
                context.list.fill(context.bounds(), currentTheme().colors.ink.withAlpha(0.06f));
            }
        });
        row->bind([raw, pressable] {
            (void)pressable->pressed();
            untrack([&] { raw->markPaintDirty(); });
        });
        row->updateSemantics([&](Semantics &s) {
            s.role = Role::button;
            s.label = notice.name + ", " + notice.license;
        });
        rows_.push_back(row.get());
        list->appendChild(row);
    }
    column.appendChild(list);
}

// --- One license ---

void LicenseScreen::construct(std::optional<LicenseChoice> choice) {
    setDebugName("LicenseScreen");
    setStyle({.position = Position::absolute, .left = 0, .top = 0, .right = 0, .bottom = 0});
    choice_ = std::move(choice);
    page_ = scrollPage(context_, 12);
    appendChild(page_.screen);
    LicenseScreen *self = this;
    build([&] {
        bind([self] {
            (void)self->text_();
            (void)theme();
            untrack([&] { self->rebuild(); });
        });
    });
    const runtime::AssetSource *assets = context_.services.assets;
    const std::string license = choice_ ? choice_->license : std::string();
    std::weak_ptr<Node> weak = weak_from_this();
    context_.services.io.submit(
            [assets, license]() -> Result<std::string> {
                auto licenses = loadLicenses(assets);
                if (!licenses) {
                    return std::unexpected(licenses.error());
                }
                const std::string file = licenses->fileFor(license);
                if (file.empty()) {
                    return fail(ENOENT, "No text for “" + license + "”");
                }
                return assets->read(std::string(kLicensesDir) + "/" + file);
            },
            [weak, self](Result<std::string> text) {
                if (weak.expired()) {
                    return;
                }
                Loaded<std::string> value{.done = true};
                if (text) {
                    value.value = std::move(*text);
                } else {
                    value.error = text.error().message.empty() ? std::strerror(text.error().code) : text.error().message;
                }
                self->text_.set(std::move(value));
            });
}

void LicenseScreen::rebuild() {
    Node &column = *page_.column;
    column.clearChildren();
    const Theme &t = currentTheme();
    const LicenseChoice choice = choice_.value_or(LicenseChoice{});

    auto head = Glass::create({.alignItems = Align::start, .gap = 10, .padding = Edges::all(20)},
                              {.radii = gfx::CornerRadii::uniform(t.radii.panel)});
    auto title = Text::create(choice.title, t.type.title, t.colors.ink);
    title->updateSemantics([](Semantics &s) { s.role = Role::heading; });
    head->appendChild(title);
    head->appendChild(Stamp(choice.license));
    if (!choice.copyright.empty()) {
        head->appendChild(Text::create(choice.copyright, t.type.bodySmall, ink(t, t.text.body)));
    }
    if (!choice.url.empty()) {
        auto url = Text::create(choice.url, monoText(t, 12), legible(t.colors.accent, t.colors.ground));
        url->setMaxLines(1);
        url->setOverflow(gfx::TextOverflow::ellipsisMiddle);
        runtime::Platform *platform = &context_.services.platform;
        auto pressable = makePressable(*url, {.onTap = [platform, link = choice.url] { platform->openUrl(link); }});
        (void)pressable;
        url->updateSemantics([&](Semantics &s) {
            s.role = Role::button;
            s.label = choice.url;
        });
        head->appendChild(url);
    }
    column.appendChild(head);

    const Loaded<std::string> &loaded = text_.peek();
    column.appendChild(pageSection("Full text"));
    auto well = Box::create({.gap = 12, .padding = Edges::all(18)});
    well->setPaintHook([](PaintContext &context) {
        const Theme &th = currentTheme();
        recipes::sunken(context.list, {context.bounds(), gfx::CornerRadii::uniform(context.px(th.radii.input))}, th,
                        context.density);
    });
    if (!loaded.done) {
        well->appendChild(Text::create("Reading…", t.type.label, textInk(t, t.text.meta, 11)));
    } else if (!loaded.error.empty()) {
        well->appendChild(Stamp("Unavailable", StampTone::destructive));
        well->appendChild(Text::create(loaded.error, monoText(t, 12), ink(t, t.text.body)));
    } else {
        const gfx::TextStyle style = t.type.bodySmall;
        for (const std::string &paragraph : paragraphs(loaded.value)) {
            well->appendChild(Text::create(paragraph, style, ink(t, t.text.body)));
        }
    }
    column.appendChild(well);
}

// --- Registration ---

std::vector<ScreenSpec> aboutScreenSpecs() {
    auto choice = std::make_shared<std::optional<LicenseChoice>>();
    return {
            {.id = kAboutScreen,
             .title = "About",
             .icon = "info",
             .build = [choice](AppContext &context) { return buildAbout(context, choice); }},
            {.id = kLicensesScreen,
             .title = "Licenses",
             .icon = "receipt_long",
             .build =
                     [choice](AppContext &context) {
                         auto node = std::make_shared<LicensesScreen>(context);
                         node->construct(choice);
                         return NodePtr(node);
                     },
             .inMenu = false},
            {.id = kLicenseScreen,
             .title = "License",
             .icon = "gavel",
             .build =
                     [choice](AppContext &context) {
                         auto node = std::make_shared<LicenseScreen>(context);
                         node->construct(*choice);
                         return NodePtr(node);
                     },
             .inMenu = false},
    };
}

}  // namespace filepan::app
