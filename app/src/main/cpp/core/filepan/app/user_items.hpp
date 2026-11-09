// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "filepan/vfs/location.hpp"

// Files and folders the user has marked: favorites, and hidden items (shown only with "Show
// hidden files", like dot files). They live in the settings and are kept in step with FilePan's
// own renames, moves and deletes (relocateUserItems, dropUserItems); changes made outside the app
// are left alone.
//
// In the settings file each is one line, after the tabs:
//
//   favorite <name> <location> <folder|file>
//   hidden-item <location>
//   home-target <location>
//
// Older files kept folders only, as "bookmark <name> <location>"; those still read, as folder
// favorites, and are written back as favorite lines.
namespace filepan::app {

struct Settings;

struct Favorite {
    enum class Kind : std::uint8_t { folder, file };

    std::string name;
    vfs::Location location = vfs::Location::local("/");
    Kind kind = Kind::folder;

    bool operator==(const Favorite &) const = default;
};

// The favorite at location, if there's one.
[[nodiscard]] std::optional<std::size_t> findFavorite(const std::vector<Favorite> &favorites,
                                                      const vfs::Location &location);

// The favorite the home button goes to, or null for the storage home: it's unset, or no longer a
// favorite (it's left set then, so undoing the removal restores it).
[[nodiscard]] const Favorite *homeFavorite(const Settings &settings);

// The names of the hidden items directly in folder, sorted.
[[nodiscard]] std::vector<std::string> hiddenNamesIn(const std::vector<vfs::Location> &hiddenItems,
                                                     const vfs::Location &folder);

// location moved to (or was renamed) target: whatever was at it or below it follows. A favorite
// that still had the old name takes the new one; one the user renamed keeps theirs. True if
// anything changed.
bool relocateUserItems(Settings &settings, const vfs::Location &location, const vfs::Location &target);
// These are gone: drops whatever was at them or below them. True if anything changed.
bool dropUserItems(Settings &settings, const std::vector<vfs::Location> &gone);

// location with its from prefix replaced by to (from must be an ancestor of it, or it).
[[nodiscard]] vfs::Location rebaseLocation(const vfs::Location &location, const vfs::Location &from,
                                           const vfs::Location &to);

// The settings file's lines for them, and reading one back (fields split on spaces, the key
// first); false if the line isn't one of theirs.
void writeUserItems(std::string &out, const Settings &settings);
bool readUserItem(const std::vector<std::string_view> &fields, Settings &settings);

}  // namespace filepan::app
