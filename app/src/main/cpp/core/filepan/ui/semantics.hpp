// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "filepan/byte_writer.hpp"
#include "filepan/gfx/geometry.hpp"

// Accessibility semantics.
//
// Each node may declare a role, strings, state flags and actions (Node::setSemantics). A node
// with a role, a label or actions becomes a semantics node; the others are transparent and their
// semantic descendants attach to the nearest semantic ancestor. mergeDescendants folds the
// labels of everything below into one node (a list row read as one item); hidden drops a
// decorative subtree.
//
// SemanticsTree turns the node tree into a flat snapshot with stable ids (the node ids) and
// screen bounds, diffs it against the previous one, and serializes the changes for Kotlin's
// AccessibilityNodeProvider. Actions come back by node id through UiRoot.
namespace filepan::ui {

class Node;

enum class Role : std::uint8_t {
    none,
    button,
    toggleSwitch,
    checkbox,
    radioButton,
    text,
    heading,
    list,
    listItem,
    grid,
    textField,
    image,
    scroll,
    tab,
    tabList,
    progress,
    slider,
    dialog,
    menu,
    menuItem,
};

// State flags; a bitmask in Semantics::flags.
namespace semantics_flags {
inline constexpr std::uint16_t kCheckable = 1 << 0;
inline constexpr std::uint16_t kChecked = 1 << 1;
inline constexpr std::uint16_t kSelected = 1 << 2;
inline constexpr std::uint16_t kDisabled = 1 << 3;
inline constexpr std::uint16_t kExpandable = 1 << 4;
inline constexpr std::uint16_t kExpanded = 1 << 5;
inline constexpr std::uint16_t kFocusable = 1 << 6;
inline constexpr std::uint16_t kFocused = 1 << 7;
inline constexpr std::uint16_t kPassword = 1 << 8;
inline constexpr std::uint16_t kLiveRegion = 1 << 9;
inline constexpr std::uint16_t kMultiline = 1 << 10;
}  // namespace semantics_flags

// Actions; a bitmask in Semantics::actions.
enum class Action : std::uint32_t {
    click = 1u << 0,
    longClick = 1u << 1,
    scrollForward = 1u << 2,
    scrollBackward = 1u << 3,
    setText = 1u << 4,
    focus = 1u << 5,
    clearFocus = 1u << 6,
    dismiss = 1u << 7,
    expand = 1u << 8,
    collapse = 1u << 9,
    setSelection = 1u << 10,
    custom = 1u << 31,  // ActionArgs::customId says which.
};

constexpr std::uint32_t operator|(Action a, Action b) {
    return static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b);
}
constexpr std::uint32_t operator|(std::uint32_t a, Action b) { return a | static_cast<std::uint32_t>(b); }

struct ActionArgs {
    std::string text;            // setText.
    std::uint32_t customId = 0;  // custom.
    std::int32_t selectionStart = -1;
    std::int32_t selectionEnd = -1;
};

struct CustomAction {
    std::uint32_t id = 0;
    std::string label;

    bool operator==(const CustomAction &) const = default;
};

struct Semantics {
    Role role = Role::none;
    std::string label;
    std::string value;
    std::string hint;
    std::uint16_t flags = 0;
    std::uint32_t actions = 0;
    std::vector<CustomAction> customActions;
    bool mergeDescendants = false;
    bool hidden = false;
    // Handles actions routed back by id. Returns whether it handled the action.
    std::function<bool(Action, const ActionArgs &)> onAction;

    [[nodiscard]] bool has(Action action) const {
        return (actions & static_cast<std::uint32_t>(action)) != 0;
    }
};

// One node of the flat snapshot.
struct SemanticsNode {
    std::uint32_t id = 0;
    std::uint32_t parent = 0;  // 0 for the root.
    gfx::Rect bounds;          // Window px, clipped by ancestors.
    Role role = Role::none;
    std::uint16_t flags = 0;
    std::uint32_t actions = 0;
    std::string label;
    std::string value;
    std::string hint;
    std::vector<std::uint32_t> children;
    std::vector<CustomAction> customActions;

    bool operator==(const SemanticsNode &) const = default;
};

// Builds snapshots of a node tree and serializes the difference from the previous one.
//
// Wire format (little-endian, read by Kotlin with a little-endian ByteBuffer):
//
//   u32 magic 0x4D535046 ("FPSM")
//   u16 version (1)
//   u16 flags            bit 0: full snapshot (Kotlin drops everything it had first)
//   u32 rootId           0 if the tree is empty
//   u32 removedCount     then removedCount x u32 id
//   u32 nodeCount        then nodeCount node records, parents before children:
//     u32 id
//     u32 parentId       0 for the root
//     i32 left, top, right, bottom   window px, rounded outward
//     u8  role           Role
//     u8  reserved       0
//     u16 flags          semantics_flags
//     u32 actions        Action bits
//     str label, str value, str hint       str = u32 byte length + UTF-8 bytes
//     u32 childCount     then childCount x u32 child id, in traversal (drawing) order
//     u32 customCount    then customCount x (u32 id, str label)
//
// A changed node is sent whole; unchanged nodes are omitted unless the snapshot is full.
class SemanticsTree {
public:
    // Rebuilds the snapshot from root. Returns whether anything differs from the last one.
    bool update(Node &root, float density);
    // Writes the changes since the last write (or everything if full) and marks them sent.
    void write(ByteWriter &out, bool full = false);

    [[nodiscard]] const std::vector<SemanticsNode> &nodes() const { return nodes_; }
    [[nodiscard]] const SemanticsNode *find(std::uint32_t id) const;
    // The node for an id from the last snapshot, if it's still alive.
    [[nodiscard]] std::shared_ptr<Node> nodeFor(std::uint32_t id) const;

private:
    std::vector<SemanticsNode> nodes_;
    std::unordered_map<std::uint32_t, std::size_t> index_;
    std::unordered_map<std::uint32_t, std::weak_ptr<Node>> live_;
    // What Kotlin has: id -> the record last sent.
    std::unordered_map<std::uint32_t, SemanticsNode> sent_;
};

}  // namespace filepan::ui
