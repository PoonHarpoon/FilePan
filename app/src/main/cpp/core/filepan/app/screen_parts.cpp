// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/screen_parts.hpp"

#include <algorithm>
#include <cstdio>

namespace filepan::app {

using namespace ui;
using namespace ui::components;

ScrollPage scrollPage(AppContext &context, float gap) {
    ScrollPage page;
    page.screen = Box::create({.position = Position::absolute, .left = 0, .top = 0, .right = 0, .bottom = 0});
    page.scroll = ScrollNode::create();
    page.scroll->setStyle({.position = Position::absolute, .left = 0, .top = 0, .right = 0, .bottom = 0});
    page.column = Box::create({.gap = gap});
    page.scroll->appendChild(page.column);
    page.screen->appendChild(page.scroll);
    Node *column = page.column.get();
    ScrollNode *scroll = page.scroll.get();
    AppContext *ctx = &context;
    page.screen->build([&] {
        page.screen->bind([column, ctx] {
            const float density = environment().density();
            const float bottom = std::max(environment().safeArea().bottom, environment().imeInsets().bottom) / density;
            const float top = ctx->barHeight();
            untrack([&] { column->updateStyle([&](Style &s) { s.padding = {16, top + 4, 16, bottom + 32}; }); });
        });
        page.screen->bind([scroll, ctx] {
            const float offset = scroll->offsetSignal()();
            untrack([&] { ctx->barScroll.set(offset); });
        });
    });
    return page;
}

NodePtr pageSection(const std::string &label) {
    auto s = SectionLabel(label);
    s->updateStyle([](Style &st) { st.padding = {4, 14, 0, 2}; });
    return s;
}

NodePtr settingLine(const std::string &title, const std::string &detail, NodePtr control) {
    const Theme &t = currentTheme();
    auto line = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 12, .minHeight = 60});
    auto texts = Box::create({.gap = 3, .padding = Edges::symmetric(0, 8), .grow = 1, .shrink = 1, .basis = 0});
    auto heading = Text::create(title, t.type.body, t.colors.ink);
    heading->updateSemantics([](Semantics &s) { s.hidden = true; });
    texts->appendChild(heading);
    if (!detail.empty()) {
        auto sub = Text::create(detail, t.type.label, textInk(t, t.text.meta, 11));
        sub->updateSemantics([](Semantics &s) { s.hidden = true; });
        texts->appendChild(sub);
    }
    line->appendChild(texts);
    if (control) {
        line->appendChild(std::move(control));
    }
    return line;
}

gfx::TextStyle monoText(const Theme &theme, float sizeSp, float weight) {
    gfx::TextStyle style = theme.type.label;
    style.uppercase = false;
    style.letterSpacing = 0.02f;
    style.size = sizeSp;
    style.weight = weight;
    return style;
}

NodePtr divider() {
    auto line = Box::create({.height = 1});
    line->setPaintHook([](PaintContext &context) {
        const Theme &t = currentTheme();
        const gfx::Rect b = context.bounds();
        context.list.fill(gfx::Rect{b.x, b.y, b.width, std::max(1.0f, context.density * 0.5f)},
                          t.colors.ink.withAlpha(t.colors.divider));
    });
    line->updateSemantics([](Semantics &s) { s.hidden = true; });
    return line;
}

std::string shortCode(std::string_view seed) {
    // FNV-1a, folded to 16 bits.
    std::uint32_t hash = 2166136261u;
    for (const char c : seed) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;
    }
    char buffer[8];
    std::snprintf(buffer, sizeof buffer, "0x%04X", static_cast<unsigned>((hash >> 16) ^ (hash & 0xFFFF)));
    return buffer;
}

}  // namespace filepan::app
