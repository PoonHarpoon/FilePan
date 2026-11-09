// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "filepan/ui/animation.hpp"
#include "filepan/ui/components/common.hpp"
#include "filepan/ui/components/surfaces.hpp"
#include "filepan/ui/node.hpp"
#include "filepan/ui/scroll.hpp"

// Navigation: the path line of the top bar, the top bar itself, and the floating bottom dock
// (glass-signal-mobile.md, "Screen anatomy").
namespace filepan::ui::components {

// --- Breadcrumb ---

struct BreadcrumbProps {
    Prop<std::vector<std::string>> segments;  // From the root: {"Internal", "Download", "Papers"}.
    std::function<void(std::size_t index)> onSelect;
};

// The whole path on one line in the body face, segments separated by a faint `/`: ancestors at
// 62% ink, the current folder at full ink and weight 650. When it doesn't fit, whole segments
// are cut from the start behind a `…` so the current folder stays in view (if even that one is
// too long, its own end is cut). Each segment is a button that navigates to it.
class Breadcrumb final : public Node {
public:
    static std::shared_ptr<Breadcrumb> create(BreadcrumbProps props);

    // The tappable segment nodes, in order.
    [[nodiscard]] const std::vector<Node *> &segmentNodes() const { return segments_; }
    // How many leading segments are cut behind the ellipsis.
    [[nodiscard]] std::size_t hiddenCount() const { return hidden_; }

    [[nodiscard]] bool hasCustomLayout() const override { return true; }
    void layoutCustom(LayoutEngine &engine, gfx::Size size) override;

private:
    std::shared_ptr<Text> ellipsis_;
    std::vector<Node *> segments_;
    std::size_t hidden_ = 0;
};

// --- TopBar ---

struct TopBarProps {
    NodePtr content;  // The 48 dp row: usually the path and an overflow button.
    // The list's scroll offset (ScrollNode::offsetSignal()): the bar slides away while it grows
    // and comes back when it shrinks or reaches the top.
    std::optional<Signal<float>> scrollOffset;
};

// A floating live-blur glass pill under the status bar holding one 48 dp row. Content scrolls
// under it; give the list a top padding of height().
class TopBar final : public Node {
public:
    static std::shared_ptr<TopBar> create(TopBarProps props);

    // Height in dp, status bar included.
    [[nodiscard]] float height() const;
    // 1 showing, 0 slid away.
    [[nodiscard]] float visibility() const { return reveal_.peek(); }
    void show(bool show);

private:
    AnimatedValue<float> reveal_{1};
    std::shared_ptr<Glass> glass_;
    float lastOffset_ = 0;
    float travel_ = 0;
};

// --- BottomDock ---

struct DockAction {
    std::string icon;
    std::string label;
    std::function<void()> onTap;
    bool destructive = false;
    // The icon's filled form while true (IconButtonProps::filled).
    Prop<bool> filled = false;
    // Hidden, it takes no place in the row.
    Prop<bool> shown = true;
};

struct BottomDockProps {
    std::vector<DockAction> idle;       // New, search, view, select.
    std::vector<DockAction> selection;  // Copy, move, delete, share, more.
    Prop<bool> selecting = false;
    Prop<int> selectionCount = 0;
};

// The floating glass pill at the bottom: 64 dp tall, inset 16 dp from the sides and the
// navigation bar. It switches between idle actions and selection actions (led by an `N SELECTED`
// stamp) with a short slide and fade, or a 120 ms crossfade under reduced motion.
class BottomDock final : public Node {
public:
    static std::shared_ptr<BottomDock> create(BottomDockProps props);

    // 0 idle, 1 selecting; in between while switching.
    [[nodiscard]] float modeProgress() const { return mode_.peek(); }
    [[nodiscard]] Node &idleRow() const { return *idle_; }
    [[nodiscard]] Node &selectionRow() const { return *selection_; }

private:
    AnimatedValue<float> mode_{0};
    std::shared_ptr<Node> idle_;
    std::shared_ptr<Node> selection_;
};

}  // namespace filepan::ui::components
