// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/settings_screen.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "filepan/app/about_screen.hpp"
#include "filepan/app/backup_sheets.hpp"
#include "filepan/app/hidden_items_screen.hpp"
#include "filepan/app/format.hpp"
#include "filepan/app/screen_parts.hpp"
#include "filepan/app/sheet_parts.hpp"
#include "filepan/ui/components/recipes.hpp"

namespace filepan::app {

namespace {

using namespace ui;
using namespace ui::components;

struct Swatch {
    const char *name;
    std::optional<gfx::Color> color;
};

const std::array<Swatch, 7> kSwatches = {{
        {"Preset", std::nullopt},
        {"Magenta", gfx::Color::fromRgb(0xFF3DBB)},
        {"Violet", gfx::Color::fromRgb(0x9B6BFF)},
        {"Blue", gfx::Color::fromRgb(0x3D8BFF)},
        {"Teal", gfx::Color::fromRgb(0x2DD4BF)},
        {"Lime", gfx::Color::fromRgb(0xC6F432)},
        {"Amber", gfx::Color::fromRgb(0xFFB547)},
}};

bool sameColor(std::optional<gfx::Color> a, std::optional<gfx::Color> b) {
    if (!a || !b) {
        return !a && !b;
    }
    return colorToHex(*a) == colorToHex(*b);
}

bool isSwatch(std::optional<gfx::Color> accent) {
    for (const Swatch &s : kSwatches) {
        if (sameColor(s.color, accent)) {
            return true;
        }
    }
    return false;
}

gfx::RoundedRect circleIn(const gfx::Rect &b) { return {b, gfx::CornerRadii::uniform(b.width / 2)}; }

// One accent swatch: a 34 dp disc in a 44 dp target, ringed when it's the current accent.
NodePtr swatch(AppState &state, const Swatch &swatch) {
    auto node = Box::create({.width = 40, .height = 40});
    AppState *s = &state;
    const std::optional<gfx::Color> color = swatch.color;
    auto pressable = makePressable(*node, {.onTap = [s, color] {
                                               s->updateSettings([&](Settings &settings) { settings.accent = color; });
                                           },
                                           .haptic = Haptic::tick});
    Box *raw = node.get();
    node->setPaintHook([color, pressable, s](PaintContext &context) {
        const Theme &t = currentTheme();
        const gfx::Rect b = context.bounds().inset(context.px(4));
        const gfx::Color fill = color ? *color : makeTheme(t.preset).colors.accent;
        recipes::raised(context.list, circleIn(b), fill, context.density);
        if (!color) {
            // "Preset": the preset's own accent with a diagonal hairline through it.
            const float w = context.px(2);
            context.list.pushClip(circleIn(b));
            const float k = 0.70710678f;
            context.list.pushTransform(gfx::Transform{k, -k, k, k, b.x + b.width / 2, b.y + b.height / 2});
            context.list.fill(gfx::Rect{-b.width, -w / 2, b.width * 2, w}, t.colors.ground.withAlpha(0.8f));
            context.list.pop();
            context.list.pop();
        }
        if (sameColor(s->settings.peek().accent, color)) {
            context.list.stroke(circleIn(context.bounds().inset(context.px(0.5f))), context.px(2), t.colors.ink);
        }
        if (pressable->pressed.peek()) {
            context.list.fill(circleIn(b), gfx::Color{0, 0, 0, 0.2f});
        }
    });
    node->bind([raw, pressable, s, color, name = std::string(swatch.name)] {
        const bool on = sameColor(s->settings().accent, color);
        (void)pressable->pressed();
        untrack([&] {
            raw->markPaintDirty();
            raw->updateSemantics([&](Semantics &sem) {
                sem.role = Role::radioButton;
                sem.label = name + " accent";
                sem.flags = semantics_flags::kCheckable | (on ? semantics_flags::kChecked : 0);
            });
        });
    });
    return node;
}

// The custom swatch: a hue wheel (or the custom accent, when one is set) that opens the picker.
NodePtr customSwatch(AppContext &context) {
    auto node = Box::create({.justify = Justify::center, .alignItems = Align::center, .width = 40, .height = 40});
    AppContext *ctx = &context;
    auto pressable = makePressable(*node, {.onTap = [ctx] { showAccentPicker(*ctx); }});
    AppState *s = &context.state;
    Box *raw = node.get();
    auto icon = Icon::create("tune", 18, gfx::Color{1, 1, 1, 1});
    icon->updateSemantics([](Semantics &sem) { sem.hidden = true; });
    node->appendChild(icon);
    node->setPaintHook([s, pressable](PaintContext &context) {
        const gfx::Rect b = context.bounds().inset(context.px(4));
        const auto accent = s->settings.peek().accent;
        if (accent && !isSwatch(accent)) {
            recipes::raised(context.list, circleIn(b), *accent, context.density);
            context.list.stroke(circleIn(context.bounds().inset(context.px(0.5f))), context.px(2),
                                currentTheme().colors.ink);
        } else {
            const gfx::Vec2 c{b.x + b.width / 2, b.y + b.height / 2};
            context.list.fill(circleIn(b), gfx::ConicGradient{c, 0,
                                                              {{0, gfx::Color::fromRgb(0xFF4F7A)},
                                                               {0.33f, gfx::Color::fromRgb(0x4FFFB0)},
                                                               {0.66f, gfx::Color::fromRgb(0x4F7AFF)},
                                                               {1, gfx::Color::fromRgb(0xFF4F7A)}}});
            context.list.fill(circleIn(b.inset(context.px(7))), currentTheme().colors.groundDeep);
        }
        if (pressable->pressed.peek()) {
            context.list.fill(circleIn(b), gfx::Color{0, 0, 0, 0.2f});
        }
    });
    node->bind([raw, s, pressable, icon = icon.get()] {
        const auto accent = s->settings().accent;
        (void)pressable->pressed();
        untrack([&] {
            raw->markPaintDirty();
            const bool custom = accent && !isSwatch(accent);
            icon->setColor(custom ? makeTheme(currentTheme().preset, accent).colors.onAccent
                                  : currentTheme().colors.ink);
            raw->updateSemantics([&](Semantics &sem) {
                sem.role = Role::button;
                sem.label = custom ? "Custom accent " + colorToHex(*accent) + ", change" : "Custom accent";
            });
        });
    });
    return node;
}

std::string accentLine(const Settings &s) {
    return s.accent ? colorToHex(*s.accent) : std::string("Preset");
}

std::string cacheSizeText(std::uint32_t size) { return std::to_string(size) + " " + kThumbnailCacheUnitName; }

std::string decayText(std::uint32_t days) {
    if (days == 0) {
        return "Never";
    }
    if (days == 7) {
        return "1 week";
    }
    return std::to_string(days) + (days == 1 ? " day" : " days");
}

std::string prefetchText(std::uint32_t files) {
    if (files == 0) {
        return "Off";
    }
    return std::to_string(files) + (files == 1 ? " file" : " files");
}

std::string videoPrefetchText(std::uint32_t size) { return size == 0 ? "Off" : cacheSizeText(size); }

std::string countText(std::uint32_t count) { return std::to_string(count); }

// A setting with a few choices: an outline button showing the current one, which opens a menu
// of them.
NodePtr choiceButton(AppContext &context, const std::string &label, std::span<const std::uint32_t> choices,
                     std::string (*text)(std::uint32_t), std::uint32_t (*get)(const Settings &),
                     void (*set)(Settings &, std::uint32_t)) {
    AppState *state = &context.state;
    AppContext *ctx = &context;
    // The button itself, once it exists: the menu drops from it.
    auto anchor = std::make_shared<Node *>(nullptr);
    std::vector<std::uint32_t> options(choices.begin(), choices.end());
    auto button = Button::create(
            {.label = [state, text, get] { return text(get(state->settings())); },
             .variant = ButtonVariant::outline,
             .onTap =
                     [ctx, state, anchor, options, text, get, set] {
                         const std::uint32_t current = get(state->settings.peek());
                         std::vector<MenuItem> items;
                         for (const std::uint32_t option : options) {
                             items.push_back({.icon = option == current ? "check" : "",
                                              .label = text(option),
                                              .onSelect = [state, set, option] {
                                                  state->updateSettings([&](Settings &s) { set(s, option); });
                                              }});
                         }
                         ctx->overlays.showMenu(std::move(items), **anchor, nullptr, 0, 168);
                     },
             .accessibilityLabel = label});
    *anchor = button.get();
    button->bind([raw = button.get(), state, text, get] {
        const std::string value = text(get(state->settings()));
        untrack([&] { raw->updateSemantics([&](Semantics &s) { s.value = value; }); });
    });
    return button;
}

std::string homeTargetName(const Settings &settings) {
    const Favorite *favorite = homeFavorite(settings);
    return favorite != nullptr ? favorite->name : "Storage home";
}

// Where the home button goes: an outline button naming it, with a menu of the storage home and
// every favorite.
NodePtr homeTargetButton(AppContext &context) {
    AppState *state = &context.state;
    AppContext *ctx = &context;
    auto anchor = std::make_shared<Node *>(nullptr);
    auto button = Button::create(
            {.label = [state] { return homeTargetName(state->settings()); },
             .variant = ButtonVariant::outline,
             .onTap =
                     [ctx, state, anchor] {
                         const Settings &settings = state->settings.peek();
                         const Favorite *current = homeFavorite(settings);
                         std::vector<MenuItem> items;
                         items.push_back({.icon = current == nullptr ? "check" : "",
                                          .label = "Storage home",
                                          .onSelect = [state] {
                                              state->updateSettings([](Settings &s) { s.homeTarget.reset(); });
                                          }});
                         for (const Favorite &favorite : settings.favorites) {
                             const bool chosen = current != nullptr && current->location == favorite.location;
                             items.push_back({.icon = chosen ? "check" : "",
                                              .label = favorite.name,
                                              .onSelect = [state, location = favorite.location] {
                                                  state->updateSettings([&](Settings &s) { s.homeTarget = location; });
                                              }});
                         }
                         ctx->overlays.showMenu(std::move(items), **anchor);
                     },
             .accessibilityLabel = "Home button goes to"});
    *anchor = button.get();
    button->updateStyle([](Style &s) {
        s.shrink = 1;
        s.maxWidth = 168;
    });
    button->bind([raw = button.get(), state] {
        const std::string value = homeTargetName(state->settings());
        untrack([&] { raw->updateSemantics([&](Semantics &s) { s.value = value; }); });
    });
    return button;
}

std::string dataSaverDetail(bool on, bool metered) {
    return on && metered ? "On mobile data: previews paused" : "Pauses previews on mobile data";
}

// "Clear thumbnail cache", with what it holds now.
NodePtr clearThumbnailsLine(AppContext &context) {
    AppContext *ctx = &context;
    auto line = settingLine("Clear thumbnail cache", "Using " + formatSize(context.thumbnails.diskUsage.peek()),
                            Button::create({.label = "Clear",
                                            .variant = ButtonVariant::glass,
                                            .onTap = [ctx] {
                                                ctx->thumbnails.clearDisk(
                                                        [ctx] { ctx->notify("Thumbnail cache cleared"); });
                                            },
                                            .accessibilityLabel = "Clear thumbnail cache"}));
    auto *detail = static_cast<Text *>(line->children()[0]->children()[1].get());
    detail->bind([detail, ctx] {
        const std::string text = "Using " + formatSize(ctx->thumbnails.diskUsage());
        untrack([&] { detail->setText(text); });
    });
    return line;
}

NodePtr buildSettings(AppContext &context) {
    AppState &state = context.state;
    auto page = scrollPage(context, 10);
    page.screen->setDebugName("SettingsScreen");
    Node *column = page.column.get();
    AppContext *ctx = &context;
    const auto rebuild = [column, ctx, &state] {
        column->clearChildren();
        const Theme &t = currentTheme();

        auto header = Box::create({.gap = 6, .padding = {4, 8, 4, 4}});
        auto title = Text::create("Settings", t.type.display, t.colors.ink);
        title->updateSemantics([](Semantics &s) { s.role = Role::heading; });
        header->appendChild(title);
        header->appendChild(Text::create("Applied at once · saved on this device", t.type.label,
                                         textInk(t, t.text.label, 11)));
        column->appendChild(header);

        // Look
        column->appendChild(pageSection("Look"));
        auto look = GlassPanel({.gap = 6, .padding = {18, 14, 18, 8}});
        look->appendChild(Text::create("Theme", t.type.label, textInk(t, t.text.label, 11)));
        auto presets = themePresetControl(state);
        presets->updateStyle([](Style &s) { s.margin = {0, 4, 0, 10}; });
        look->appendChild(presets);
        look->appendChild(divider());
        auto accentHead = Box::create({.direction = Direction::row, .justify = Justify::spaceBetween,
                                       .alignItems = Align::center, .margin = {0, 12, 0, 2}});
        accentHead->appendChild(Text::create("Accent", t.type.body, t.colors.ink));
        auto hex = Text::create(accentLine(state.settings.peek()), t.type.label, textInk(t, t.text.label, 11));
        hex->bind([raw = hex.get(), &state] {
            const std::string line = accentLine(state.settings());
            untrack([&] { raw->setText(line); });
        });
        accentHead->appendChild(hex);
        look->appendChild(accentHead);
        auto swatches = accentSwatches(*ctx);
        swatches->updateStyle([](Style &s) { s.padding = {0, 6, 0, 10}; });
        look->appendChild(swatches);
        column->appendChild(look);

        // Browsing
        column->appendChild(pageSection("Browsing"));
        auto browsing = GlassPanel({.gap = 6, .padding = {18, 14, 18, 8}});
        browsing->appendChild(Text::create("Default view", t.type.label, textInk(t, t.text.label, 11)));
        auto view = SegmentedControl::create(
                {.options = {"List", "Grid"},
                 .selected = [&state] { return std::size_t{state.settings().view == ViewMode::grid ? 1u : 0u}; },
                 .onSelect = [&state](std::size_t i) {
                     state.updateSettings([&](Settings &s) { s.view = i == 1 ? ViewMode::grid : ViewMode::list; });
                 },
                 .label = "Default view"});
        view->updateStyle([](Style &s) { s.margin = {0, 4, 0, 8}; });
        browsing->appendChild(view);
        browsing->appendChild(Text::create("Sort by", t.type.label, textInk(t, t.text.label, 11)));
        auto sort = SegmentedControl::create(
                {.options = {"Name", "Size", "Date", "Type"},
                 .selected = [&state] { return static_cast<std::size_t>(state.settings().sort.key); },
                 .onSelect = [&state](std::size_t i) {
                     state.updateSettings([&](Settings &s) { s.sort.key = static_cast<vfs::SortKey>(i); });
                 },
                 .label = "Sort by"});
        sort->updateStyle([](Style &s) { s.margin = {0, 4, 0, 8}; });
        browsing->appendChild(sort);
        browsing->appendChild(divider());
        browsing->appendChild(settingLine(
                "Reverse order", "Z to A, largest, newest first",
                Switch::create({.checked = [&state] { return state.settings().sort.descending; },
                                .onChange = [&state](bool on) {
                                    state.updateSettings([&](Settings &s) { s.sort.descending = on; });
                                },
                                .label = "Reverse order"})));
        browsing->appendChild(divider());
        browsing->appendChild(settingLine(
                "Folders first", "Above files, in either order",
                Switch::create({.checked = [&state] { return state.settings().sort.directoriesFirst; },
                                .onChange = [&state](bool on) {
                                    state.updateSettings([&](Settings &s) { s.sort.directoriesFirst = on; });
                                },
                                .label = "Folders first"})));
        browsing->appendChild(divider());
        browsing->appendChild(settingLine(
                "Show hidden files", "Dot files and items you hid",
                Switch::create({.checked = [&state] { return state.settings().showHidden; },
                                .onChange = [&state](bool on) {
                                    state.updateSettings([&](Settings &s) { s.showHidden = on; });
                                },
                                .label = "Show hidden files"})));
        browsing->appendChild(hiddenItemsLink(*ctx));
        browsing->appendChild(divider());
        browsing->appendChild(settingLine("Home button", "Storage home or any favorite", homeTargetButton(*ctx)));
        browsing->appendChild(divider());
        browsing->appendChild(settingLine(
                "Thumbnails", "Previews for images and videos",
                Switch::create({.checked = [&state] { return state.settings().thumbnails; },
                                .onChange = [&state](bool on) {
                                    state.updateSettings([&](Settings &s) { s.thumbnails = on; });
                                },
                                .label = "Thumbnails"})));
        browsing->appendChild(divider());
        browsing->appendChild(settingLine(
                "Thumbnail cache size", "Kept between runs",
                choiceButton(*ctx, "Thumbnail cache size", std::span<const std::uint32_t>(kThumbnailCacheSizes),
                             cacheSizeText, [](const Settings &s) { return s.thumbnailCacheSize; },
                             [](Settings &s, std::uint32_t v) { s.thumbnailCacheSize = v; })));
        browsing->appendChild(divider());
        browsing->appendChild(settingLine(
                "Remove thumbnails not viewed for", "From the cache on this device",
                choiceButton(*ctx, "Remove thumbnails not viewed for", std::span<const std::uint32_t>(kThumbnailDecayChoices),
                             decayText, [](const Settings &s) { return s.thumbnailDecayDays; },
                             [](Settings &s, std::uint32_t v) { s.thumbnailDecayDays = v; })));
        browsing->appendChild(divider());
        browsing->appendChild(clearThumbnailsLine(*ctx));
        column->appendChild(browsing);

        // Remote files
        column->appendChild(pageSection("Remote files"));
        auto remote = GlassPanel({.gap = 6, .padding = {18, 14, 18, 8}});
        remote->appendChild(dataSaverLine(*ctx));
        remote->appendChild(divider());
        remote->appendChild(settingLine(
                "Thumbnails for remote files", "Images on SFTP servers, not kept",
                Switch::create({.checked = [&state] { return state.settings().remoteThumbnails; },
                                .onChange = [&state](bool on) {
                                    state.updateSettings([&](Settings &s) { s.remoteThumbnails = on; });
                                },
                                .label = "Thumbnails for remote files"})));
        remote->appendChild(divider());
        remote->appendChild(settingLine(
                "Largest remote file to thumbnail", "Larger ones aren't fetched",
                choiceButton(*ctx, "Largest remote file to thumbnail",
                             std::span<const std::uint32_t>(kRemoteThumbnailMaxSizes), cacheSizeText,
                             [](const Settings &s) { return s.remoteThumbnailMaxSize; },
                             [](Settings &s, std::uint32_t v) { s.remoteThumbnailMaxSize = v; })));
        remote->appendChild(divider());
        remote->appendChild(settingLine(
                "Viewer prefetch", "Files each side, fetched ahead",
                choiceButton(*ctx, "Viewer prefetch", std::span<const std::uint32_t>(kViewerPrefetchChoices),
                             prefetchText, [](const Settings &s) { return s.viewerPrefetch; },
                             [](Settings &s, std::uint32_t v) { s.viewerPrefetch = v; })));
        remote->appendChild(divider());
        remote->appendChild(settingLine(
                "Video prefetch", "The start of each video",
                choiceButton(*ctx, "Video prefetch", std::span<const std::uint32_t>(kViewerPrefetchVideoChoices),
                             videoPrefetchText, [](const Settings &s) { return s.viewerPrefetchVideoMb; },
                             [](Settings &s, std::uint32_t v) { s.viewerPrefetchVideoMb = v; })));
        remote->appendChild(divider());
        remote->appendChild(settingLine(
                "Parallel connections", "Per server, for prefetching",
                choiceButton(*ctx, "Parallel connections", std::span<const std::uint32_t>(kRemoteConnectionChoices),
                             countText, [](const Settings &s) { return s.remoteConnections; },
                             [](Settings &s, std::uint32_t v) { s.remoteConnections = v; })));
        remote->appendChild(divider());
        remote->appendChild(Text::create("Default card PIN type", t.type.label, textInk(t, t.text.label, 11)));
        auto pinType = SegmentedControl::create(
                {.options = {"# PIN", "ASCII PIN"},
                 .selected = [&state] { return std::size_t{state.settings().cardPinLetters ? 1u : 0u}; },
                 .onSelect = [&state](std::size_t i) {
                     state.updateSettings([&](Settings &s) { s.cardPinLetters = i == 1; });
                 },
                 .label = "Default card PIN type"});
        pinType->updateStyle([](Style &s) { s.margin = {0, 4, 0, 8}; });
        remote->appendChild(pinType);
        column->appendChild(remote);

        // Experimental
        const bool diskUsage = ctx->screens.find(kDiskUsageScreen) != nullptr;
        if (ctx->services.documentTrees != nullptr || diskUsage) {
            column->appendChild(pageSection("Experimental"));
            auto experimental = GlassPanel({.gap = 6, .padding = {18, 14, 18, 8}});
            if (ctx->services.documentTrees != nullptr) {
                experimental->appendChild(settingLine(
                        "System storage (SAF)", "Folders from the system's picker, on the storage home",
                        Switch::create({.checked = [&state] { return state.settings().documentTrees; },
                                        .onChange = [&state](bool on) {
                                            state.updateSettings([&](Settings &s) { s.documentTrees = on; });
                                        },
                                        .label = "System storage (SAF)"})));
            }
            if (diskUsage) {
                experimental->appendChild(settingLine(
                        "Disk usage", "A map of what takes the space in a folder, from the menu",
                        Switch::create({.checked = [&state] { return state.settings().diskUsage; },
                                        .onChange = [&state](bool on) {
                                            state.updateSettings([&](Settings &s) { s.diskUsage = on; });
                                        },
                                        .label = "Disk usage"})));
            }
            column->appendChild(experimental);
        }

        // FilePan
        column->appendChild(pageSection("FilePan"));
        auto about = GlassPanel({.padding = Edges::symmetric(0, 6)});
        const auto action = [&](const char *icon, const char *title, const char *detail, std::function<void()> onTap) {
            auto row = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 14,
                                    .padding = Edges::symmetric(18, 8), .minHeight = 60});
            row->appendChild(Icon::create(icon, 22, legible(t.colors.accent, t.colors.ground)));
            auto texts = Box::create({.gap = 3, .grow = 1, .shrink = 1, .basis = 0});
            texts->appendChild(Text::create(title, t.type.body, t.colors.ink));
            texts->appendChild(Text::create(detail, t.type.label, textInk(t, t.text.meta, 11)));
            for (const auto &child : texts->children()) {
                child->updateSemantics([](Semantics &s) { s.hidden = true; });
            }
            row->appendChild(texts);
            row->appendChild(Icon::create("chevron_right", 20, ink(t, 0.6f)));
            makePressable(*row, {.onTap = std::move(onTap)});
            row->updateSemantics([&](Semantics &s) {
                s.role = Role::button;
                s.label = title;
            });
            return row;
        };
        const auto link = [&](const char *icon, const char *title, const char *detail, const char *screen) {
            return action(icon, title, detail, [&state, id = std::string(screen)] { state.openScreen(id); });
        };
        about->appendChild(action("backup", "Back up settings", "Theme, favorites, servers", [ctx] { showBackupSheet(*ctx); }));
        about->appendChild(divider());
        about->appendChild(action("settings_backup_restore", "Restore settings", "From a backup file",
                                  [ctx] { startRestore(*ctx); }));
        about->appendChild(divider());
        about->appendChild(action("waving_hand", "Show the intro again", "Permissions, look, remote files",
                                  [ctx] { ctx->showOnboarding(); }));
        about->appendChild(divider());
        about->appendChild(link("info", "About FilePan", "Version, build, source", kAboutScreen));
        about->appendChild(divider());
        about->appendChild(link("receipt_long", "Open source licenses", "Notices and full texts", kLicensesScreen));
        column->appendChild(about);
    };
    page.screen->build([&] {
        page.screen->bind([rebuild] {
            (void)theme();
            untrack(rebuild);
        });
    });
    return page.screen;
}

