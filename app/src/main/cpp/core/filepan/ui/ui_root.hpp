// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "filepan/byte_writer.hpp"
#include "filepan/gfx/display_list.hpp"
#include "filepan/gfx/text.hpp"
#include "filepan/ui/animation.hpp"
#include "filepan/ui/environment.hpp"
#include "filepan/ui/focus.hpp"
#include "filepan/ui/gesture.hpp"
#include "filepan/ui/node.hpp"
#include "filepan/ui/semantics.hpp"
#include "filepan/ui/text_input.hpp"
#include "filepan/ui/ui_loop.hpp"

// Ties the framework together on the UI thread.
//
// Events come in (pointer, key, back, accessibility actions) and update state. A frame then:
//   1. ticks animations (the frame clock),
//   2. runs queued effects,
//   3. lays out what's dirty,
//   4. re-records dirty display lists (clean subtrees are reused by reference),
//   5. rebuilds the semantics snapshot if accessibility is on and anything changed,
// and returns the root display list for the render thread, or nothing when there was nothing to
// do. Frames are only requested while something is dirty or animating: an idle UI does no work.
//
// UiRoot installs itself as the reactive scheduler and the frame clock's frame requester for
// the thread, and its Environment becomes environment(). Use one per UI thread.
namespace filepan::ui {

class UiRoot final : public NodeHost {
public:
    UiRoot(UiLoop &loop, gfx::TextSystem *text);
    ~UiRoot();
    UiRoot(const UiRoot &) = delete;
    UiRoot &operator=(const UiRoot &) = delete;

    [[nodiscard]] Environment &env() { return environment_; }
    [[nodiscard]] GestureArena &arena() { return arena_; }
    [[nodiscard]] FocusManager &focus() { return focus_; }
    [[nodiscard]] TextInputService &textInput() { return textInput_; }
    [[nodiscard]] UiLoop &loop() { return loop_; }

    void setContent(NodePtr content);
    [[nodiscard]] Node *content() const { return content_.get(); }

    // --- Input (UI thread) ---
    void dispatchPointer(PointerEvent event);
    // To the focused node and its ancestors; Tab moves focus if nobody handles it.
    bool dispatchKey(const KeyEvent &event);
    // Back handlers run newest first; false means nobody handled it and the shell should
    // finish. A handler registered inside an owner (a node's scope) is removed with it.
    bool dispatchBack();
    using BackHandlerId = std::uint64_t;
    BackHandlerId addBackHandler(std::function<bool()> handler);
    void removeBackHandler(BackHandlerId id);

    // --- Frames ---
    struct Frame {
        std::shared_ptr<const gfx::DisplayList> displayList;  // Never null; may be empty.
        bool semanticsChanged = false;
    };
    // Produces a frame for frameTimeNs, or nothing if idle.
    std::optional<Frame> frame(std::int64_t frameTimeNs);
    // Whether a frame would do anything.
    [[nodiscard]] bool needsFrame() const;
    // Called with each produced frame when the loop runs frames.
    void setFrameSink(std::function<void(const Frame &)> sink) { frameSink_ = std::move(sink); }

    // --- Accessibility ---
    void setAccessibilityEnabled(bool enabled);
    [[nodiscard]] bool accessibilityEnabled() const { return accessibilityEnabled_; }
    // Writes the semantics changes since the last call (see SemanticsTree for the format).
    // Returns false and writes nothing if there are none.
    bool takeSemanticsUpdate(ByteWriter &out, bool full = false);
    bool performAccessibilityAction(std::uint32_t id, Action action, const ActionArgs &args = {});
    [[nodiscard]] const SemanticsTree &semantics() const { return semantics_; }

    // --- NodeHost ---
    void requestFrame() override;
    void nodeDetached(Node &node) override;
    void requestFocus(Node *node) override;
    [[nodiscard]] Node *focusedNode() const override { return focus_.focused(); }
    void semanticsChanged() override {
        semanticsDirty_ = true;
        if (accessibilityEnabled_) {
            requestFrame();
        }
    }

    // Per-frame counters, for tests and profiling.
    struct FrameStats {
        bool laidOut = false;
        bool recorded = false;
        std::uint32_t layoutComputes = 0;
    };
    [[nodiscard]] const FrameStats &lastFrameStats() const { return stats_; }

private:
    [[nodiscard]] LayoutContext layoutContext() const;
    [[nodiscard]] gfx::Vec2 toLocal(Node &node, gfx::Vec2 window) const;
    void scheduleArenaDeadline();

    UiLoop &loop_;
    gfx::TextSystem *text_;
    Environment environment_;
    Environment *previousEnvironment_;
    Scope scope_;
    NodePtr content_;
    GestureArena arena_;
    TextInputService textInput_;
    FocusManager focus_{textInput_};
    SemanticsTree semantics_;

    // Per pointer: the hit path, and which node each recognizer in the arena belongs to.
    struct Route {
        std::vector<NodePtr> path;
        std::unordered_map<const GestureRecognizer *, std::weak_ptr<Node>> owners;
    };
    std::unordered_map<std::int32_t, Route> routes_;
    std::optional<UiLoop::TimerId> deadlineTimer_;

    struct BackHandler {
        BackHandlerId id;
        std::function<bool()> handler;
    };
    std::vector<BackHandler> backHandlers_;
    BackHandlerId nextBackHandler_ = 1;

    std::function<void(const Frame &)> frameSink_;
    gfx::Size laidOutSize_{-1, -1};
    bool inFrame_ = false;
    bool frameRequested_ = false;
    bool semanticsDirty_ = true;
    bool semanticsPending_ = false;
    bool accessibilityEnabled_ = false;
    FrameStats stats_;
};

}  // namespace filepan::ui
