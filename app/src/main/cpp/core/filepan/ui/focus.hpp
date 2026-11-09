// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <memory>

#include "filepan/ui/node.hpp"
#include "filepan/ui/text_input.hpp"

// Keyboard focus. One node at a time has it; Node::focused() is a signal, so a component can
// restyle on focus with a bind. Focusing a node that has a TextInputClient attaches it to the
// TextInputService and asks for the keyboard; losing focus detaches it.
namespace filepan::ui {

class FocusManager {
public:
    explicit FocusManager(TextInputService &textInput) : textInput_(textInput) {}

    [[nodiscard]] Node *focused() const { return focused_.lock().get(); }
    // nullptr clears focus.
    void requestFocus(Node *node);
    // A subtree left the tree: drop focus if it was inside.
    void nodeDetached(Node &subtree);
    // Tab / Shift+Tab: the next focusable node in tree order, wrapping around.
    bool moveFocus(Node &root, bool forward);

private:
    TextInputService &textInput_;
    std::weak_ptr<Node> focused_;
};

}  // namespace filepan::ui
