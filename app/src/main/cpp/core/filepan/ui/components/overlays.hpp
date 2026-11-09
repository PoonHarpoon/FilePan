// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <deque>
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
#include "filepan/ui/ui_loop.hpp"

// Containers that float over the screen: bottom sheets, dialogs, menus and snackbars. Each fills
// its parent (put them in the screen's overlay layer, above the content) and draws its own scrim.
// Nothing opens a separate activity.
//
// Back: the screen registers handleBack() of the topmost overlay with UiRoot::addBackHandler.
// While one is open, the screen should also hide what's behind it from semantics.
namespace filepan::ui::components {

// --- BottomSheet ---

struct BottomSheetProps {
    std::string title;
    NodePtr content;
    std::function<void()> onDismiss;  // After the dismiss animation; remove the sheet then.
    bool cornerTicks = true;          // On the header.
    // At the header's end, after the title: an edit toggle.
    NodePtr headerTrailing;
};

// A glass sheet with a 28 dp top radius, sliding up from the bottom over a scrim. Drag it down
// to dismiss: released past kDismissFraction of its height, or flung down faster than
// kDismissVelocity, it goes; otherwise it springs back. Focus is trapped inside while open, and
// in semantics it's a dialog with a dismiss action.
class BottomSheet final : public Node {
public:
    static constexpr float kDismissFraction = 0.3f;
    static constexpr float kDismissVelocity = 900;  // dp/s

    static std::shared_ptr<BottomSheet> create(BottomSheetProps props);

    // Slides in (or appears, with animate false or under reduced motion).
    void open(bool animate = true);
    void dismiss();
    [[nodiscard]] bool isOpen() const { return open_; }
    // For UiRoot::addBackHandler: dismisses and returns true while open.
    bool handleBack();

    [[nodiscard]] Node &panel() const { return *panel_; }
    // How far the panel is pushed down, dp.
    [[nodiscard]] float offset() const { return offset_.peek(); }

private:
    void release(float velocityDp);

    AnimatedValue<float> offset_{0};
    std::shared_ptr<Node> scrim_;
    std::shared_ptr<Glass> panel_;
    std::function<void()> onDismiss_;
    bool open_ = false;
};

// --- Dialog ---

struct DialogProps {
    std::string title;
    std::string message;
    NodePtr content;               // Below the message, optional.
    std::vector<NodePtr> actions;  // Buttons, right-aligned; the last is the default.
    std::function<void()> onDismiss;
    bool dismissible = true;       // Scrim taps and back dismiss.
};

// A centered glass sheet (radius 28) over a scrim, scaling in slightly; a crossfade under
// reduced motion.
class Dialog final : public Node {
public:
    static std::shared_ptr<Dialog> create(DialogProps props);

    void open(bool animate = true);
    void dismiss();
    bool handleBack();
    [[nodiscard]] bool isOpen() const { return open_; }
    [[nodiscard]] Node &card() const { return *card_; }

private:
    AnimatedValue<float> shown_{0};
    std::shared_ptr<Glass> card_;
    std::function<void()> onDismiss_;
    bool dismissible_ = true;
    bool open_ = false;
};

// --- Menu ---

struct MenuItem {
    std::string icon;
    std::string label;
    std::function<void()> onSelect;
    bool destructive = false;
    // A short mono figure at the row's end: the number of tabs.
    std::string trailing = {};
    // The icon's filled form: a star for what's already a favorite.
    bool filled = false;
};

struct MenuProps {
    std::vector<MenuItem> items;
    gfx::Rect anchor;  // dp, in the menu's parent: the button it drops from.
    std::function<void()> onDismiss;
    float width = 224;
    // Controls above the items (the browsing screen's sort options), headerHeight dp tall. Using
    // them leaves the menu open.
    NodePtr header;
    float headerHeight = 0;
};

// Where a menu goes, in dp in its parent (the window): its card's left and top edges, and the
// most it may be tall.
struct MenuPlacement {
    float left = 0;
    float top = 0;
    float maxHeight = 0;
};

// Places a menu height dp tall and width dp wide by its anchor, inside the window's safe area
// less a margin: below the anchor if it fits there, else above, else on the roomier side,
// capped to the room there (but never below minHeight) so its content scrolls.
MenuPlacement placeMenu(gfx::Rect anchor, float width, float height, float minHeight);

// A glass list anchored below (or, without room, above) a button, right-aligned to it when it
// would run off the screen, with an optional header of controls. Choosing an item or tapping
// outside dismisses it. Taller than the room by its anchor, it's capped and its content scrolls.
class Menu final : public Node {
public:
    static std::shared_ptr<Menu> create(MenuProps props);

    void dismiss();
    bool handleBack();
    [[nodiscard]] Node &card() const { return *card_; }
    // Holds the header and the items.
    [[nodiscard]] ScrollNode &scroll() const { return *scroll_; }

private:
    std::shared_ptr<Glass> card_;
    std::shared_ptr<ScrollNode> scroll_;
    std::function<void()> onDismiss_;
    bool open_ = true;
};

// --- Snackbars and toasts ---

struct SnackbarMessage {
    std::string text;
    std::string action;  // Empty for a toast.
    std::function<void()> onAction;
    // 0 picks the default: longer with an action to reach for.
    std::int64_t durationMs = 0;
};

inline constexpr std::int64_t kToastMs = 2500;
inline constexpr std::int64_t kSnackbarActionMs = 4000;

// Shows one message at a time above the dock: a glass bar with the text and an optional action.
// A new message replaces the one showing. Each times out on loop (no timeout without one), or
// goes when its action is tapped.
// A live region in semantics.
class SnackbarHost final : public Node {
public:
    // bottom: dp above the navigation bar (the dock's height and inset, by default).
    static std::shared_ptr<SnackbarHost> create(UiLoop *loop, float bottom = 92);
    ~SnackbarHost() override;

    void show(SnackbarMessage message);
    void dismissCurrent();
    // Dismisses the message showing and drops any waiting.
    void clear();
    // While held (a sheet or dialog is up, whose buttons a bar would cover), messages wait in
    // the queue; the one showing goes back to its front, to show again once released.
    void setHeld(bool held);
    [[nodiscard]] bool held() const { return held_; }
    [[nodiscard]] std::size_t queued() const { return queue_.size(); }
    [[nodiscard]] const std::optional<SnackbarMessage> &current() const { return current_; }

private:
    void showNext();

    UiLoop *loop_ = nullptr;
    AnimatedValue<float> shown_{0};
    std::shared_ptr<Glass> bar_;
    std::shared_ptr<Text> text_;
    std::shared_ptr<Node> actionSlot_;
    std::deque<SnackbarMessage> queue_;
    std::optional<SnackbarMessage> current_;
    std::optional<UiLoop::TimerId> timer_;
    std::uint64_t generation_ = 0;
    bool held_ = false;
};

}  // namespace filepan::ui::components