// --- The accent picker ---

std::string degrees(float h) { return std::to_string(static_cast<int>(std::lround(h))) + "°"; }
std::string percentText(float v) { return std::to_string(static_cast<int>(std::lround(v * 100))) + "%"; }

std::string ratioText(float ratio) {
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "%.1f:1", static_cast<double>(ratio));
    return buffer;
}

// A horizontal strip of n colors, blended stop to stop (gradients take at most 4 stops).
void paintStops(gfx::DisplayList &list, const gfx::RoundedRect &track, const std::vector<gfx::Color> &colors) {
    const gfx::Rect r = track.rect;
    list.pushClip(track);
    const std::size_t segments = colors.size() - 1;
    for (std::size_t i = 0; i < segments; ++i) {
        const float x0 = r.x + r.width * static_cast<float>(i) / static_cast<float>(segments);
        const float x1 = r.x + r.width * static_cast<float>(i + 1) / static_cast<float>(segments);
        // Overlap by a pixel so no seams show.
        const gfx::Rect seg{std::floor(x0), r.y, std::ceil(x1) - std::floor(x0) + 1, r.height};
        list.fill(seg, recipes::horizontalGradient(seg, {{0, colors[i]}, {1, colors[i + 1]}}));
    }
    list.pop();
    // A hairline edge so light ends stay visible on the sheet.
    list.stroke(track, 1, gfx::Color{1, 1, 1, 0.18f});
}

}  // namespace

