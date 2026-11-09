// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <memory>
#include <string>

#include "filepan/app/app.hpp"
#include "filepan/ui/scroll.hpp"

// Pieces the registered screens share: a scrolling page under the top bar, section labels, and
// setting lines.
namespace filepan::app {

struct ScrollPage {
    std::shared_ptr<ui::Node> screen;    // Fills the screen slot.
    std::shared_ptr<ui::ScrollNode> scroll;
    std::shared_ptr<ui::Node> column;    // Add content here.
};

// A page that scrolls under the top bar, padded for it and the navigation bar, and drives the
// bar's second row like the home and browsing screens do.
ScrollPage scrollPage(AppContext &context, float gap = 10);

// A quiet mono caps label, spaced for a page.
ui::NodePtr pageSection(const std::string &label);

// A setting row: a title and a mono detail line on the left, the control on the right.
ui::NodePtr settingLine(const std::string &title, const std::string &detail, ui::NodePtr control);

// JetBrains Mono without the label's caps: fingerprints, hosts, paths, license text.
gfx::TextStyle monoText(const ui::Theme &theme, float sizeSp, float weight = 450);

// A hairline between rows in a panel.
ui::NodePtr divider();

// "a1b2" -> a short stable hex code: `0x7F3A`, for ID tags.
std::string shortCode(std::string_view seed);

}  // namespace filepan::app
