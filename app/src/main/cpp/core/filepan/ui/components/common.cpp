// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/components/common.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "filepan/ui/input.hpp"

namespace filepan::ui::components {

const Theme &theme() { return environment().theme(); }

const Theme &currentTheme() { return environment().theme.peek(); }

bool reducedMotion() { return environment().reducedMotion(); }

gfx::Color mix(gfx::Color a, gfx::Color b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

gfx::Color grayscale(gfx::Color c, float brightness) {
    const float y = (0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b) * brightness;
    return {y, y, y, c.a};
}

gfx::Color ink(const Theme &theme, float alpha) { return theme.colors.ink.withAlpha(alpha); }

gfx::Color textInk(const Theme &theme, float alpha, float sizeSp) {
    return theme.colors.ink.withAlpha(std::max(inkAlpha(alpha, sizeSp), theme.text.minimum));
}

gfx::Color legible(gfx::Color color, gfx::Color background, float minimum) {
    gfx::Color out = color;
    for (int step = 1; step <= 20 && contrastRatio(out, background) < minimum; ++step) {
        // Toward white on a dark background, toward black on a light one.
        const float t = static_cast<float>(step) * 0.05f;
        out = relativeLuminance(background) > 0.4f ? darken(color, t) : lighten(color, t);
    }
    return out;
}

gfx::TextStyle withSize(gfx::TextStyle style, float sizeSp) {
    style.size = sizeSp;
    return style;
}

gfx::TextStyle withWeight(gfx::TextStyle style, float weight) {
    style.weight = weight;
    return style;
}

gfx::TextStyle stampStyle(const Theme &theme) {
    gfx::TextStyle style = theme.type.label;
    style.size = 10;
    style.weight = 700;
    style.letterSpacing = 0.1f;
    style.lineHeight = 1.2f;
    if (theme.style.utilitarian) {
        // Plain small text, not a spaced caps code.
        style.size = 11;
        style.weight = 600;
        style.letterSpacing = 0;
        style.uppercase = false;
    }
    return style;
}

namespace {

thread_local std::function<void(Haptic)> hapticHandler;

bool isActivationKey(const KeyEvent &event) {
    return event.keyCode == keys::kEnter || event.keyCode == keys::kSpace ||
           event.keyCode == keys::kDpadCenter;
}

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

void setHapticHandler(std::function<void(Haptic)> handler) { hapticHandler = std::move(handler); }

void haptic(Haptic kind) {
    if (hapticHandler) {
        hapticHandler(kind);
    }
}

std::shared_ptr<Pressable> makePressable(Node &node, PressableOptions options) {
    auto state = std::make_shared<Pressable>();
    state->tap = std::make_shared<TapRecognizer>();
    auto onTap = std::make_shared<std::function<void()>>(std::move(options.onTap));
    const Haptic kind = options.haptic;
    Signal<bool> pressed = state->pressed;
    state->tap->onTapDown = [pressed](gfx::Vec2) { pressed.set(true); };
    state->tap->onTapCancel = [pressed] { pressed.set(false); };
    state->tap->onTap = [pressed, onTap, kind] {
        pressed.set(false);
        haptic(kind);
        if (*onTap) {
            (*onTap)();
        }
    };
    node.addGestureRecognizer(state->tap);
    if (options.onLongPress) {
        state->longPress = std::make_shared<LongPressRecognizer>();
        auto onLongPress = std::move(options.onLongPress);
        state->longPress->onLongPressStart = [pressed, onLongPress](gfx::Vec2) {
            pressed.set(false);
            haptic(Haptic::longPress);
            onLongPress();
        };
        node.addGestureRecognizer(state->longPress);
    }
    node.setFocusable(options.focusable);
    if (options.focusable) {
        Node *raw = &node;
        node.setKeyHandler([raw, pressed, tap = state->tap.get()](const KeyEvent &event) {
            if (!isActivationKey(event) || !tap->enabled()) {
                return false;
            }
            if (event.action == KeyAction::down) {
                pressed.set(true);
            } else {
                pressed.set(false);
                tap->activate(static_cast<std::uint32_t>(Action::click));
            }
            (void)raw;
            return true;
        });
    }
    Prop<bool> enabled = std::move(options.enabled);
    const bool focusable = options.focusable;
    node.bind([raw = &node, state = state.get(), enabled, focusable] {
        const bool on = enabled.get();
        state->tap->setEnabled(on);
        if (state->longPress) {
            state->longPress->setEnabled(on);
        }
        untrack([&] {
            raw->setFocusable(focusable && on);
            if (!on) {
                state->pressed.set(false);
                raw->clearFocus();
            }
            setSemanticsFlag(*raw, semantics_flags::kDisabled, !on);
        });
    });
    return state;
}

float FingerTracker::update(float deltaPx) {
    if (first_ && deltaPx != 0) {
        first_ = false;
        x_ += std::copysign(GestureSettings{}.touchSlop * environment().density.peek(), deltaPx);
    }
    x_ += deltaPx;
    return x_;
}

void setSemanticsFlag(Node &node, std::uint16_t flag, bool on) {
    const std::uint16_t flags = on ? (node.semantics().flags | flag) : (node.semantics().flags & ~flag);
    if (flags != node.semantics().flags) {
        node.updateSemantics([&](Semantics &s) { s.flags = flags; });
    }
}

bool moveFocusWithin(Node &root, bool forward) {
    std::vector<Node *> nodes;
    collectFocusable(root, nodes);
    if (nodes.empty()) {
        return false;
    }
    NodeHost *host = root.host();
    Node *current = host != nullptr ? host->focusedNode() : nullptr;
    std::size_t next = forward ? 0 : nodes.size() - 1;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i] == current) {
            next = forward ? (i + 1) % nodes.size() : (i + nodes.size() - 1) % nodes.size();
            break;
        }
    }
    nodes[next]->requestFocus();
    nodes[next]->revealInAncestors();
    return true;
}

Node *firstFocusable(Node &root) {
    std::vector<Node *> nodes;
    collectFocusable(root, nodes);
    return nodes.empty() ? nullptr : nodes.front();
}

gfx::Transform nodeToWindow(const Node &node, float density) {
    gfx::Transform transform = node.transformPx(density);
    for (const Node *n = node.parent(); n != nullptr; n = n->parent()) {
        transform = n->transformPx(density) * n->childrenTransformPx(density) * transform;
    }
    return transform;
}

}  // namespace filepan::ui::components