NodePtr themePresetControl(AppState &state) {
    return SegmentedControl::create(
            {.options = {"Ultraviolet", "Acid", "Clean"},
             .selected = [&state] { return static_cast<std::size_t>(state.settings().preset); },
             .onSelect = [&state](std::size_t i) {
                 state.updateSettings([&](Settings &s) { s.preset = static_cast<ThemePreset>(i); });
             },
             .label = "Theme"});
}

NodePtr accentSwatches(AppContext &context) {
    auto swatches = Box::create({.direction = Direction::row, .justify = Justify::spaceBetween, .alignItems = Align::center});
    for (const Swatch &s : kSwatches) {
        swatches->appendChild(swatch(context.state, s));
    }
    swatches->appendChild(customSwatch(context));
    return swatches;
}

NodePtr dataSaverLine(AppContext &context) {
    AppState &state = context.state;
    AppContext *ctx = &context;
    auto line = settingLine("Mobile data mode", dataSaverDetail(state.settings.peek().dataSaver, context.services.metered.peek()),
                            Switch::create({.checked = [&state] { return state.settings().dataSaver; },
                                            .onChange = [&state](bool on) {
                                                state.updateSettings([&](Settings &s) { s.dataSaver = on; });
                                            },
                                            .label = "Mobile data mode"}));
    auto *detail = static_cast<Text *>(line->children()[0]->children()[1].get());
    detail->bind([detail, ctx] {
        const std::string text = dataSaverDetail(ctx->state.settings().dataSaver, ctx->services.metered());
        untrack([&] { detail->setText(text); });
    });
    return line;
}

