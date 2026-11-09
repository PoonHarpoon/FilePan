// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/hidden_items_screen.hpp"

#include <memory>
#include <string>
#include <vector>

#include "filepan/app/favorites_sheet.hpp"
#include "filepan/app/format.hpp"
#include "filepan/app/screen_parts.hpp"

namespace filepan::app {

namespace {

using namespace ui;
using namespace ui::components;

void confirmUnhideAll(AppContext &context) {
    const std::size_t count = context.state.settings.peek().hiddenItems.size();
    if (count == 0) {
        return;
    }
    auto holder = std::make_shared<Dialog *>(nullptr);
    AppState *state = &context.state;
    std::vector<NodePtr> actions;
    actions.push_back(Button::create({.label = "Cancel", .variant = ButtonVariant::text, .onTap = [holder] {
                                          if (*holder != nullptr) {
                                              (*holder)->dismiss();
                                          }
                                      }}));
    actions.push_back(Button::create({.label = "Unhide all",
                                      .variant = ButtonVariant::primary,
                                      .onTap = [holder, state] {
                                          state->updateSettings([](Settings &s) { s.hiddenItems.clear(); });
                                          if (*holder != nullptr) {
                                              (*holder)->dismiss();
                                          }
                                      },
                                      .accessibilityLabel = "Unhide all " + formatCount(count)}));
    *holder = &context.overlays.showDialog(
            {.title = "Unhide all?",
             .message = formatCount(count) + " will show again with hidden files off.",
             .actions = std::move(actions)});
}

NodePtr buildHiddenItems(AppContext &context) {
    ScrollPage page = scrollPage(context);
    page.screen->setDebugName("HiddenItemsScreen");
    AppContext *ctx = &context;
    Node *column = page.column.get();
    page.screen->build([&] {
        page.screen->bind([ctx, column] {
            const std::vector<vfs::Location> items = ctx->state.settings().hiddenItems;
            (void)ctx->volumes();
            const Theme &t = theme();
            untrack([&] {
                column->clearChildren();
                column->appendChild(pageSection("Hidden by you"));
                if (items.empty()) {
                    auto text = Text::create("Nothing hidden. Hide files and folders from their ⋮ menu; they "
                                             "show again, dimmed, with Show hidden files.",
                                             t.type.bodySmall, ink(t, t.text.label));
                    text->updateStyle([](Style &s) { s.margin = {4, 2, 4, 4}; });
                    column->appendChild(text);
                    return;
                }
                auto panel = GlassPanel({.padding = Edges::symmetric(0, 6)});
                panel->setDebugName("HiddenItems");
                for (std::size_t i = 0; i < items.size(); ++i) {
                    if (i > 0) {
                        panel->appendChild(divider());
                    }
                    const vfs::Location item = items[i];
                    const std::string name = item.isRoot() ? locationPath(*ctx, item) : std::string(item.name());
                    const std::string path = locationPath(*ctx, item);
                    auto row = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 12,
                                            .padding = {18, 6, 8, 6}, .minHeight = 60});
                    row->setDebugName("HiddenItemRow");
                    row->appendChild(Icon::create(item.kind() == vfs::BackendKind::kSftp ? "lan" : "visibility_off",
                                                  22, ink(t, 0.6f)));
                    auto texts = Box::create({.gap = 3, .grow = 1, .shrink = 1, .basis = 0});
                    auto title = Text::create(name, t.type.body, t.colors.ink);
                    title->setMaxLines(1);
                    title->setOverflow(gfx::TextOverflow::ellipsisMiddle);
                    auto line = Text::create(path, monoText(t, 11, 500), ink(t, t.text.meta));
                    line->setMaxLines(1);
                    line->setOverflow(gfx::TextOverflow::ellipsisStart);
                    texts->appendChild(title);
                    texts->appendChild(line);
                    row->appendChild(texts);
                    row->appendChild(IconButton::create({.icon = "visibility",
                                                         .label = "Unhide " + name,
                                                         .style = IconButtonStyle::plain,
                                                         .size = 44,
                                                         .onTap = [ctx, item] {
                                                             ctx->state.setHidden({item}, false);
                                                         }}));
                    row->updateSemantics([&](Semantics &s) {
                        s.role = Role::listItem;
                        s.label = name + ", " + path;
                    });
                    for (Node *n : {static_cast<Node *>(title.get()), static_cast<Node *>(line.get())}) {
                        n->updateSemantics([](Semantics &s) { s.hidden = true; });
                    }
                    panel->appendChild(row);
                }
                column->appendChild(panel);
                auto all = Button::create({.label = "Unhide all",
                                           .variant = ButtonVariant::glass,
                                           .icon = "visibility",
                                           .onTap = [ctx] { confirmUnhideAll(*ctx); }});
                all->updateStyle([](Style &s) { s.margin = {0, 8, 0, 0}; });
                column->appendChild(all);
            });
        });
    });
    return page.screen;
}

}  // namespace

ScreenSpec hiddenItemsScreenSpec() {
    return {.id = kHiddenItemsScreen, .title = "Hidden items", .icon = "visibility_off", .build = buildHiddenItems,
            .inMenu = false};
}

NodePtr hiddenItemsLink(AppContext &context) {
    const Theme &t = currentTheme();
    const std::size_t count = context.state.settings.peek().hiddenItems.size();
    const std::string detail = count == 0 ? std::string("None yet") : formatCount(count);
    auto row = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 14,
                            .padding = Edges::symmetric(0, 8), .minHeight = 56});
    row->setDebugName("HiddenItemsLink");
    auto texts = Box::create({.gap = 3, .grow = 1, .shrink = 1, .basis = 0});
    texts->appendChild(Text::create("Hidden items", t.type.body, t.colors.ink));
    texts->appendChild(Text::create(detail, t.type.label, textInk(t, t.text.meta, 11)));
    for (const auto &child : texts->children()) {
        child->updateSemantics([](Semantics &s) { s.hidden = true; });
    }
    row->appendChild(texts);
    row->appendChild(Icon::create("chevron_right", 20, ink(t, 0.6f)));
    AppState *state = &context.state;
    makePressable(*row, {.onTap = [state] { state->openScreen(kHiddenItemsScreen); }});
    row->updateSemantics([&](Semantics &s) {
        s.role = Role::button;
        s.label = "Hidden items, " + detail;
    });
    return row;
}

}  // namespace filepan::app
