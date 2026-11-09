// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/user_items.hpp"

#include <algorithm>

#include "filepan/app/format.hpp"
#include "filepan/app/settings.hpp"

namespace filepan::app {

namespace {

// The settings file's encoding of a possibly empty string, as settings.cpp writes it: "-" for
// empty, a literal "-" escaped.
std::string field(std::string_view value) {
    if (value.empty()) {
        return "-";
    }
    if (value == "-") {
        return "%2D";
    }
    return percentEncode(value);
}

std::optional<std::string> unfield(std::string_view token) {
    if (token == "-") {
        return std::string();
    }
    return percentDecode(token);
}

std::optional<vfs::Location> readLocation(std::string_view token) {
    auto decoded = percentDecode(token);
    return decoded ? decodeLocation(*decoded) : std::nullopt;
}

bool affected(const vfs::Location &location, const vfs::Location &by) { return by.isAncestorOf(location); }

}  // namespace

std::optional<std::size_t> findFavorite(const std::vector<Favorite> &favorites, const vfs::Location &location) {
    for (std::size_t i = 0; i < favorites.size(); ++i) {
        if (favorites[i].location == location) {
            return i;
        }
    }
    return std::nullopt;
}

std::vector<std::string> hiddenNamesIn(const std::vector<vfs::Location> &hiddenItems, const vfs::Location &folder) {
    std::vector<std::string> names;
    for (const vfs::Location &item : hiddenItems) {
        if (const auto parent = item.parent(); parent && *parent == folder) {
            names.emplace_back(item.name());
        }
    }
    std::ranges::sort(names);
    return names;
}

vfs::Location rebaseLocation(const vfs::Location &location, const vfs::Location &from, const vfs::Location &to) {
    const std::string rest = location.path().substr(from.isRoot() ? 0 : from.path().size());
    const std::string path = to.isRoot() ? (rest.empty() ? std::string("/") : rest) : to.path() + rest;
    return to.withPath(path);
}

const Favorite *homeFavorite(const Settings &settings) {
    if (!settings.homeTarget) {
        return nullptr;
    }
    const auto index = findFavorite(settings.favorites, *settings.homeTarget);
    return index ? &settings.favorites[*index] : nullptr;
}

bool relocateUserItems(Settings &settings, const vfs::Location &location, const vfs::Location &target) {
    if (location == target) {
        return false;
    }
    bool changed = false;
    for (Favorite &favorite : settings.favorites) {
        if (!affected(favorite.location, location)) {
            continue;
        }
        if (favorite.location == location && favorite.name == location.name()) {
            favorite.name = std::string(target.name());
        }
        favorite.location = rebaseLocation(favorite.location, location, target);
        changed = true;
    }
    // Two favorites that now name the same place: the first stays.
    std::vector<Favorite> unique;
    for (Favorite &favorite : settings.favorites) {
        if (!findFavorite(unique, favorite.location)) {
            unique.push_back(std::move(favorite));
        }
    }
    settings.favorites = std::move(unique);
    // Hidden items the same way, kept unique.
    std::vector<vfs::Location> hidden;
    for (const vfs::Location &item : settings.hiddenItems) {
        const vfs::Location moved = affected(item, location) ? rebaseLocation(item, location, target) : item;
        changed = changed || moved != item;
        if (std::ranges::find(hidden, moved) == hidden.end()) {
            hidden.push_back(moved);
        }
    }
    settings.hiddenItems = std::move(hidden);
    if (settings.homeTarget && affected(*settings.homeTarget, location)) {
        settings.homeTarget = rebaseLocation(*settings.homeTarget, location, target);
        changed = true;
    }
    return changed;
}

bool dropUserItems(Settings &settings, const std::vector<vfs::Location> &gone) {
    const auto isGone = [&](const vfs::Location &location) {
        return std::ranges::any_of(gone, [&](const vfs::Location &g) { return affected(location, g); });
    };
    const std::size_t before = settings.favorites.size() + settings.hiddenItems.size();
    std::erase_if(settings.favorites, [&](const Favorite &f) { return isGone(f.location); });
    std::erase_if(settings.hiddenItems, isGone);
    if (settings.homeTarget && isGone(*settings.homeTarget)) {
        settings.homeTarget.reset();
    }
    return settings.favorites.size() + settings.hiddenItems.size() != before;
}

void writeUserItems(std::string &out, const Settings &settings) {
    for (const Favorite &favorite : settings.favorites) {
        out += "favorite " + field(favorite.name) + ' ' + percentEncode(encodeLocation(favorite.location)) +
               (favorite.kind == Favorite::Kind::file ? " file" : " folder") + '\n';
    }
    for (const vfs::Location &item : settings.hiddenItems) {
        out += "hidden-item " + percentEncode(encodeLocation(item)) + '\n';
    }
    if (settings.homeTarget) {
        out += "home-target " + percentEncode(encodeLocation(*settings.homeTarget)) + '\n';
    }
}

bool readUserItem(const std::vector<std::string_view> &f, Settings &settings) {
    const std::string_view key = f[0];
    if ((key == "favorite" && f.size() == 4) || (key == "bookmark" && f.size() == 3)) {
        auto name = unfield(f[1]);
        auto location = readLocation(f[2]);
        const bool file = key == "favorite" && f[3] == "file";
        if (name && location && !findFavorite(settings.favorites, *location) &&
            (key == "bookmark" || f[3] == "file" || f[3] == "folder")) {
            settings.favorites.push_back({*name, *location, file ? Favorite::Kind::file : Favorite::Kind::folder});
        }
        return true;
    }
    if (key == "hidden-item") {
        auto location = f.size() == 2 ? readLocation(f[1]) : std::nullopt;
        if (location && std::ranges::find(settings.hiddenItems, *location) == settings.hiddenItems.end()) {
            settings.hiddenItems.push_back(*location);
        }
        return true;
    }
    if (key == "home-target") {
        settings.homeTarget = f.size() == 2 ? readLocation(f[1]) : std::nullopt;
        return true;
    }
    return key == "favorite" || key == "bookmark";
}

}  // namespace filepan::app
