// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/components/overlays.hpp"

#include <algorithm>
#include <cmath>

#include "filepan/ui/components/buttons.hpp"
#include "filepan/ui/components/marks.hpp"
#include "filepan/ui/components/recipes.hpp"
#include "filepan/ui/input.hpp"

namespace filepan::ui::components {

namespace {

Style absoluteFill() {
    Style style;
    style.position = Position::absolute;
    style.left = 0;
    style.top = 0;
    style.right = 0;
    style.bottom = 0;
    return style;
}

gfx::Size windowDp() {
    const float density = environment().density.peek();
    const gfx::Size window = environment().windowSize.peek();
    return {window.width / density, window.height / density};
}

// A scrim that dismisses on tap; hidden from semantics (the overlay has a dismiss action).
std::shared_ptr<Box> makeScrim(std::function<void()> onTap) {
    auto scrim = Box::create(absoluteFill());
    scrim->setHitTestBehavior(HitTestBehavior::opaque);
    scrim->updateSemantics([](Semantics &s) { s.hidden = true; });
    if (onTap) {
        auto tap = std::make_shared<TapRecognizer>();
        tap->onTap = std::move(onTap);
        scrim->addGestureRecognizer(tap);
    }
    scrim->setPaintHook([](PaintContext &context) {
        context.list.fill(context.bounds(), gfx::Color{0, 0, 0, currentTheme().colors.scrim});
    });
    return scrim;
}

// Keeps Tab inside root and sends Escape to dismiss.
Node::KeyHandler trapFocus(Node *root, std::function<bool()> back) {
    return [root, back = std::move(back)](const KeyEvent &event) {
        if (event.keyCode == keys::kTab) {
            if (event.action == KeyAction::down) {
                moveFocusWithin(*root, (event.modifiers & modifiers::kShift) == 0);
            }
            return true;
        }
        if (event.keyCode == keys::kEscape) {
            return event.action != KeyAction::down || back();
        }
        return false;
    };
}

void dialogSemantics(Node &node, const std::string &title, std::function<void()> dismiss) {
    node.updateSemantics([&](Semantics &s) {
        s.role = Role::dialog;
        s.label = title;
        s.actions = static_cast<std::uint32_t>(Action::dismiss);
        s.onAction = [dismiss = std::move(dismiss)](Action action, const ActionArgs &) {
            if (action == Action::dismiss) {
                dismiss();
                return true;
            }
            return false;
        };
    });
}

std::shared_ptr<Text> titleText(const std::string &title) {
    auto text = Text::create(title, currentTheme().type.title);
    text->updateSemantics([](Semantics &s) { s.role = Role::heading; });
    text->bind([t = text.get()] {
        const Theme &th = theme();
        untrack([&] {
            t->setTextStyle(th.type.title);
            t->setColor(ink(th, 1));
        });
    });
    return text;
}

}  // namespace

// --- BottomSheet ---

std::shared_ptr<BottomSheet> BottomSheet::create(BottomSheetProps props) {
    auto node = std::make_shared<BottomSheet>();
    BottomSheet *raw = node.get();
    node->setDebugName("BottomSheet");
    node->setStyle(absoluteFill());
    node->onDismiss_ = std::move(props.onDismiss);
    node->scrim_ = makeScrim([raw] { raw->dismiss(); });
    node->appendChild(node->scrim_);

    const Theme &initial = currentTheme();
    const float r = initial.radii.sheet;
    // Utilitarian: the sides off screen, the header ruled off.
    const bool dense = initial.style.utilitarian;
    node->panel_ = Glass::create({.padding = {0, 0, 0, 16},
                                  .position = Position::absolute,
                                  .left = dense ? -1.0f : 0.0f,
                                  .right = dense ? -1.0f : 0.0f,
                                  .bottom = 0},
                                 {.radii = {r, r, 0, 0}});
    auto handle = Box::create({.alignSelf = Align::center, .margin = {0, 10, 0, 2}, .width = 36, .height = 4},
                              {.background = gfx::Paint{initial.colors.ink.withAlpha(0.32f)},
                               .radii = gfx::CornerRadii::uniform(2)});
    handle->updateSemantics([](Semantics &s) { s.hidden = true; });
    node->panel_->appendChild(handle);
    auto header = Box::create({.direction = Direction::row, .alignItems = Align::center,
                               .padding = Edges::symmetric(dense ? 17.0f : 12.0f, 0),
                               .margin = dense ? Edges{0, 0, 0, 8} : Edges{12, 2, 12, 6}, .height = dense ? 44.0f : 52.0f});
    if (dense) {
        header->setPaintHook([](PaintContext &context) {
            const Theme &th = currentTheme();
            const gfx::Rect b = context.bounds();
            const float hair = std::max(1.0f, std::round(context.density * 0.5f));
            context.list.fill(gfx::Rect{b.x, b.bottom() - hair, b.width, hair}, th.colors.ink.withAlpha(th.colors.divider));
        });
    }
    auto title = titleText(props.title);
    if (props.headerTrailing) {
        title->updateStyle([](Style &s) {
            s.grow = 1;
            s.shrink = 1;
            s.basis = 0;
        });
    }
    header->appendChild(title);
    if (props.headerTrailing) {
        header->appendChild(props.headerTrailing);
    }
    if (props.cornerTicks) {
        header->appendChild(CornerTicks(0, 8));
    }
    node->panel_->appendChild(header);
    if (props.content) {
        node->panel_->appendChild(props.content);
    }
    dialogSemantics(*node->panel_, props.title, [raw] { raw->dismiss(); });
    node->panel_->setKeyHandler(trapFocus(node->panel_.get(), [raw] { return raw->handleBack(); }));
    node->appendChild(node->panel_);

    auto drag = std::make_shared<DragRecognizer>(DragAxis::vertical);
    drag->onUpdate = [raw](gfx::Vec2 delta) {
        const float density = environment().density.peek();
        const float next = raw->offset_.peek() + delta.y / density;
        // Resists being pulled up past its resting place.
        raw->offset_.snapTo(next < 0 ? next * 0.25f : next);
    };
    drag->onEnd = [raw](gfx::Vec2 velocity) { raw->release(velocity.y / environment().density.peek()); };
    drag->onCancel = [raw] { raw->release(0); };
    node->panel_->addGestureRecognizer(drag);

    node->bind([raw] {
        const float offset = raw->offset_();
        untrack([&] {
            raw->panel_->setTransform(gfx::Transform::translation({0, offset}));
            const float height = std::max(1.0f, raw->panel_->frame().height);
            raw->scrim_->setOpacity(raw->open_ ? std::clamp(1 - offset / height, 0.0f, 1.0f) : 0.0f);
        });
    });
    // The keyboard pushes the panel up to sit on it, covering the navigation bar.
    node->bind([raw] {
        const float density = environment().density();
        const float ime = environment().imeInsets().bottom / density;
        const float inset = ime > 0 ? 0.0f : environment().safeArea().bottom / density;
        untrack([&] {
            raw->panel_->updateStyle([&](Style &s) {
                s.bottom = ime;
                s.padding.bottom = inset + 16;
            });
        });
    });
    // At most 90% of the window above the keyboard, which a rotation can halve or double while the
    // sheet is up; a closed sheet waits just below the window's new bottom edge.
    node->bind([raw] {
        const float density = environment().density();
        const float height = environment().windowSize().height / density;
        const float above = height - environment().imeInsets().bottom / density;
        untrack([&] {
            raw->panel_->updateStyle([&](Style &s) { s.maxHeight = above * 0.9f; });
            if (!raw->open_ && !raw->offset_.animating()) {
                raw->offset_.snapTo(height);
            }
        });
    });
    node->setHitTestBehavior(HitTestBehavior::none);
    node->updateSemantics([](Semantics &s) { s.hidden = true; });
    return node;
}

void BottomSheet::open(bool animate) {
    open_ = true;
    setHitTestBehavior(HitTestBehavior::deferToChild);
    updateSemantics([](Semantics &s) { s.hidden = false; });
    if (animate) {
        offset_.snapTo(std::max(panel_->frame().height, windowDp().height * 0.5f));
        const Theme &theme = currentTheme();
        offset_.animateTo(0, Spring{theme.motion.springStiffness, 1});
    } else {
        offset_.snapTo(0);
    }
    scrim_->setOpacity(1);
    if (Node *first = firstFocusable(*panel_); first != nullptr && host() != nullptr && host()->focusedNode() != nullptr) {
        first->requestFocus();
    }
}

void BottomSheet::dismiss() {
    if (!open_) {
        return;
    }
    open_ = false;
    setHitTestBehavior(HitTestBehavior::none);
    clearFocus();
    if (NodeHost *h = host(); h != nullptr && h->focusedNode() != nullptr) {
        for (Node *n = h->focusedNode(); n != nullptr; n = n->parent()) {
            if (n == this) {
                h->requestFocus(nullptr);
                break;
            }
        }
    }
    const float height = std::max(panel_->frame().height, 1.0f);
    const auto done = [this] {
        updateSemantics([](Semantics &s) { s.hidden = true; });
        if (onDismiss_) {
            onDismiss_();
        }
    };
    const Theme &theme = currentTheme();
    offset_.animateTo(height + 24, Spring{theme.motion.springStiffness, 1}, done);
}

bool BottomSheet::handleBack() {
    if (!open_) {
        return false;
    }
    dismiss();
    return true;
}

void BottomSheet::release(float velocityDp) {
    if (!open_) {
        return;
    }
    const float height = std::max(panel_->frame().height, 1.0f);
    if (offset_.peek() > height * kDismissFraction || velocityDp > kDismissVelocity) {
        dismiss();
    } else {
        const Theme &theme = currentTheme();
        offset_.animateTo(0, Spring{theme.motion.springStiffness, theme.motion.springDamping});
    }
}

// --- Dialog ---

std::shared_ptr<Dialog> Dialog::create(DialogProps props) {
    auto node = std::make_shared<Dialog>();
    Dialog *raw = node.get();
    node->setDebugName("Dialog");
    node->setStyle([] {
        Style s = absoluteFill();
        s.justify = Justify::center;
        s.alignItems = Align::center;
        s.padding = Edges::all(24);
        return s;
    }());
    node->onDismiss_ = std::move(props.onDismiss);
    node->dismissible_ = props.dismissible;
    auto scrim = makeScrim(props.dismissible ? std::function<void()>([raw] { raw->dismiss(); }) : nullptr);
    node->appendChild(scrim);
    node->card_ = Glass::create({.gap = 12, .padding = {24, 24, 16, 16}, .width = Length::percent(100), .maxWidth = 360},
                                {.radii = gfx::CornerRadii::uniform(currentTheme().radii.sheet)});
    node->card_->appendChild(titleText(props.title));
    // The message and content scroll once the keyboard leaves too little room, keeping the title
    // and actions in view.
    if (!props.message.empty() || props.content) {
        auto body = Box::create({.gap = 12});
        if (!props.message.empty()) {
            auto message = Text::create(props.message, currentTheme().type.body);
            message->bind([m = message.get()] {
                const Theme &th = theme();
                untrack([&] { m->setColor(ink(th, th.text.body)); });
            });
            body->appendChild(message);
        }
        if (props.content) {
            body->appendChild(props.content);
        }
        auto scroll = ScrollNode::create();
        scroll->setStyle({.shrink = 1});
        scroll->setFitContent(true);
        scroll->appendChild(body);
        node->card_->appendChild(scroll);
    }
    if (!props.actions.empty()) {
        auto actions = Box::create({.direction = Direction::row, .justify = Justify::end, .alignItems = Align::center,
                                    .gap = 8, .margin = {0, 8, 0, 0}});
        for (auto &action : props.actions) {
            actions->appendChild(action);
        }
        node->card_->appendChild(actions);
    }
    dialogSemantics(*node->card_, props.title, [raw] { raw->dismiss(); });
    node->card_->setKeyHandler(trapFocus(node->card_.get(), [raw] { return raw->handleBack(); }));
    node->appendChild(node->card_);
    node->bind([raw, scrim = scrim.get()] {
        const float t = raw->shown_();
        untrack([&] {
            scrim->setOpacity(t);
            raw->card_->setOpacity(t);
            const float scale = environment().reducedMotion.peek() ? 1.0f : 0.94f + 0.06f * t;
            raw->card_->setTransform(gfx::Transform::scale(scale, scale));
        });
    });
    // The keyboard lifts the card to center in the room above it, and caps it to that room.
    node->bind([raw] {
        const float density = environment().density();
        const float height = environment().windowSize().height / density;
        const float bottom = std::max(24.0f, environment().imeInsets().bottom / density + 16);
        untrack([&] {
            raw->updateStyle([&](Style &s) { s.padding.bottom = bottom; });
            raw->card_->updateStyle([&](Style &s) { s.maxHeight = std::max(0.0f, height - 24 - bottom); });
        });
    });
    node->setHitTestBehavior(HitTestBehavior::none);
    node->updateSemantics([](Semantics &s) { s.hidden = true; });
    return node;
}

void Dialog::open(bool animate) {
    open_ = true;
    setHitTestBehavior(HitTestBehavior::deferToChild);
    updateSemantics([](Semantics &s) { s.hidden = false; });
    const Theme &theme = currentTheme();
    if (animate) {
        shown_.animateTo(1, environment().reducedMotion.peek()
                                    ? AnimationSpec{Tween{.durationMs = theme.motion.crossfadeMs, .easing = easing::kLinear, .crossfade = true}}
                                    : AnimationSpec{Spring{theme.motion.springStiffness, 1}});
    } else {
        shown_.snapTo(1);
    }
}

void Dialog::dismiss() {
    if (!open_) {
        return;
    }
    open_ = false;
    setHitTestBehavior(HitTestBehavior::none);
    const Theme &theme = currentTheme();
    shown_.animateTo(0, Tween{.durationMs = theme.motion.crossfadeMs, .easing = easing::kLinear, .crossfade = true}, [this] {
        updateSemantics([](Semantics &s) { s.hidden = true; });
        if (onDismiss_) {
            onDismiss_();
        }
    });
}

bool Dialog::handleBack() {
    if (!open_ || !dismissible_) {
        return open_;
    }
    dismiss();
    return true;
}

// --- Menu ---

MenuPlacement placeMenu(gfx::Rect anchor, float width, float height, float minHeight) {
    constexpr float kMargin = 8;
    constexpr float kGap = 4;  // Between the anchor and the card.
    const float density = environment().density.peek();
    const Insets safe = environment().safeArea.peek();
    const gfx::Size window = windowDp();
    const float areaLeft = safe.left / density + kMargin;
    const float areaRight = window.width - safe.right / density - kMargin;
    const float areaTop = safe.top / density + kMargin;
    const float areaBottom = window.height - safe.bottom / density - kMargin;

    MenuPlacement place;
    place.left = std::clamp(anchor.right() - width, areaLeft, std::max(areaLeft, areaRight - width));
    const float below = areaBottom - (anchor.bottom() + kGap);
    const float above = anchor.y - kGap - areaTop;
    if (height <= below || (height > above && below >= above)) {
        place.maxHeight = std::max(minHeight, std::min(height, below));
        place.top = anchor.bottom() + kGap;
    } else {
        place.maxHeight = std::max(minHeight, std::min(height, above));
        place.top = std::max(areaTop, anchor.y - kGap - place.maxHeight);
    }
    return place;
}

std::shared_ptr<Menu> Menu::create(MenuProps props) {
    auto node = std::make_shared<Menu>();
    Menu *raw = node.get();
    node->setDebugName("Menu");
    node->setStyle(absoluteFill());
    node->onDismiss_ = std::move(props.onDismiss);
    auto scrim = Box::create(absoluteFill());
    scrim->setHitTestBehavior(HitTestBehavior::opaque);
    scrim->updateSemantics([](Semantics &s) { s.hidden = true; });
    auto outside = std::make_shared<TapRecognizer>();
    outside->onTap = [raw] { raw->dismiss(); };
    scrim->addGestureRecognizer(outside);
    node->appendChild(scrim);

    const float rowHeight = currentTheme().spacing.listRowCompact;
    const float height = static_cast<float>(props.items.size()) * rowHeight + 12 + (props.header ? props.headerHeight : 0);
    const MenuPlacement place = placeMenu(props.anchor, props.width, height, rowHeight + 12);
    node->card_ = Glass::create({.padding = Edges::symmetric(0, 6),
                                 .width = props.width,
                                 .maxHeight = place.maxHeight,
                                 .position = Position::absolute,
                                 .left = place.left,
                                 .top = place.top},
                                {.radii = gfx::CornerRadii::uniform(20)});
    node->card_->updateSemantics([](Semantics &s) { s.role = Role::menu; });
    // Everything scrolls when the card is capped, as a sheet's body does; keyboard focus moving
    // through it scrolls the focused item into view.
    node->scroll_ = ScrollNode::create();
    node->scroll_->setDebugName("MenuScroll");
    node->scroll_->setStyle({.shrink = 1});
    node->scroll_->setFitContent(true);
    node->card_->appendChild(node->scroll_);
    if (props.header) {
        props.header->updateStyle([&](Style &s) { s.height = props.headerHeight; });
        node->scroll_->appendChild(props.header);
    }
    const Theme &initial = currentTheme();
    for (auto &item : props.items) {
        auto row = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 14,
                                .padding = Edges::symmetric(16, 0), .height = rowHeight});
        std::shared_ptr<Icon> icon;
        if (!item.icon.empty()) {
            icon = Icon::create(item.icon, 20, {1, 1, 1, 1}, item.filled);
            row->appendChild(icon);
        }
        auto label = Text::create(item.label, initial.type.body);
        label->setMaxLines(1);
        label->setStyle({.grow = 1, .shrink = 1, .basis = 0});
        label->updateSemantics([](Semantics &s) { s.hidden = true; });
        row->appendChild(label);
        std::shared_ptr<Text> trailing;
        if (!item.trailing.empty()) {
            gfx::TextStyle mono = withSize(initial.type.label, 12);
            mono.uppercase = false;
            mono.letterSpacing = 0;
            trailing = Text::create(item.trailing, mono);
            trailing->setMaxLines(1);
            trailing->updateSemantics([](Semantics &s) { s.hidden = true; });
            row->appendChild(trailing);
        }
        auto onSelect = item.onSelect;
        auto pressable = makePressable(*row, {.onTap = [raw, onSelect] {
            if (onSelect) {
                onSelect();
            }
            raw->dismiss();
        }});
        row->updateSemantics([&](Semantics &s) {
            s.role = Role::menuItem;
            s.label = item.trailing.empty() ? item.label : item.label + ", " + item.trailing;
        });
        row->setPaintHook([pressable, r = row.get()](PaintContext &context) {
            const gfx::RoundedRect shape{{context.px(6), 0, context.size.width - context.px(12), context.size.height},
                                         gfx::CornerRadii::uniform(context.px(currentTheme().corner(14)))};
            if (pressable->pressed.peek()) {
                context.list.fill(shape, currentTheme().colors.ink.withAlpha(0.10f));
            }
            if (r->focused()) {
                context.list.stroke(shape, context.px(2), currentTheme().colors.accent);
            }
        });
        const bool destructive = item.destructive;
        row->bind([r = row.get(), l = label.get(), i = icon.get(), t = trailing.get(), pressable, destructive] {
            const Theme &th = theme();
            (void)pressable->pressed();
            (void)r->trackFocused();
            untrack([&] {
                const gfx::Color color = destructive ? legible(th.colors.destructive, th.colors.ground) : ink(th, 1);
                l->setColor(color);
                if (i != nullptr) {
                    i->setColor(destructive ? color : ink(th, th.text.body));
                }
                if (t != nullptr) {
                    t->setColor(ink(th, th.text.meta));
                }
                r->markPaintDirty();
            });
        });
        node->scroll_->appendChild(row);
    }
    node->card_->setKeyHandler(trapFocus(node->card_.get(), [raw] { return raw->handleBack(); }));
    node->appendChild(node->card_);
    // It's placed by an anchor that a rotation or resize moves: it closes then, as the
    // platform's own popups do, rather than float where the anchor used to be.
    node->bind([raw, opened = environment().windowSize.peek()] {
        const gfx::Size window = environment().windowSize();
        if (window != opened) {
            untrack([&] { raw->dismiss(); });
        }
    });
    return node;
}

