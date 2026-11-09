// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <functional>

#include "filepan/ui/node.hpp"

// Screens built from the component kit for the golden tests: a gallery of every component over
// a few pages, and a mock of the browsing screen as glass-signal-mobile.md lays it out. Each is
// built under the current environment's theme.
namespace filepan::test {

struct Page {
    ui::NodePtr root;
    // Runs once the root is attached and laid out: focus a field, open a sheet.
    std::function<void()> attached;
};

Page galleryControls();
Page galleryMarks();
Page galleryOverlays();
Page galleryMenus();
Page galleryDialog();
Page browsingMock();

}  // namespace filepan::test