Hsl toHsl(gfx::Color c) {
    const float maxV = std::max({c.r, c.g, c.b});
    const float minV = std::min({c.r, c.g, c.b});
    Hsl out;
    out.l = (maxV + minV) / 2;
    const float d = maxV - minV;
    if (d < 1e-6f) {
        return out;
    }
    out.s = out.l > 0.5f ? d / (2 - maxV - minV) : d / (maxV + minV);
    float h;
    if (maxV == c.r) {
        h = (c.g - c.b) / d + (c.g < c.b ? 6 : 0);
    } else if (maxV == c.g) {
        h = (c.b - c.r) / d + 2;
    } else {
        h = (c.r - c.g) / d + 4;
    }
    out.h = h * 60;
    return out;
}

gfx::Color fromHsl(Hsl hsl) {
    const float h = std::fmod(std::fmod(hsl.h, 360.0f) + 360.0f, 360.0f) / 360.0f;
    const float s = std::clamp(hsl.s, 0.0f, 1.0f);
    const float l = std::clamp(hsl.l, 0.0f, 1.0f);
    if (s <= 0) {
        return {l, l, l, 1};
    }
    const float q = l < 0.5f ? l * (1 + s) : l + s - l * s;
    const float p = 2 * l - q;
    const auto channel = [&](float t) {
        if (t < 0) t += 1;
        if (t > 1) t -= 1;
        if (t < 1.0f / 6) return p + (q - p) * 6 * t;
        if (t < 0.5f) return q;
        if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
        return p;
    };
    return {channel(h + 1.0f / 3), channel(h), channel(h - 1.0f / 3), 1};
}

