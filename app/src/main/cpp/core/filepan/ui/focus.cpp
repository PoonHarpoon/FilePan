// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/focus.hpp"

#include <vector>

namespace filepan::ui {

namespace {

void collectFocusable(Node &node, std::vector<Node *> &out) {
    if (!node.style().display || node.semantics().hidden) {
        return;
    }
    if (node.focusable()) {
        out.push_back(&node);
    }
    for (const auto &child : node.children()) {
        collectFocusable(*child, out);
    }
}

}  // namespace

void FocusManager::requestFocus(Node *node) {
    auto previous = focused_.lock();
    if (previous.get() == node) {
        // Tapping a focused field asks for the keyboard again: it may have been dismissed, or
        // never shown because the window didn't have focus when the field took it.
        if (node != nullptr && node->textInputClient() != nullptr) {
            textInput_.requestShow();
        }
        return;
    }
    if (previous) {
        previous->focused_.set(false);
        previous->markSemanticsDirty();
        if (previous->textInputClient() != nullptr) {
            textInput_.detach(previous->textInputClient());
        }
    }
    if (node == nullptr || !node->focusable()) {
        focused_.reset();
        return;
    }
    focused_ = node->weak_from_this();
    node->focused_.set(true);
    node->markSemanticsDirty();
    if (TextInputClient *client = node->textInputClient()) {
        textInput_.attach(client);
        textInput_.requestShow();
    }
}

void FocusManager::nodeDetached(Node &subtree) {
    auto current = focused_.lock();
    if (!current) {
        return;
    }
    for (Node *n = current.get(); n != nullptr; n = n->parent()) {
        if (n == &subtree) {
            requestFocus(nullptr);
            return;
        }
    }
}

bool FocusManager::moveFocus(Node &root, bool forward) {
    std::vector<Node *> nodes;
    collectFocusable(root, nodes);
    if (nodes.empty()) {
        return false;
    }
    Node *current = focused();
    std::size_t index = 0;
    bool found = false;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i] == current) {
            index = i;
            found = true;
            break;
        }
    }
    std::size_t next;
    if (!found) {
        next = forward ? 0 : nodes.size() - 1;
    } else if (forward) {
        next = (index + 1) % nodes.size();
    } else {
        next = (index + nodes.size() - 1) % nodes.size();
    }
    requestFocus(nodes[next]);
    nodes[next]->revealInAncestors();
    return true;
}

}  // namespace filepan::ui
