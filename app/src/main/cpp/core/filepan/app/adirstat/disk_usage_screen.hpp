// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "filepan/adirstat/file_types.hpp"
#include "filepan/adirstat/scanner.hpp"
#include "filepan/adirstat/treemap.hpp"
#include "filepan/app/app.hpp"
#include "filepan/ui/virtual_list.hpp"

// The disk usage screen (the adirstat module, built with FILEPAN_ADIRSTAT): scans a local folder
// and shows where its space goes. A treemap of flat, theme-colored tiles sits over a size-sorted
// tree of folders, or the totals by file type; tapping a tile or a row selects it in both. A long press
// starts a multi-selection that can be opened, shown in its folder or deleted, and deleting takes
// the files out of the tree without a rescan.
namespace filepan::app {

struct DiskUsageOptions {
    // Says how much space the files take on disk when it's far from their size (sparse or
    // compressed files). Off for golden renders: file systems report fresh files' blocks late.
    bool showAllocated = true;
};

// The module's screens, for AppOptions::screens.
std::vector<ScreenSpec> diskUsageScreenSpecs(DiskUsageOptions options = {});

class TreemapView;

class DiskUsageScreen final : public ui::Node {
public:
    enum class Phase : std::uint8_t { scanning, done, failed };
    enum class View : std::uint8_t { folders, types };

    // One line of the list under the treemap.
    struct Item {
        enum class Kind : std::uint8_t { node, category, extension };
        Kind kind = Kind::node;
        adirstat::NodeId node = adirstat::kNoNode;  // kind node
        int depth = 0;
        adirstat::Category category = adirstat::Category::kOther;  // kind category and extension
        std::size_t extension = 0;  // Into stats().extensions, for kind extension.
    };

    // Starts scanning root at once.
    static std::shared_ptr<DiskUsageScreen> create(AppContext &context, std::string root,
                                                   DiskUsageOptions options = {});
    DiskUsageScreen(AppContext &context, std::string root, DiskUsageOptions options);
    ~DiskUsageScreen() override;

    // --- For tests and the parts ---
    [[nodiscard]] Phase phase() const { return phase_; }
    [[nodiscard]] const adirstat::Tree *tree() const { return tree_.get(); }
    [[nodiscard]] const std::string &rootPath() const { return root_; }
    [[nodiscard]] adirstat::NodeId zoomRoot() const { return zoom_; }
    [[nodiscard]] adirstat::NodeId focus() const { return focus_; }
    [[nodiscard]] const std::set<adirstat::NodeId> &selection() const { return selection_; }
    [[nodiscard]] const std::vector<Item> &items() const { return items_; }
    [[nodiscard]] const adirstat::TypeStats &stats() const { return stats_; }
    [[nodiscard]] View view() const { return view_.peek(); }
    [[nodiscard]] ui::VirtualList &list() const { return *list_; }
    [[nodiscard]] TreemapView &treemap() const { return *treemap_; }
    [[nodiscard]] const std::vector<adirstat::Tile> &tiles() const { return tiles_; }
    [[nodiscard]] std::optional<adirstat::Category> highlight() const { return highlight_; }
    [[nodiscard]] bool scrolledDown() const;
    // Which top-level folder of the map a node falls under, by size: picks its tiles' hue.
    [[nodiscard]] int groupOf(adirstat::NodeId id) const;

    void rescan();
    // The back button: closes the selection, the open file type, scrolls up, then zooms out, in that
    // order; false once there is nothing left to close, to leave the screen.
    bool back();
    void scrollToTop();
    void setView(View view);
    // Lays the treemap out from this directory; the root by default.
    void zoomTo(adirstat::NodeId dir);
    void zoomOut();
    // Selects one node in the treemap and the list, opening its ancestors in the list.
    void setFocus(adirstat::NodeId id);
    void toggleExpanded(adirstat::NodeId dir);
    void toggleSelected(adirstat::NodeId id);
    void clearSelection();
    void setHighlight(std::optional<adirstat::Category> category);
    // Through FileActions::removeFrom, after its confirmation.
    void deleteSelection();
    void openSelection();
    void revealSelection();

