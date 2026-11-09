// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/app_state.hpp"

#include <algorithm>

#include "filepan/app/format.hpp"

namespace filepan::app {

void TabModel::setSearching(bool on) {
    ui::batch([&] {
        searching.set(on);
        if (!on) {
            filter.set({});
        }
    });
}

TabModel::ScrollPosition TabModel::scrollFor(const vfs::Location &location) const {
    const auto found = scroll_.find(encodeLocation(location));
    return found == scroll_.end() ? ScrollPosition{} : found->second;
}

void TabModel::setScroll(const vfs::Location &location, ScrollPosition position) {
    scroll_[encodeLocation(location)] = std::move(position);
}

AppState::AppState(Settings initial, std::string homePath)
    : settings(std::move(initial)), homePath_(homePath.empty() ? std::string("/") : std::move(homePath)) {
    std::vector<std::shared_ptr<TabModel>> list;
    const Settings &s = settings.peek();
    for (const SavedTab &saved : s.tabs) {
        auto tab = std::make_shared<TabModel>(nextTabId_++, s.view);
        std::vector<Route> stack{Route::home()};
        for (const vfs::Location &location : saved.history) {
            stack.push_back(Route::browse(location));
        }
        tab->stack.set(std::move(stack));
        list.push_back(std::move(tab));
    }
    std::size_t active = s.activeTab;
    if (list.empty()) {
        list.push_back(makeTab(vfs::Location::local(homePath_)));
        active = 0;
    }
    activeId.set(list[std::min(active, list.size() - 1)]->id);
    tabs.set(std::move(list));
}

std::shared_ptr<TabModel> AppState::makeTab(std::optional<vfs::Location> location) {
    auto tab = std::make_shared<TabModel>(nextTabId_++, settings.peek().view);
    if (location) {
        tab->stack.set({Route::home(), Route::browse(*location)});
    }
    return tab;
}

std::shared_ptr<TabModel> AppState::tab(std::uint64_t id) const {
    for (const auto &tab : tabs.peek()) {
        if (tab->id == id) {
            return tab;
        }
    }
    return nullptr;
}

std::shared_ptr<TabModel> AppState::active() const {
    const std::uint64_t id = activeId();
    for (const auto &tab : tabs()) {
        if (tab->id == id) {
            return tab;
        }
    }
    return tabs().front();
}

std::shared_ptr<TabModel> AppState::peekActive() const {
    return ui::untrack([&] { return active(); });
}

void AppState::resetTabState(TabModel &tab) {
    tab.selection.set({});
    tab.searching.set(false);
    tab.filter.set({});
}

void AppState::navigate(const vfs::Location &location) {
    auto tab = peekActive();
    ui::batch([&] {
        resetTabState(*tab);
        tab->stack.update([&](std::vector<Route> &stack) {
            if (stack.back().location != location || stack.back().kind != Route::Kind::browse) {
                stack.push_back(Route::browse(location));
            }
        });
    });
}

void AppState::navigateHome() {
    auto tab = peekActive();
    ui::batch([&] {
        resetTabState(*tab);
        tab->stack.set({Route::home()});
    });
}

void AppState::openScreen(std::string id) {
    auto tab = peekActive();
    ui::batch([&] {
        resetTabState(*tab);
        tab->stack.update([&](std::vector<Route> &stack) { stack.push_back(Route::screen(std::move(id))); });
    });
}

bool AppState::back() {
    auto tab = peekActive();
    if (!tab->selection.peek().empty()) {
        tab->selection.set({});
        return true;
    }
    if (tab->searching.peek() || !tab->filter.peek().empty()) {
        ui::batch([&] {
            tab->searching.set(false);
            tab->filter.set({});
        });
        return true;
    }
    return pop();
}

bool AppState::pop() {
    auto tab = peekActive();
    if (tab->stack.peek().size() <= 1) {
        return false;
    }
    ui::batch([&] {
        resetTabState(*tab);
        tab->stack.update([](std::vector<Route> &stack) { stack.pop_back(); });
    });
    return true;
}

std::uint64_t AppState::addTab(std::optional<vfs::Location> location) {
    auto tab = makeTab(std::move(location));
    const std::uint64_t id = tab->id;
    const std::uint64_t current = activeId.peek();
    ui::batch([&] {
        tabs.update([&](std::vector<std::shared_ptr<TabModel>> &list) {
            auto at = std::ranges::find_if(list, [&](const auto &t) { return t->id == current; });
            list.insert(at == list.end() ? list.end() : at + 1, tab);
        });
        activeId.set(id);
    });
    return id;
}

void AppState::closeTab(std::uint64_t id) {
    const auto &list = tabs.peek();
    if (list.size() <= 1) {
        if (!list.empty() && list.front()->id == id) {
            navigateHome();
        }
        return;
    }
    const auto at = std::ranges::find_if(list, [&](const auto &t) { return t->id == id; });
    if (at == list.end()) {
        return;
    }
    const std::size_t index = static_cast<std::size_t>(at - list.begin());
    ui::batch([&] {
        if (activeId.peek() == id) {
            // The neighbour to the right takes over, or the left one at the end.
            const std::size_t next = index + 1 < list.size() ? index + 1 : index - 1;
            activeId.set(list[next]->id);
        }
        tabs.update([&](std::vector<std::shared_ptr<TabModel>> &l) { l.erase(l.begin() + static_cast<long>(index)); });
    });
}

void AppState::closeOtherTabs(std::uint64_t id) {
    auto keep = tab(id);
    if (!keep) {
        return;
    }
    ui::batch([&] {
        activeId.set(id);
        tabs.set({keep});
    });
}

void AppState::selectTab(std::uint64_t id) {
    if (tab(id)) {
        activeId.set(id);
    }
}

void AppState::setSelection(std::vector<std::string> names) {
    std::ranges::sort(names);
    names.erase(std::unique(names.begin(), names.end()), names.end());
    peekActive()->selection.set(std::move(names));
}

void AppState::toggleSelected(const std::string &name) {
    peekActive()->selection.update([&](std::vector<std::string> &names) {
        const auto at = std::ranges::lower_bound(names, name);
        if (at != names.end() && *at == name) {
            names.erase(at);
        } else {
            names.insert(at, name);
        }
    });
}

void AppState::clearSelection() { peekActive()->selection.set({}); }

bool AppState::isFavorite(const vfs::Location &location) const {
    return findFavorite(settings().favorites, location).has_value();
}

void AppState::addFavorite(Favorite favorite, std::optional<std::size_t> index) {
    if (findFavorite(settings.peek().favorites, favorite.location)) {
        return;
    }
    settings.update([&](Settings &s) {
        const std::size_t at = std::min(index.value_or(s.favorites.size()), s.favorites.size());
        s.favorites.insert(s.favorites.begin() + static_cast<std::ptrdiff_t>(at), std::move(favorite));
    });
}

void AppState::removeFavorite(const vfs::Location &location) {
    if (!findFavorite(settings.peek().favorites, location)) {
        return;
    }
    settings.update([&](Settings &s) {
        std::erase_if(s.favorites, [&](const Favorite &f) { return f.location == location; });
    });
}

void AppState::renameFavorite(std::size_t index, std::string name) {
    if (index >= settings.peek().favorites.size() || name.empty()) {
        return;
    }
    settings.update([&](Settings &s) { s.favorites[index].name = std::move(name); });
}

void AppState::moveFavorite(std::size_t from, std::size_t to) {
    const std::size_t count = settings.peek().favorites.size();
    if (from >= count || to >= count || from == to) {
        return;
    }
    settings.update([&](Settings &s) {
        Favorite moving = std::move(s.favorites[from]);
        s.favorites.erase(s.favorites.begin() + static_cast<std::ptrdiff_t>(from));
        s.favorites.insert(s.favorites.begin() + static_cast<std::ptrdiff_t>(to), std::move(moving));
    });
}

bool AppState::isHiddenItem(const vfs::Location &location) const {
    return std::ranges::find(settings().hiddenItems, location) != settings().hiddenItems.end();
}

void AppState::setHidden(const std::vector<vfs::Location> &locations, bool hidden) {
    Settings next = settings.peek();
    bool changed = false;
    for (const vfs::Location &location : locations) {
        const auto at = std::ranges::find(next.hiddenItems, location);
        if (hidden && at == next.hiddenItems.end()) {
            next.hiddenItems.push_back(location);
            changed = true;
        } else if (!hidden && at != next.hiddenItems.end()) {
            next.hiddenItems.erase(at);
            changed = true;
        }
    }
    if (changed) {
        settings.set(std::move(next));
    }
}

void AppState::itemMoved(const vfs::Location &location, const vfs::Location &target) {
    Settings next = settings.peek();
    if (relocateUserItems(next, location, target)) {
        settings.set(std::move(next));
    }
}

void AppState::itemsGone(const std::vector<vfs::Location> &gone) {
    Settings next = settings.peek();
    if (dropUserItems(next, gone)) {
        settings.set(std::move(next));
    }
}

Settings AppState::snapshot() const {
    Settings s = settings();
    s.tabs.clear();
    const auto &list = tabs();
    const std::uint64_t active = activeId();
    for (std::size_t i = 0; i < list.size(); ++i) {
        SavedTab saved;
        for (const Route &route : list[i]->stack()) {
            if (route.kind == Route::Kind::browse && route.location) {
                saved.history.push_back(*route.location);
            }
        }
        s.tabs.push_back(std::move(saved));
        if (list[i]->id == active) {
            s.activeTab = i;
        }
    }
    return s;
}

}  // namespace filepan::app
