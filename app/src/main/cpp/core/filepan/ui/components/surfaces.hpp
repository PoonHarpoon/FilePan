// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <memory>
#include <string>

#include "filepan/gfx/geometry.hpp"
#include "filepan/gfx/text.hpp"
#include "filepan/ui/node.hpp"
#include "filepan/ui/style.hpp"
#include "filepan/ui/theme.hpp"

// Background, panels and icons: what everything else sits on.
namespace filepan::ui::components {

struct BackgroundProps {
    // The 48 dp grid at the theme's gridAlpha: the storage home only.
    bool grid = false;
};

// The screen's background: the ground, the optional grid, then BackgroundEndCommand so BottomFade
// can reuse it. Fills its parent; make it the screen's first child.
class Background final : public Node {
public:
    static std::shared_ptr<Background> create(BackgroundProps props = {});

protected:
    void paint(PaintContext &context) override;

private:
    BackgroundProps props_;
};

// A fade along the bottom of the screen, behind the dock and data band, so list rows passing
// under them never compete with them: the bottom `solid` dp (plus the navigation bar) show only
// the screen's background, and the rest fades from it to transparent. It draws the cached
// background (BlurSource::background), so put a Background first. Hit-transparent; heights in
// dp above the navigation bar.
NodePtr BottomFade(float height = 140, float solid = 96);

struct GlassProps {
    // dp; anything over kCorner draws at kCorner (Theme::corner).
    gfx::CornerRadii radii = gfx::CornerRadii::uniform(kCorner);
    // Corner ticks over the content: the primary volume card, sheet headers.
    bool cornerTicks = false;
    // Clips the children to the shape.
    bool clip = false;
};

// A panel or bar (recipes::glass): opaque, with a hard shadow. Children lay out inside it like a Box.
class Glass final : public Node {
public:
    static std::shared_ptr<Glass> create(Style style, GlassProps props = {});

    void setProps(GlassProps props);
    [[nodiscard]] const GlassProps &props() const { return props_; }

protected:
    void paint(PaintContext &context) override;
    void paintOverlay(PaintContext &context) override;

private:
    void updateClip(const Theme &theme);

    GlassProps props_;
};

// A panel with uniform corners.
std::shared_ptr<Glass> GlassPanel(Style style, float radius = kCorner);

// A Material Symbols icon, drawn from its ligature name ("folder", "content_copy"). Decorative:
// it adds nothing to semantics; the control around it carries the label.
class Icon final : public Text {
public:
    static std::shared_ptr<Icon> create(std::string name, float sizeDp = 24, gfx::Color color = {1, 1, 1, 1},
                                        bool filled = false);

    void setIcon(std::string name) { setText(std::move(name)); }
    void setFilled(bool filled);
    void setSize(float sizeDp);
    [[nodiscard]] std::string_view semanticsText() const override { return {}; }
};

}  // namespace filepan::ui::components
