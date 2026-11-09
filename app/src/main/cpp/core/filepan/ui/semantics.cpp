// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/semantics.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

#include "filepan/ui/gesture.hpp"
#include "filepan/ui/node.hpp"

namespace filepan::ui {

namespace {

constexpr std::uint32_t kMagic = 0x4D535046;  // "FPSM" read as little-endian bytes.
constexpr std::uint16_t kVersion = 1;

gfx::Rect transformedBounds(const gfx::Transform &transform, gfx::Size size) {
    const gfx::Vec2 corners[] = {transform.apply({0, 0}), transform.apply({size.width, 0}),
                                 transform.apply({0, size.height}), transform.apply({size.width, size.height})};
    float left = corners[0].x;
    float top = corners[0].y;
    float right = left;
    float bottom = top;
    for (const auto &c : corners) {
        left = std::min(left, c.x);
        top = std::min(top, c.y);
        right = std::max(right, c.x);
        bottom = std::max(bottom, c.y);
    }
    return gfx::Rect::fromEdges(left, top, right, bottom);
}

bool isSemantic(const Node &node) {
    const Semantics &s = node.semantics();
    if (s.role != Role::none || !s.label.empty() || s.actions != 0 || !s.customActions.empty() ||
        s.mergeDescendants || node.focusable() || !node.semanticsText().empty()) {
        return true;
    }
    for (const auto &recognizer : node.gestureRecognizers()) {
        if (recognizer->semanticsActions() != 0) {
            return true;
        }
    }
    return false;
}

void appendLabel(std::string &label, std::string_view part) {
    if (part.empty()) {
        return;
    }
    if (!label.empty()) {
        label += ", ";
    }
    label += part;
}

// Labels of a merged subtree, in drawing order.
void collectLabels(const Node &node, std::string &label) {
    for (const auto &child : node.children()) {
        if (!child->style().display || child->semantics().hidden) {
            continue;
        }
        const Semantics &s = child->semantics();
        appendLabel(label, s.label.empty() ? child->semanticsText() : std::string_view(s.label));
        appendLabel(label, s.value);
        collectLabels(*child, label);
    }
}

class Builder {
public:
    Builder(float density, std::vector<SemanticsNode> &nodes,
            std::unordered_map<std::uint32_t, std::weak_ptr<Node>> &live)
        : density_(density), nodes_(nodes), live_(live) {}

    void visit(Node &node, const gfx::Transform &parentToWindow, const std::optional<gfx::Rect> &clip,
               std::uint32_t parent, std::vector<std::uint32_t> &siblings, bool root) {
        const Semantics &s = node.semantics();
        if (!node.style().display || s.hidden) {
            return;
        }
        const gfx::Transform toWindow = parentToWindow * node.transformPx(density_);
        const gfx::Size size = node.sizePx(density_);
        gfx::Rect bounds = transformedBounds(toWindow, size);
        if (clip) {
            bounds = bounds.intersect(*clip);
        }
        const bool semantic = root || isSemantic(node);
        if (semantic && !root && bounds.empty()) {
            return;  // Scrolled or clipped out of view.
        }

        std::size_t index = 0;
        std::vector<std::uint32_t> children;
        if (semantic) {
            SemanticsNode out;
            out.id = node.id();
            out.parent = parent;
            out.bounds = bounds;
            out.role = s.role;
            out.flags = s.flags;
            out.actions = s.actions;
            out.label = s.label;
            out.value = s.value;
            out.hint = s.hint;
            out.customActions = s.customActions;
            if (out.label.empty() && !node.semanticsText().empty()) {
                out.label = node.semanticsText();
                if (out.role == Role::none) {
                    out.role = Role::text;
                }
            }
            for (const auto &recognizer : node.gestureRecognizers()) {
                out.actions |= recognizer->semanticsActions();
            }
            if (node.focusable()) {
                out.flags |= semantics_flags::kFocusable;
                out.actions |= static_cast<std::uint32_t>(node.focused() ? Action::clearFocus : Action::focus);
            }
            if (node.focused()) {
                out.flags |= semantics_flags::kFocused;
            }
            if (!out.customActions.empty()) {
                out.actions |= static_cast<std::uint32_t>(Action::custom);
            }
            if (s.mergeDescendants) {
                collectLabels(node, out.label);
            }
            index = nodes_.size();
            nodes_.push_back(std::move(out));
            live_[node.id()] = node.weak_from_this();
            siblings.push_back(node.id());
            if (s.mergeDescendants) {
                return;
            }
        }

        std::optional<gfx::Rect> childClip = clip;
        if (node.clips()) {
            const gfx::Rect own = transformedBounds(toWindow, size);
            childClip = childClip ? childClip->intersect(own) : own;
        }
        const gfx::Transform childBase = toWindow * node.childrenTransformPx(density_);
        for (const auto &child : node.children()) {
            visit(*child, childBase, childClip, semantic ? node.id() : parent, semantic ? children : siblings, false);
        }
        if (semantic) {
            nodes_[index].children = std::move(children);
        }
    }

private:
    float density_;
    std::vector<SemanticsNode> &nodes_;
    std::unordered_map<std::uint32_t, std::weak_ptr<Node>> &live_;
};

void putString(ByteWriter &out, std::string_view text) {
    out.put(static_cast<std::uint32_t>(text.size()));
    out.putBytes(text);
}

}  // namespace

bool SemanticsTree::update(Node &root, float density) {
    std::vector<SemanticsNode> previous = std::move(nodes_);
    nodes_.clear();
    live_.clear();
    std::vector<std::uint32_t> top;
    Builder(density, nodes_, live_).visit(root, {}, std::nullopt, 0, top, true);
    index_.clear();
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        index_.emplace(nodes_[i].id, i);
    }
    return previous != nodes_;
}

