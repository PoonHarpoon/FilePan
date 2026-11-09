// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "filepan/app/settings.hpp"
#include "filepan/ui/reactive.hpp"
#include "filepan/vfs/location.hpp"

// The app's state as signals: the settings, the tabs and what each one shows. Screens read it in
// binds and change it through the methods here; nothing else holds navigation state.
//
// Navigation: each tab has a stack of routes whose bottom is always the storage home. Opening a
// folder pushes a browse route, a registered screen (settings, about) pushes a screen route, and
// back pops. Each tab also keeps a scroll position per folder it has shown, its selection (names
// in the current folder), its view mode, and its in-folder filter.
namespace filepan::app {

struct Route {
    enum class Kind : std::uint8_t { home, browse, screen };

    Kind kind = Kind::home;
    std::optional<vfs::Location> location;  // browse
    std::string screenId;                   // screen

    static Route home() { return {}; }
    static Route browse(vfs::Location location) { return {Kind::browse, std::move(location), {}}; }
    static Route screen(std::string id) { return {Kind::screen, std::nullopt, std::move(id)}; }

    bool operator==(const Route &) const = default;
};

class TabModel {
public:
    explicit TabModel(std::uint64_t id, ViewMode view) : id(id), view(view) {}

    const std::uint64_t id;
    // Never empty; the first route is the home.
    ui::Signal<std::vector<Route>> stack{std::vector<Route>{Route::home()}};
    ui::Signal<ViewMode> view;
    // Names in the current folder, sorted and unique.
    ui::Signal<std::vector<std::string>> selection;
    // The in-folder filter: shown while searching, applied while non-empty.
    ui::Signal<bool> searching{false};
    ui::Signal<std::string> filter;
    // Opens the filter, or closes it and clears what it held.
    void setSearching(bool on);

    // Untracked.
    [[nodiscard]] const Route &current() const { return stack.peek().back(); }
    // Tracked.
    [[nodiscard]] Route trackCurrent() const { return stack().back(); }

    // Where each folder it has shown was scrolled to, so going back lands there: the name of the
    // entry at the top (the list re-anchors on it once listed) and the offset in dp.
    struct ScrollPosition {
        std::string anchor;
        float offset = 0;
    };
    [[nodiscard]] ScrollPosition scrollFor(const vfs::Location &location) const;
    void setScroll(const vfs::Location &location, ScrollPosition position);

private:
    std::unordered_map<std::string, ScrollPosition> scroll_;
};

class AppState {
public:
    // Restores the settings' saved tabs; with none, opens one tab on home, then homePath.
    AppState(Settings settings, std::string homePath);

    ui::Signal<Settings> settings;
    ui::Signal<std::vector<std::shared_ptr<TabModel>>> tabs;
    ui::Signal<std::uint64_t> activeId;

    // Tracked: re-runs a bind when the active tab changes.
    [[nodiscard]] std::shared_ptr<TabModel> active() const;
    [[nodiscard]] std::shared_ptr<TabModel> peekActive() const;
    [[nodiscard]] std::shared_ptr<TabModel> tab(std::uint64_t id) const;

    // --- Navigation, on the active tab ---
    void navigate(const vfs::Location &location);
    // Back to the tab's home, dropping its stack.
    void navigateHome();
    void openScreen(std::string id);
    // Clears the selection, then closes the filter, then pops a route. False at the home with
    // nothing to clear: the system handles back.
    bool back();
    // Pops one route without clearing anything else; false at the home.
    bool pop();

    // --- Tabs ---
    // A new tab after the active one, on location (or the home), and selects it.
    std::uint64_t addTab(std::optional<vfs::Location> location);
    // The last tab can't be closed; it goes back to the home instead.
    void closeTab(std::uint64_t id);
    void closeOtherTabs(std::uint64_t id);
    void selectTab(std::uint64_t id);

    // --- Selection, in the active tab ---
    void setSelection(std::vector<std::string> names);
    void toggleSelected(const std::string &name);
    void clearSelection();

    // --- Settings ---
    template <typename F>
    void updateSettings(F &&fn) {
        settings.update(std::forward<F>(fn));
    }

    // --- Favorites (user_items.hpp), in the settings ---
    // Tracked.
    [[nodiscard]] bool isFavorite(const vfs::Location &location) const;
    // At the end, or at index; nothing if the location already is one.
    void addFavorite(Favorite favorite, std::optional<std::size_t> index = std::nullopt);
    void removeFavorite(const vfs::Location &location);
    // An empty name is ignored.
    void renameFavorite(std::size_t index, std::string name);
    void moveFavorite(std::size_t from, std::size_t to);

    // --- Hidden items (user_items.hpp), in the settings ---
    // Tracked.
    [[nodiscard]] bool isHiddenItem(const vfs::Location &location) const;
    // Hides or unhides each; nothing for those already so.
    void setHidden(const std::vector<vfs::Location> &locations, bool hidden);

    // FilePan's own operations: location was renamed or moved to target, or these were deleted.
    // Favorites and hidden items at or below them follow, or go.
    void itemMoved(const vfs::Location &location, const vfs::Location &target);
    void itemsGone(const std::vector<vfs::Location> &gone);

    // The settings with the open tabs folded in, as they're saved. Tracked.
    [[nodiscard]] Settings snapshot() const;

    [[nodiscard]] const std::string &homePath() const { return homePath_; }

private:
    std::shared_ptr<TabModel> makeTab(std::optional<vfs::Location> location);
    void resetTabState(TabModel &tab);

    std::string homePath_;
    std::uint64_t nextTabId_ = 1;
};

}  // namespace filepan::app