void Menu::dismiss() {
    if (!open_) {
        return;
    }
    open_ = false;
    setHitTestBehavior(HitTestBehavior::none);
    updateSemantics([](Semantics &s) { s.hidden = true; });
    setOpacity(0);
    if (onDismiss_) {
        onDismiss_();
    }
}

bool Menu::handleBack() {
    if (!open_) {
        return false;
    }
    dismiss();
    return true;
}

// --- SnackbarHost ---

std::shared_ptr<SnackbarHost> SnackbarHost::create(UiLoop *loop, float bottom) {
    auto node = std::make_shared<SnackbarHost>();
    SnackbarHost *raw = node.get();
    node->setDebugName("SnackbarHost");
    node->loop_ = loop;
    node->setStyle({.position = Position::absolute, .left = 16, .right = 16});
    node->bar_ = Glass::create({.direction = Direction::row, .alignItems = Align::center, .gap = 8,
                                .padding = {20, 0, 8, 0}, .minHeight = 52},
                               {.radii = gfx::CornerRadii::uniform(18)});
    node->text_ = Text::create({}, currentTheme().type.body);
    node->text_->setMaxLines(2);
    node->text_->setStyle({.margin = Edges::symmetric(0, 14), .grow = 1, .shrink = 1});
    node->bar_->appendChild(node->text_);
    node->actionSlot_ = Box::create({.direction = Direction::row});
    node->bar_->appendChild(node->actionSlot_);
    node->appendChild(node->bar_);
    node->updateSemantics([](Semantics &s) { s.flags = semantics_flags::kLiveRegion; });
    node->bind([raw, bottom] {
        const float inset = environment().safeArea().bottom / environment().density();
        untrack([&] { raw->updateStyle([&](Style &s) { s.bottom = inset + bottom; }); });
    });
    node->bind([raw] {
        const Theme &th = theme();
        untrack([&] { raw->text_->setColor(ink(th, 1)); });
    });
    node->bind([raw] {
        const float t = raw->shown_();
        untrack([&] {
            raw->setOpacity(std::clamp(t, 0.0f, 1.0f));
            const bool moving = !environment().reducedMotion.peek();
            raw->setTransform(gfx::Transform::translation({0, moving ? 16 * (1 - t) : 0}));
        });
    });
    node->setHitTestBehavior(HitTestBehavior::none);
    return node;
}

