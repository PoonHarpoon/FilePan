// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/components/navigation.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

#include "filepan/ui/components/buttons.hpp"
#include "filepan/ui/components/marks.hpp"
#include "filepan/ui/components/recipes.hpp"
#include "filepan/ui/layout.hpp"

namespace filepan::ui::components {

namespace {

constexpr float kRow = 48;

Style absoluteFill() {
    Style style;
    style.position = Position::absolute;
    style.left = 0;
    style.top = 0;
    style.right = 0;
    style.bottom = 0;
    return style;
}

// Out of sight to the left (the path line clips): segments cut behind the ellipsis.
constexpr float kAway = -100000;

}  // namespace

// --- Breadcrumb ---

std::shared_ptr<Breadcrumb> Breadcrumb::create(BreadcrumbProps props) {
    auto node = std::make_shared<Breadcrumb>();
    Breadcrumb *raw = node.get();
    node->setDebugName("Breadcrumb");
    node->setStyle({.height = kRow});
    node->setClip(true);
    const gfx::TextStyle body = currentTheme().type.body;
    node->ellipsis_ = Text::create("…", body);
    node->ellipsis_->setStyle({.margin = {0, 0, 6, 0}});
    node->ellipsis_->updateSemantics([](Semantics &sem) { sem.hidden = true; });
    node->appendChild(node->ellipsis_);
    auto onSelect = std::make_shared<std::function<void(std::size_t)>>(std::move(props.onSelect));
    Prop<std::vector<std::string>> segments = props.segments;
    node->bind([raw, segments, onSelect, body] {
        const std::vector<std::string> list = segments.get();
        untrack([&] {
            while (raw->children().size() > 1) {
                raw->removeChild(raw->children().back().get());
            }
            raw->segments_.clear();
            for (std::size_t i = 0; i < list.size(); ++i) {
                const bool last = i + 1 == list.size();
                if (i > 0) {
                    auto slash = Text::create("/", body);
                    slash->setStyle({.margin = {6, 0, 6, 0}});
                    slash->setMaxLines(1);
                    slash->updateSemantics([](Semantics &sem) { sem.hidden = true; });
                    slash->bind([sl = slash.get()] {
                        const Theme &th = theme();
                        untrack([&] { sl->setColor(ink(th, th.text.meta * 0.45f)); });
                    });
                    raw->appendChild(slash);
                }
                auto segment = Box::create({.justify = Justify::center, .height = kRow});
                auto text = Text::create(list[i], last ? withWeight(body, 650) : body);
                text->setMaxLines(1);
                text->setOverflow(gfx::TextOverflow::ellipsisEnd);
                text->updateSemantics([](Semantics &sem) { sem.hidden = true; });
                segment->appendChild(text);
                auto pressable = makePressable(*segment, {.onTap =
                                                                  [onSelect, i] {
                                                                      if (*onSelect) {
                                                                          (*onSelect)(i);
                                                                      }
                                                                  },
                                                          .haptic = Haptic::tick});
                segment->setPaintHook([pressable](PaintContext &context) {
                    if (pressable->pressed.peek()) {
                        const gfx::Rect b = context.bounds();
                        context.list.fill(gfx::RoundedRect{{-context.px(6), b.height / 2 - context.px(16),
                                                            b.width + context.px(12), context.px(32)},
                                                           gfx::CornerRadii::uniform(context.px(currentTheme().corner(10)))},
                                          currentTheme().colors.ink.withAlpha(0.10f));
                    }
                });
                segment->bind([seg = segment.get(), t = text.get(), pressable, last] {
                    const Theme &th = theme();
                    (void)pressable->pressed();
                    untrack([&] {
                        t->setColor(last ? ink(th, th.text.primary) : ink(th, th.text.meta));
                        seg->markPaintDirty();
                    });
                });
                segment->updateSemantics([&](Semantics &sem) {
                    sem.role = Role::button;
                    sem.label = list[i];
                    if (last) {
                        sem.flags |= semantics_flags::kSelected;
                    }
                });
                raw->segments_.push_back(segment.get());
                raw->appendChild(segment);
            }
            raw->markLayoutDirty();
        });
    });
    node->bind([raw] {
        const Theme &th = theme();
        untrack([&] { raw->ellipsis_->setColor(ink(th, th.text.meta)); });
    });
    return node;
}

void Breadcrumb::layoutCustom(LayoutEngine &engine, gfx::Size size) {
    const float h = size.height;
    const auto &all = children();
    const std::size_t count = all.size() - 1;  // After the ellipsis.
    std::vector<float> widths(count);
    float total = 0;
    for (std::size_t i = 0; i < count; ++i) {
        Node &child = *all[i + 1];
        const Edges &m = child.style().margin;
        widths[i] = engine.compute(child, std::nullopt, std::nullopt, kInfinity, h, false).width + m.horizontal();
        total += widths[i];
    }
    const auto put = [&](Node &child, float x, std::optional<float> width) {
        const Edges &m = child.style().margin;
        const float w = width ? std::max(0.0f, *width - m.horizontal()) : 0;
        const gfx::Size s = engine.compute(child, width ? std::optional<float>(w) : std::nullopt, std::nullopt,
                                           width ? w : kInfinity, h, true);
        LayoutEngine::place(child, {x + m.left, (h - s.height) / 2, s.width, s.height});
        return x + m.horizontal() + s.width;
    };
    // Everything fits: left to right, no ellipsis.
    std::size_t first = 0;
    float x = 0;
    if (total > size.width && count > 0) {
        // Keep the longest tail that fits after the ellipsis.
        const Edges &em = ellipsis_->style().margin;
        const float ellipsis = engine.compute(*ellipsis_, std::nullopt, std::nullopt, kInfinity, h, false).width + em.horizontal();
        float room = size.width - ellipsis;
        first = count;
        while (first > 0 && widths[first - 1] <= room) {
            room -= widths[first - 1];
            --first;
        }
        // A separator never leads the tail.
        if (first < count && first % 2 == 1) {
            ++first;
        }
        if (first == count) {
            first = count - 1;
        }
        x = put(*ellipsis_, 0, std::nullopt);
    } else {
        put(*ellipsis_, kAway, std::nullopt);
    }
    hidden_ = (first + 1) / 2;
    for (std::size_t i = 0; i < count; ++i) {
        Node &child = *all[i + 1];
        if (i < first) {
            put(child, kAway, std::nullopt);
        } else if (i + 1 == count && x + widths[i] > size.width) {
            // The current folder alone is too long: its own end is cut.
            put(child, x, std::max(0.0f, size.width - x));
        } else {
            x = put(child, x, std::nullopt);
        }
    }
}

// --- TopBar ---

std::shared_ptr<TopBar> TopBar::create(TopBarProps props) {
    auto node = std::make_shared<TopBar>();
    TopBar *raw = node.get();
    node->setDebugName("TopBar");
    node->setStyle({.padding = Edges::symmetric(8, 0), .position = Position::absolute, .left = 0, .top = 0, .right = 0});
    node->glass_ = Glass::create({.height = kRow}, {.radii = gfx::CornerRadii::uniform(kRow / 2), .clip = true});
    node->appendChild(node->glass_);
    if (props.content) {
        props.content->updateStyle([](Style &s) { s.height = kRow; });
        node->glass_->appendChild(props.content);
    }
    node->bind([raw] {
        const float density = environment().density();
        const float top = environment().safeArea().top / density;
        const bool edge = theme().style.utilitarian;
        untrack([&] {
            // Utilitarian: fixed to the top edge, under the status bar too, its sides off screen.
            raw->updateStyle([&](Style &s) {
                s.padding = edge ? Edges{} : Edges{8, top + 6, 8, 0};
            });
            raw->glass_->updateStyle([&](Style &s) {
                s.padding = {0, edge ? top : 0, 0, 0};
                s.height = (edge ? top : 0) + kRow;
                s.margin = edge ? Edges{-1, -1, -1, 0} : Edges{};
            });
            raw->glass_->setProps({.radii = gfx::CornerRadii::uniform(edge ? 0 : kRow / 2), .clip = true});
            if (edge) {
                raw->show(true);
            }
        });
    });
    node->bind([raw] {
        const float f = std::clamp(raw->reveal_(), 0.0f, 1.0f);
        untrack([&] {
            raw->setTransform(gfx::Transform::translation({0, -(raw->height() + 8) * (1 - f)}));
            raw->setOpacity(f);
            raw->setHitTestBehavior(f < 0.5f ? HitTestBehavior::none : HitTestBehavior::deferToChild);
            raw->glass_->updateSemantics([&](Semantics &s) { s.hidden = f < 0.5f; });
        });
    });
    if (props.scrollOffset) {
        Signal<float> offset = *props.scrollOffset;
        node->bind([raw, offset] {
            const float value = offset();
            untrack([&] {
                const float delta = value - raw->lastOffset_;
                raw->lastOffset_ = value;
                // Direction changes reset the travel, so a small wobble doesn't toggle it.
                if ((delta > 0) != (raw->travel_ > 0)) {
                    raw->travel_ = 0;
                }
                raw->travel_ += delta;
                if (value <= 0 || currentTheme().style.utilitarian) {
                    raw->show(true);
                } else if (raw->travel_ > 24 && value > 60) {
                    raw->show(false);
                } else if (raw->travel_ < -24) {
                    raw->show(true);
                }
            });
        });
    }
    return node;
}

float TopBar::height() const {
    const float density = environment().density.peek();
    return environment().safeArea.peek().top / density + (currentTheme().style.utilitarian ? 0 : 6) + kRow;
}

void TopBar::show(bool show) {
    const float target = show ? 1.0f : 0.0f;
    if (reveal_.target() == target) {
        return;
    }
    const Theme &theme = currentTheme();
    if (environment().reducedMotion.peek()) {
        reveal_.animateTo(target, Tween{.durationMs = theme.motion.crossfadeMs, .easing = easing::kLinear, .crossfade = true});
    } else {
        reveal_.animateTo(target, Spring{theme.motion.springStiffness, 1});
    }
}

// --- BottomDock ---

std::shared_ptr<BottomDock> BottomDock::create(BottomDockProps props) {
    auto node = std::make_shared<BottomDock>();
    BottomDock *raw = node.get();
    node->setDebugName("BottomDock");
    const Theme &initial = currentTheme();
    node->setStyle({.position = Position::absolute, .left = initial.spacing.dockInset, .right = initial.spacing.dockInset});
    auto glass = Glass::create({.height = initial.spacing.dock}, {.radii = gfx::CornerRadii::uniform(1e6f), .clip = true});
    node->appendChild(glass);

    const auto actionButton = [](const DockAction &action) {
        auto button = IconButton::create({.icon = action.icon,
                                          .label = action.label,
                                          .style = IconButtonStyle::plain,
                                          .size = 48,
                                          .filled = action.filled,
                                          .destructive = action.destructive,
                                          .onTap = action.onTap});
        if (action.shown.reactive() || !action.shown.get()) {
            button->bind([b = button.get(), shown = action.shown] {
                const bool on = shown.get();
                untrack([&] { b->updateStyle([&](Style &s) { s.display = on; }); });
            });
        }
        return button;
    };
    Style rowStyle = absoluteFill();
    rowStyle.direction = Direction::row;
    rowStyle.alignItems = Align::center;
    rowStyle.justify = Justify::spaceEvenly;
    rowStyle.padding = Edges::symmetric(8, 0);
    node->idle_ = Box::create(rowStyle);
    for (const auto &action : props.idle) {
        node->idle_->appendChild(actionButton(action));
    }
    rowStyle.justify = Justify::spaceBetween;
    rowStyle.padding = {16, 0, 8, 0};
    node->selection_ = Box::create(rowStyle);
    Prop<int> count = props.selectionCount;
    // The stamp, or plain text in the utilitarian style.
    auto stamp = Stamp([count] { return std::to_string(count.get()) + " selected"; });
    auto plain = Text::create({}, initial.type.body);
    plain->setMaxLines(1);
    plain->bind([p = plain.get(), s = stamp.get(), count] {
        const std::string text = std::to_string(count.get()) + " selected";
        const Theme &th = theme();
        untrack([&] {
            p->setText(text);
            p->setTextStyle(withWeight(th.type.body, 600));
            p->setColor(ink(th, th.text.primary));
            p->updateStyle([&](Style &st) { st.display = th.style.utilitarian; });
            s->updateStyle([&](Style &st) { st.display = !th.style.utilitarian; });
        });
    });
    auto lead = Box::create({.direction = Direction::row, .alignItems = Align::center});
    lead->appendChild(stamp);
    lead->appendChild(plain);
    node->selection_->appendChild(lead);
    auto actions = Box::create({.direction = Direction::row, .alignItems = Align::center});
    for (const auto &action : props.selection) {
        actions->appendChild(actionButton(action));
    }
    node->selection_->appendChild(actions);
    glass->appendChild(node->idle_);
    glass->appendChild(node->selection_);

    node->bind([raw, glass = glass.get()] {
        const float density = environment().density();
        const float bottom = environment().safeArea().bottom / density;
        const Theme &th = theme();
        untrack([&] {
            // Utilitarian: a toolbar fixed to the bottom edge, under the navigation bar too, its
            // sides off screen.
            const bool edge = th.style.utilitarian;
            raw->updateStyle([&](Style &s) {
                s.left = edge ? -1 : th.spacing.dockInset;
                s.right = edge ? -1 : th.spacing.dockInset;
                s.bottom = edge ? -1 : bottom + th.spacing.dockInset;
            });
            glass->updateStyle([&](Style &s) { s.height = th.spacing.dock + (edge ? bottom + 1 : 0); });
            glass->setProps({.radii = gfx::CornerRadii::uniform(edge ? 0 : 1e6f), .clip = true});
            for (Node *row : {raw->idle_.get(), raw->selection_.get()}) {
                row->updateStyle([&](Style &s) {
                    s.bottom.reset();
                    s.height = th.spacing.dock;
                });
            }
        });
    });
    Prop<bool> selecting = props.selecting;
    node->bind([raw, selecting, first = std::make_shared<bool>(true)] {
        const bool on = selecting.get();
        untrack([&] {
            const float target = on ? 1.0f : 0.0f;
            const Theme &theme = currentTheme();
            if (*first) {
                raw->mode_.snapTo(target);
                *first = false;
            } else if (environment().reducedMotion.peek()) {
                raw->mode_.animateTo(target, Tween{.durationMs = theme.motion.crossfadeMs, .easing = easing::kLinear,
                                                   .crossfade = true});
            } else {
                raw->mode_.animateTo(target, Spring{theme.motion.springStiffness, theme.motion.springDamping});
            }
            // The row being left stops taking input and leaves semantics right away.
            for (Node *row : {raw->idle_.get(), raw->selection_.get()}) {
                const bool active = (row == raw->selection_.get()) == on;
                row->setHitTestBehavior(active ? HitTestBehavior::deferToChild : HitTestBehavior::none);
                row->updateSemantics([&](Semantics &s) { s.hidden = !active; });
            }
        });
    });
    node->bind([raw] {
        const float t = raw->mode_();
        const bool moving = !environment().reducedMotion();
        untrack([&] {
            const float shift = moving ? 14.0f : 0.0f;
            raw->idle_->setOpacity(std::clamp(1 - t, 0.0f, 1.0f));
            raw->idle_->setTransform(gfx::Transform::translation({0, shift * t}));
            raw->selection_->setOpacity(std::clamp(t, 0.0f, 1.0f));
            raw->selection_->setTransform(gfx::Transform::translation({0, shift * (1 - t)}));
        });
    });
    return node;
}

}  // namespace filepan::ui::components