    // The treemap's gestures, in its local px.
    void tapTreemap(float x, float y);
    void longPressTreemap(float x, float y);
    void doubleTapTreemap(float x, float y);
    // The list's.
    void tapItem(std::size_t index);
    void longPressItem(std::size_t index);

    // The treemap's frame size changed.
    void treemapResized();

private:
    void construct();
    void poll();
    void finishScan(Result<std::unique_ptr<adirstat::Tree>> result);
    void relayout();
    void rebuildItems();
    void refreshHeader();
    void refreshSelectionBar();
    void applyTheme(const ui::Theme &t);
    void placeTopButton();
    void afterDelete(std::vector<adirstat::NodeId> removed);
    [[nodiscard]] std::vector<adirstat::NodeId> effectiveSelection() const;
    [[nodiscard]] vfs::Location locationOf(adirstat::NodeId id) const;
    [[nodiscard]] vfs::Entry entryOf(adirstat::NodeId id) const;
    void bindItem(std::size_t index, ui::Node &node);

    AppContext &context_;
    // Outlives the App, and so the context, which can go first.
    runtime::AppServices &services_;
    std::string root_;
    DiskUsageOptions options_;
    Phase phase_ = Phase::scanning;
    std::string failure_;
    std::shared_ptr<adirstat::ScanProgress> progress_;
    CancellationToken cancel_;
    ui::UiLoop::TimerId pollTimer_ = 0;
    std::uint64_t scanGeneration_ = 0;

    std::unique_ptr<adirstat::Tree> tree_;
    adirstat::NodeId zoom_ = 0;
    adirstat::NodeId focus_ = adirstat::kNoNode;
    std::set<adirstat::NodeId> selection_;
    std::set<adirstat::NodeId> expanded_;
    std::optional<adirstat::Category> highlight_;
    std::optional<adirstat::Category> openCategory_;
    // A signal so the Folders / File types switch follows it.
    ui::Signal<View> view_{View::folders};
    adirstat::TypeStats stats_;
    std::vector<Item> items_;

    std::vector<adirstat::Tile> tiles_;
    std::unordered_map<adirstat::NodeId, std::size_t> tileOf_;
    std::unordered_map<adirstat::NodeId, int> groups_;

    std::shared_ptr<ui::VirtualList> list_;
    std::shared_ptr<TreemapView> treemap_;
    std::shared_ptr<ui::Node> header_;
    std::shared_ptr<ui::Text> pathText_;
    std::shared_ptr<ui::Text> statusText_;
    std::shared_ptr<ui::Text> detailText_;
    std::shared_ptr<ui::Node> zoomRow_;
    std::shared_ptr<ui::Text> zoomText_;
    std::shared_ptr<ui::Node> zoomOutButton_;
    std::shared_ptr<ui::Node> cancelButton_;
    std::shared_ptr<ui::Node> tabs_;
    std::shared_ptr<ui::Node> topButton_;
    std::shared_ptr<ui::Node> selectionBar_;
    std::shared_ptr<ui::Text> selectionText_;
    std::shared_ptr<ui::Node> openButton_;
    std::shared_ptr<ui::Node> revealButton_;
    std::unordered_map<const ui::Node *, std::size_t> boundItem_;
    std::int64_t lastTapNs_ = 0;
    gfx::Vec2 lastTap_{};
    std::uint64_t themeGeneration_ = 0;
    float safeBottom_ = 0;
};

// The treemap: flat tiles in the theme's colors, files tinted along its spectrum by how much of
// the map they take, folders framing their children. The focus and the selection are outlined on
// a layer above.
class TreemapView final : public ui::Node {
public:
    explicit TreemapView(DiskUsageScreen &screen);

    // The layout or the highlight changed: redraws the tiles and the outlines.
    void tilesChanged() {
        markPaintDirty();
        outlinesChanged();
    }
    // The selection or focus moved: redraws the outlines only.
    void outlinesChanged() { outlines_->markPaintDirty(); }

protected:
    void paint(ui::PaintContext &context) override;
    void onSizeChanged() override;

private:
    void paintOutlines(ui::PaintContext &context);

    DiskUsageScreen &screen_;
    std::shared_ptr<ui::Node> outlines_;
    gfx::Vec2 down_{};
};

}  // namespace filepan::app