AccentContrast accentContrast(ThemePreset preset, gfx::Color accent) {
    const Theme theme = makeTheme(preset, accent);
    return {.onGround = contrastRatio(accent, theme.colors.ground),
            .inkOnAccent = contrastRatio(theme.colors.onAccent, accent)};
}

void showAccentPicker(AppContext &context) {
    AppState &state = context.state;
    const Theme &t = currentTheme();
    const Settings &settings = state.settings.peek();
    const gfx::Color start = settings.accent ? *settings.accent : makeTheme(settings.preset).colors.accent;
    // Shared by the sheet's binds and callbacks; lives as long as the sheet.
    struct Picker {
        Signal<Hsl> hsl;
        TextField *hex = nullptr;
        std::vector<Node *> sliders;
    };
    auto picker = std::make_shared<Picker>();
    picker->hsl.set(toHsl(start));
    AppState *s = &state;
    const auto apply = [s, picker](Hsl hsl, bool fromHex) {
        picker->hsl.set(hsl);
        const gfx::Color color = fromHsl(hsl);
        s->updateSettings([&](Settings &settings) { settings.accent = color; });
        if (!fromHex && picker->hex != nullptr && !picker->hex->editor().focused()) {
            picker->hex->editor().setText(colorToHex(color));
        }
    };

    auto content = Box::create({.gap = 10, .padding = {24, 0, 24, 8}});

    // The preview: the accent as a raised fill carrying its own ink, and the contrast readout.
    auto preview = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 14,
                                .padding = Edges::symmetric(20, 0), .height = 64});
    preview->setPaintHook([s](PaintContext &context) {
        const Theme &th = currentTheme();
        const gfx::Rect b = context.bounds();
        recipes::raised(context.list, {b, gfx::CornerRadii::uniform(th.cornerPx(b.height / 2, context.density))},
                        th.colors.accent, context.density);
        (void)s;
    });
    gfx::TextStyle big = t.type.display;
    big.size = 30;
    auto sample = Text::create("Aa", big, t.colors.onAccent);
    preview->appendChild(sample);
    auto previewHex = Text::create(colorToHex(start), withSize(t.type.label, 13), t.colors.onAccent);
    previewHex->updateStyle([](Style &st) {
        st.grow = 1;
        st.shrink = 1;
        st.basis = 0;
    });
    preview->appendChild(previewHex);
    auto check = Icon::create("check_circle", 24, t.colors.onAccent, true);
    preview->appendChild(check);
    preview->updateSemantics([](Semantics &sem) { sem.hidden = true; });
    content->appendChild(preview);

    auto checks = Box::create({.direction = Direction::row, .wrap = true, .gap = 8, .lineGap = 8, .margin = {0, 2, 0, 4}});
    auto checkSlot = checks.get();
    content->appendChild(checks);

    // Follow the accent: the preview's ink and hex, the contrast stamps, the slider tracks.
    content->bind([picker, s, sample = sample.get(), previewHex = previewHex.get(), checkSlot,
                   preview = preview.get(), check = check.get()] {
        const Hsl hsl = picker->hsl();
        const ThemePreset preset = s->settings().preset;
        (void)theme();
        untrack([&] {
            const gfx::Color color = fromHsl(hsl);
            const Theme th = makeTheme(preset, color);
            sample->setColor(th.colors.onAccent);
            previewHex->setColor(th.colors.onAccent);
            previewHex->setText(colorToHex(color));
            preview->markPaintDirty();
            check->setColor(th.colors.onAccent);
            const AccentContrast c = accentContrast(preset, color);
            checkSlot->clearChildren();
            checkSlot->appendChild(Stamp("On ground " + ratioText(c.onGround),
                                         c.groundPasses() ? StampTone::ink : StampTone::destructive,
                                         c.groundPasses() ? "check" : "warning"));
            checkSlot->appendChild(Stamp("Text on accent " + ratioText(c.inkOnAccent),
                                         c.inkPasses() ? StampTone::ink : StampTone::destructive,
                                         c.inkPasses() ? "check" : "warning"));
            if (!c.groundPasses()) {
                checkSlot->appendChild(OutlineTag("Too dim on the ground"));
            }
            for (Node *slider : picker->sliders) {
                slider->markPaintDirty();
            }
        });
    });

    // The three sliders, each over a track that shows what it does.
    const auto sliderRow = [&](const char *label, const char *spoken, std::function<float(const Hsl &)> get,
                               std::function<Hsl(Hsl, float)> set, std::function<std::string(float)> format,
                               std::function<std::vector<gfx::Color>(const Hsl &)> stops) {
        auto row = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 6});
        auto name = Text::create(label, t.type.label, textInk(t, t.text.label, 11));
        name->updateStyle([](Style &st) { st.width = 44; });
        name->updateSemantics([](Semantics &sem) { sem.hidden = true; });
        row->appendChild(name);
        auto slider = Slider::create({.value = [picker, get] { return get(picker->hsl()); },
                                      .onChange = [picker, apply, set](float v) { apply(set(picker->hsl.peek(), v), false); },
                                      .label = spoken,
                                      .step = 0.02f,
                                      .format = format,
                                      .paintTrack = [picker, stops](gfx::DisplayList &list, const gfx::RoundedRect &track,
                                                                    float) {
                                          paintStops(list, track, stops(picker->hsl.peek()));
                                      }});
        slider->updateStyle([](Style &st) {
            st.grow = 1;
            st.shrink = 1;
            st.basis = 0;
        });
        picker->sliders.push_back(slider.get());
        row->appendChild(slider);
        auto value = Text::create(format(get(picker->hsl.peek())), t.type.label, textInk(t, t.text.body, 11));
        value->updateStyle([](Style &st) { st.width = 44; });
        value->setAlign(gfx::TextAlign::end);
        value->updateSemantics([](Semantics &sem) { sem.hidden = true; });
        value->bind([raw = value.get(), picker, get, format] {
            const std::string text = format(get(picker->hsl()));
            untrack([&] { raw->setText(text); });
        });
        row->appendChild(value);
        return row;
    };
    content->appendChild(sliderRow(
            "Hue", "Hue", [](const Hsl &h) { return h.h / 360; },
            [](Hsl h, float v) {
                h.h = v * 360;
                return h;
            },
            [](float v) { return degrees(v * 360); },
            [](const Hsl &h) {
                std::vector<gfx::Color> colors;
                for (int i = 0; i <= 6; ++i) {
                    colors.push_back(fromHsl({static_cast<float>(i) * 60, std::max(h.s, 0.35f), std::clamp(h.l, 0.35f, 0.65f)}));
                }
                return colors;
            }));
    content->appendChild(sliderRow(
            "Sat", "Saturation", [](const Hsl &h) { return h.s; },
            [](Hsl h, float v) {
                h.s = v;
                return h;
            },
            percentText, [](const Hsl &h) { return std::vector<gfx::Color>{fromHsl({h.h, 0, h.l}), fromHsl({h.h, 1, h.l})}; }));
    content->appendChild(sliderRow(
            "Light", "Lightness", [](const Hsl &h) { return h.l; },
            [](Hsl h, float v) {
                h.l = v;
                return h;
            },
            percentText, [](const Hsl &h) {
                return std::vector<gfx::Color>{fromHsl({h.h, h.s, 0.05f}), fromHsl({h.h, h.s, 0.5f}),
                                               fromHsl({h.h, h.s, 0.95f})};
            }));

    auto hex = TextField::create({.label = "Hex",
                                  .placeholder = "#RRGGBB",
                                  .text = colorToHex(start),
                                  .icon = "palette",
                                  .helper = std::string("Six hex digits"),
                                  .config = {.keyboard = KeyboardType::visiblePassword,
                                             .action = ImeAction::done},
                                  .onChange = [picker, apply](const std::string &text) {
                                      std::string value = text;
                                      if (!value.empty() && value.front() != '#') {
                                          value.insert(value.begin(), '#');
                                      }
                                      for (char &c : value) {
                                          c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                                      }
                                      if (auto color = colorFromHex(value)) {
                                          apply(toHsl(*color), true);
                                      }
                                  }});
    picker->hex = hex.get();
    content->appendChild(hex);

    auto row = Box::create({.direction = Direction::row, .gap = 10, .margin = {0, 4, 0, 0}});
    auto reset = Button::create({.label = "Reset", .variant = ButtonVariant::glass, .icon = "restart_alt",
                                 .onTap = [s, picker] {
                                     s->updateSettings([](Settings &settings) { settings.accent.reset(); });
                                     const gfx::Color preset = makeTheme(s->settings.peek().preset).colors.accent;
                                     picker->hsl.set(toHsl(preset));
                                     if (picker->hex != nullptr) {
                                         picker->hex->editor().setText(colorToHex(preset));
                                     }
                                 },
                                 .accessibilityLabel = "Reset to the preset's accent"});
    Overlays *overlays = &context.overlays;
    auto done = Button::create({.label = "Done", .variant = ButtonVariant::primary, .onTap = [overlays] {
                                    if (auto *sheet = overlays->topSheet()) {
                                        sheet->dismiss();
                                    }
                                }});
    for (Node *b : {static_cast<Node *>(reset.get()), static_cast<Node *>(done.get())}) {
        b->updateStyle([](Style &st) {
            st.grow = 1;
            st.shrink = 1;
            st.basis = 0;
        });
    }
    row->appendChild(reset);
    row->appendChild(done);
    content->appendChild(row);
    context.overlays.showSheet("Accent", parts::scrolling(content), true, [picker] {
        if (picker->hex != nullptr) {
            picker->hex->editor().clearFocus();
        }
        picker->hex = nullptr;
        picker->sliders.clear();
    });
}

ScreenSpec settingsScreenSpec() {
    return {.id = kSettingsScreen, .title = "Settings", .icon = "settings", .build = buildSettings};
}

}  // namespace filepan::app