const SemanticsNode *SemanticsTree::find(std::uint32_t id) const {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : &nodes_[it->second];
}

std::shared_ptr<Node> SemanticsTree::nodeFor(std::uint32_t id) const {
    auto it = live_.find(id);
    return it == live_.end() ? nullptr : it->second.lock();
}

void SemanticsTree::write(ByteWriter &out, bool full) {
    std::vector<std::uint32_t> removed;
    if (full) {
        sent_.clear();
    } else {
        for (const auto &[id, node] : sent_) {
            if (!index_.contains(id)) {
                removed.push_back(id);
            }
        }
        std::ranges::sort(removed);
    }
    std::vector<const SemanticsNode *> changed;
    for (const auto &node : nodes_) {
        auto it = sent_.find(node.id);
        if (full || it == sent_.end() || !(it->second == node)) {
            changed.push_back(&node);
        }
    }

    out.put(kMagic);
    out.put(kVersion);
    out.put(static_cast<std::uint16_t>(full ? 1 : 0));
    out.put(nodes_.empty() ? std::uint32_t{0} : nodes_.front().id);
    out.put(static_cast<std::uint32_t>(removed.size()));
    for (std::uint32_t id : removed) {
        out.put(id);
    }
    out.put(static_cast<std::uint32_t>(changed.size()));
    for (const SemanticsNode *node : changed) {
        out.put(node->id);
        out.put(node->parent);
        out.put(static_cast<std::int32_t>(std::floor(node->bounds.x)));
        out.put(static_cast<std::int32_t>(std::floor(node->bounds.y)));
        out.put(static_cast<std::int32_t>(std::ceil(node->bounds.right())));
        out.put(static_cast<std::int32_t>(std::ceil(node->bounds.bottom())));
        out.put(static_cast<std::uint8_t>(node->role));
        out.put(std::uint8_t{0});
        out.put(node->flags);
        out.put(node->actions);
        putString(out, node->label);
        putString(out, node->value);
        putString(out, node->hint);
        out.put(static_cast<std::uint32_t>(node->children.size()));
        for (std::uint32_t child : node->children) {
            out.put(child);
        }
        out.put(static_cast<std::uint32_t>(node->customActions.size()));
        for (const auto &action : node->customActions) {
            out.put(action.id);
            putString(out, action.label);
        }
    }

    for (std::uint32_t id : removed) {
        sent_.erase(id);
    }
    for (const SemanticsNode *node : changed) {
        sent_[node->id] = *node;
    }
}

}  // namespace filepan::ui
