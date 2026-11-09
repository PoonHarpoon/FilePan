// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "filepan/app/app.hpp"
#include "filepan/app/file_actions.hpp"
#include "filepan/app/user_items.hpp"

// Favorites on screen (glass-signal-mobile.md, "Favorites"): the sheet the dock's star opens, and
// what the menus and the storage home do with them.
//
// The sheet lists every favorite with a kind icon (folder, file, or a server's glyph for one on
// SFTP), its name, and its path cut at the start so the end stays in view. Tapping a folder opens
// it in the current tab; tapping a file opens its folder with the file scrolled to and briefly
// tinted. Below the list, the current folder can be added or removed. The pencil in the header
// switches to editing: a drag handle to reorder, rename (a sheet with a name field) and remove
// (with an undo snackbar). Each row also offers rename, remove, move up and move down as
// accessibility actions.
//
// A local favorite whose target is gone (moved or deleted outside FilePan) shows as unavailable;
// AppContext::missingFavorites says which (App checks on start, on resume and when they change).
namespace filepan::app {

inline constexpr float kFavoriteRowHeight = 56;

void showFavorites(AppContext &context);

// What a favorite is, for its row: "folder", "file", "SFTP folder"...
std::string favoriteKind(const Favorite &favorite);
const char *favoriteIcon(const Favorite &favorite);
// Its path from the root, with the server's own name for SFTP: "Internal / Download / Papers",
// "SFTP · Home NAS / srv / media".
std::string favoritePath(const AppContext &context, const Favorite &favorite);
// The same for any location.
std::string locationPath(const AppContext &context, const vfs::Location &location);
[[nodiscard]] bool favoriteMissing(const AppContext &context, const Favorite &favorite);

// Opens a favorite: a folder in the current tab, a file in its folder, revealed. One that's gone
// says so instead, with a way to remove it.
void openFavorite(AppContext &context, const Favorite &favorite);
// The top bar's home button: the favorite chosen in the settings, or the storage home.
void openHome(AppContext &context);
// Removes the favorite at index, with an undo snackbar.
void removeFavoriteAt(AppContext &context, std::size_t index);
// The rename sheet for the favorite at index.
void showRenameFavorite(AppContext &context, std::size_t index);
// The current folder: adds or removes it, and says so.
void toggleFavoriteFolder(AppContext &context, const vfs::Location &folder);
// The selection (or a row's entry): adds each that isn't one yet, or with on false removes
// them all; says so.
void setFavorites(AppContext &context, const FileTargets &targets, bool on);
// Whether every target is a favorite already. Untracked.
[[nodiscard]] bool allFavorites(const AppContext &context, const FileTargets &targets);

}  // namespace filepan::app
