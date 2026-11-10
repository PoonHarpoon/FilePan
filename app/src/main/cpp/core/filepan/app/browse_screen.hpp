// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "filepan/app/app.hpp"
#include "filepan/app/directory_model.hpp"
#include "filepan/ui/animation.hpp"
#include "filepan/ui/components/components.hpp"
#include "filepan/ui/ui_loop.hpp"
#include "filepan/ui/virtual_list.hpp"

// The browsing screen (glass-signal-mobile.md, "Screen anatomy"): the active tab's folder as a
// list of FileRows or a grid of GridTiles, split into `FOLDERS` and `FILES` sections, over the
// background and under the app's top bar, with the bottom dock.
//
// - It follows the active tab's route: a new folder is listed (streaming), the old one's scroll
//   position is saved in the tab and restored on the way back.
// - Tap opens: folders and archives navigate, local images and videos go to the viewer, other
//   files to Platform::openFile (copied out first if they aren't local). Long press
//   selects, and dragging after it selects a range; in selection mode a tap toggles.
// - Pull down at the top to refresh: past kPullThreshold the indicator arms (a tick, the accent),
//   and on release it spins until the folder is listed again, for at least kMinRefreshMs, then a
//   toast says how it went.
// - Empty folders, filters that match nothing and errors show the Orb empty state, errors with
//   a retry.
namespace filepan::app {

// Whether a refresh shows the indicator and a toast (the user asked for it) or runs quietly (a
// setting changed what the rows show).
enum class RefreshFeedback : std::uint8_t { silent, shown };

class BrowseScreen final : public ui::Node, private ui::FrameClock::Ticker {
public:
    struct Item {
        enum Type : std::uint32_t { row = 0, header = 1, tiles = 2 };
        Type type = row;
        int section = 0;     // header: 1, 2 (its key).
        std::string label;   // header: "Folders".
        std::size_t begin = 0;  // Entries [begin, end) of the model's visible().
        std::size_t end = 0;
    };

    static constexpr int kGridColumns = 3;
    static constexpr float kPullThreshold = 72;  // dp
    // Pulling back under this disarms, so a finger resting on the threshold doesn't tick again.
    static constexpr float kPullDisarm = 64;  // dp
    // A refresh that finishes sooner keeps the indicator up this long, so it registers.
    static constexpr std::int64_t kMinRefreshMs = 400;

    static std::shared_ptr<BrowseScreen> create(AppContext &context);
    explicit BrowseScreen(AppContext &context);
    ~BrowseScreen() override;

    [[nodiscard]] DirectoryModel &model() const { return *model_; }
    [[nodiscard]] ui::VirtualList &list() const { return *list_; }
    [[nodiscard]] ui::components::BottomDock &dock() const { return *dock_; }
    [[nodiscard]] ui::components::ScrollThumb &scrollThumb() const { return *scrollThumb_; }
    // The back-to-top button, shown once the list has scrolled a screen away.
    [[nodiscard]] ui::Node &topButton() const { return *topButton_; }
    void scrollToTop();
    // The utilitarian status line: counts, sizes and free space.
    [[nodiscard]] ui::Text &statusText() const { return *statusText_; }
    [[nodiscard]] const std::vector<Item> &items() const { return items_; }
    // The empty or error state, or null while the folder has entries (or is loading).
    [[nodiscard]] ui::Node *emptyState() const;

    // What a tap, a long press, and a long-press drag do, by index into model().visible().
    void activate(std::size_t entry);
    void longPress(std::size_t entry);
    void dragTo(std::size_t entry);
    void endDrag();
    // The entry under a window position (px), if any.
    [[nodiscard]] std::optional<std::size_t> entryAt(gfx::Vec2 window) const;
    // The display item showing an entry.
    [[nodiscard]] std::optional<std::size_t> itemOf(std::size_t entry) const;

