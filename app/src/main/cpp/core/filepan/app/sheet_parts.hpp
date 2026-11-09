// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "filepan/ui/components/components.hpp"
#include "filepan/ui/node.hpp"
#include "filepan/vfs/entry.hpp"

// The pieces the file operation sheets and dialogs share, so they line up with each other: a
// padded content column, section labels, label/value lines, file rows, a progress track and a
// row of buttons.
namespace filepan::app::parts {

// A sheet's content: 24 dp sides, the given gap.
std::shared_ptr<ui::Box> sheetColumn(float gap = 8);
// Wraps a column that can outgrow the screen in a vertical scroller.
ui::NodePtr scrolling(ui::NodePtr column);
// A quiet mono caps label (`FOLDERS`), spaced for a sheet.
ui::NodePtr section(std::string label);
// Wrapping body text at the body opacity.
std::shared_ptr<ui::Text> bodyText(std::string text);
// A mono line at the meta opacity; wraps unless maxLines is set. Names and paths keep their case
// (caps false); labels and figures go in caps.
std::shared_ptr<ui::Text> monoText(std::string text, int maxLines = 0, bool caps = true);
// The mono style without caps, for names, paths and hashes.
gfx::TextStyle plainMono();

// A mono caps label over a value in body type. With onTap, the line is a button (copy) with a
// trailing icon.
struct InfoLine {
    std::string label;
    ui::Prop<std::string> value;
    std::function<void()> onTap;
    std::string icon = "content_copy";
    // Mono for hashes, paths and modes.
    bool mono = false;
};
ui::NodePtr infoLine(InfoLine line);

// What a FileRow shows for an entry, as the browsing list shows it.
ui::components::FileItem fileItemFor(const vfs::Entry &entry, std::optional<std::uint64_t> itemCount = std::nullopt);
// A file as a row that doesn't take input, inset to the sheet's edges.
ui::NodePtr fileRow(const vfs::Entry &entry, std::string meta = {}, std::string stamp = {});

// A slim sunken track with the theme's spectrum filling it to value (0-1). Negative shows
// hazard stripes: still counting.
ui::NodePtr progressTrack(ui::Prop<float> value);

// Buttons side by side, sharing the width.
ui::NodePtr buttonRow(std::vector<ui::NodePtr> buttons);

}  // namespace filepan::app::parts
