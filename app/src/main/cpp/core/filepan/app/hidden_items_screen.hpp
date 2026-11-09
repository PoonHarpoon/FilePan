// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include "filepan/app/app.hpp"

// The hidden items screen, opened from settings next to "Show hidden files": every file and
// folder the user hid (user_items.hpp), local or on a server, with its path and an Unhide
// button, and Unhide all (after a confirmation).
namespace filepan::app {

inline constexpr const char *kHiddenItemsScreen = "hidden-items";

ScreenSpec hiddenItemsScreenSpec();

// The settings line that opens it: "Hidden items", with how many there are.
ui::NodePtr hiddenItemsLink(AppContext &context);

}  // namespace filepan::app