    void refresh(RefreshFeedback feedback = RefreshFeedback::silent);
    // Binds the rows on screen again, for what they show that isn't the listing (thumbnails, counts).
    void rebindVisible();
    // Opens file's folder in the active tab and, once it's listed, scrolls to the file and tints
    // it for kRevealFlashMs (a file favorite).
    void reveal(const vfs::Location &file);
    static constexpr std::int64_t kRevealFlashMs = 1600;
    // The entry tinted by reveal, while it is.
    [[nodiscard]] const std::optional<std::string> &flashing() const { return flash_; }
    // The sort options the overflow menu leads with: sort by name, size, date or type, the order,
    // and folders first, then the view (list or grid). kSortControlsHeight dp tall.
    static constexpr float kSortControlsHeight = 247;
    [[nodiscard]] ui::NodePtr sortControls();
    void showNewMenu(const ui::Node &anchor);
    // The selection's other actions; all adds the dock's (a row's long-press menu).
    void showMoreMenu(const ui::Node &anchor, bool all = false);
    void selectAll();
    // Hides or unhides the targets (user_items.hpp), clears the selection, and offers an undo.
    void setHidden(const FileTargets &targets, bool hidden);
    [[nodiscard]] FileTargets selectionTargets() const;
    // Pull-to-refresh: how far the list is pulled, dp.
    [[nodiscard]] float pull() const { return pull_.peek(); }
    // Pulled past the threshold (releasing refreshes).
    [[nodiscard]] bool armed() const { return armed_.peek(); }
    // A refresh is running, or its indicator is still up.
    [[nodiscard]] bool refreshing() const { return refreshing_; }
    // The indicator is spinning (never under reduced motion).
    [[nodiscard]] bool spinning() const { return ticking_; }

private:
    void construct();
    void followRoute();
    void rebuildItems();
    [[nodiscard]] std::uint64_t keyOf(std::size_t index) const;
    void bindItem(std::size_t index, ui::Node &node);
    ui::components::FileItem fileItem(std::size_t entry);
    void updateEmptyState();
    void tryReveal();
    void onPointer(const ui::PointerEvent &event);
    void setPull(float pull);
    void finishRefresh();
    void endRefresh();
    void cancelRefresh();
    void setSpinning(bool on);
    bool tick(std::int64_t nowNs) override;
    void saveScroll();
    void restoreScroll();
    void pruneRequests();
    [[nodiscard]] std::vector<vfs::Entry> selectedEntries() const;
    std::shared_ptr<TabModel> tab() const;

    AppContext &context_;
    // For the destructor: the screen may outlive the App (and its context) as the root's content.
    ui::UiLoop &loop_;
    std::shared_ptr<DirectoryModel> model_;
    std::shared_ptr<ui::VirtualList> list_;
    std::shared_ptr<ui::components::BottomDock> dock_;
    std::shared_ptr<ui::components::ScrollThumb> scrollThumb_;
    std::shared_ptr<ui::Node> topButton_;
    std::shared_ptr<ui::Node> emptySlot_;
    std::shared_ptr<ui::Node> statusLine_;
    std::shared_ptr<ui::Text> statusText_;
    std::shared_ptr<ui::Node> pullIndicator_;
    std::vector<Item> items_;
    std::vector<std::size_t> itemOfEntry_;
    std::unordered_map<std::uint64_t, std::size_t> itemOfKey_;
    // Which entry each bound row or tile node shows.
    std::unordered_map<const ui::Node *, std::size_t> boundEntry_;
    std::optional<vfs::Location> shown_;
    std::uint64_t shownTab_ = 0;
    ViewMode view_ = ViewMode::list;
    // A scroll position waiting for its anchor entry to be listed.
    std::optional<TabModel::ScrollPosition> restore_;
    // Long-press drag: the entry it started on and the selection before it.
    std::optional<std::size_t> dragAnchor_;
    std::vector<std::string> dragBase_;
    std::optional<std::int32_t> pointer_;
    ui::AnimatedValue<float> pull_{0};
    ui::Signal<bool> armed_{false};
    bool refreshing_ = false;
    bool feedback_ = false;
    std::int64_t refreshStartNs_ = 0;
    std::optional<ui::UiLoop::TimerId> refreshTimer_;
    std::shared_ptr<ui::components::Icon> pullIcon_;
    // The spin: the arrow's angle at release, the clock then, and whether the ticker runs.
    float spinBase_ = 0;
    std::int64_t spinStartNs_ = -1;
    bool ticking_ = false;
    std::string emptyKey_;
    struct Reveal {
        vfs::Location folder;
        std::string name;
    };
    std::optional<Reveal> revealing_;
    std::optional<std::string> flash_;
    std::uint64_t salt_ = 0;
};

}  // namespace filepan::app