SnackbarHost::~SnackbarHost() {
    if (timer_ && loop_ != nullptr) {
        loop_->cancel(*timer_);
    }
}

void SnackbarHost::show(SnackbarMessage message) {
    // The newest message replaces whatever is showing or waiting: an old one is out of date.
    queue_.clear();
    queue_.push_back(std::move(message));
    if (current_) {
        dismissCurrent();
    } else {
        showNext();
    }
}

void SnackbarHost::setHeld(bool held) {
    if (held == held_) {
        return;
    }
    held_ = held;
    if (held && current_) {
        queue_.push_front(*current_);
        dismissCurrent();
    } else if (!held && !current_) {
        showNext();
    }
}

void SnackbarHost::showNext() {
    // Already showing one (shown while the last one faded out), or held.
    if (current_ || held_ || queue_.empty()) {
        return;
    }
    current_ = std::move(queue_.front());
    queue_.pop_front();
    const std::uint64_t generation = ++generation_;
    text_->setText(current_->text);
    actionSlot_->clearChildren();
    if (!current_->action.empty()) {
        actionSlot_->appendChild(Button::create({.label = current_->action,
                                                 .variant = ButtonVariant::text,
                                                 .onTap = [this] {
                                                     if (current_ && current_->onAction) {
                                                         current_->onAction();
                                                     }
                                                     dismissCurrent();
                                                 }}));
    }
    updateSemantics([&](Semantics &s) { s.label = current_->text; });
    setHitTestBehavior(HitTestBehavior::deferToChild);
    const Theme &theme = currentTheme();
    shown_.animateTo(1, environment().reducedMotion.peek()
                                ? AnimationSpec{Tween{.durationMs = theme.motion.crossfadeMs, .easing = easing::kLinear, .crossfade = true}}
                                : AnimationSpec{Spring{theme.motion.springStiffness, 1}});
    const std::int64_t durationMs = current_->durationMs > 0 ? current_->durationMs
                                    : current_->action.empty() ? kToastMs
                                                               : kSnackbarActionMs;
    if (loop_ != nullptr) {
        std::weak_ptr<Node> weak = weak_from_this();
        timer_ = loop_->postDelayed(
                [weak, generation] {
                    if (auto self = weak.lock()) {
                        auto *host = static_cast<SnackbarHost *>(self.get());
                        if (host->generation_ == generation) {
                            host->timer_.reset();
                            host->dismissCurrent();
                        }
                    }
                },
                durationMs * 1'000'000);
    }
}

void SnackbarHost::clear() {
    queue_.clear();
    dismissCurrent();
}

void SnackbarHost::dismissCurrent() {
    if (!current_) {
        return;
    }
    if (timer_ && loop_ != nullptr) {
        loop_->cancel(*timer_);
        timer_.reset();
    }
    current_.reset();
    ++generation_;
    setHitTestBehavior(HitTestBehavior::none);
    updateSemantics([](Semantics &s) { s.label.clear(); });
    const Theme &theme = currentTheme();
    shown_.animateTo(0, Tween{.durationMs = theme.motion.crossfadeMs, .easing = easing::kLinear, .crossfade = true},
                     [this] { showNext(); });
}

}  // namespace filepan::ui::components
