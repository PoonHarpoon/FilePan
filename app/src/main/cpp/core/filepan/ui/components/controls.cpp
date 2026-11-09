// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/components/controls.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "filepan/ui/components/recipes.hpp"
#include "filepan/ui/components/surfaces.hpp"
#include "filepan/ui/input.hpp"

namespace filepan::ui::components {

namespace {

constexpr float kSwitchWidth = 52;
constexpr float kSwitchHeight = 32;
constexpr float kKnobInset = 4;

// What the design doc draws as a pill, with kCorner corners.
gfx::RoundedRect pill(const gfx::Rect &rect, const Theme &theme, float density) {
    return {rect, gfx::CornerRadii::uniform(theme.cornerPx(1e6f, density))};
}

gfx::RoundedRect circle(gfx::Vec2 center, float radius) {
    return {{center.x - radius, center.y - radius, radius * 2, radius * 2}, gfx::CornerRadii::uniform(radius)};
}

// A knob: a square with kCorner corners.
gfx::RoundedRect knobShape(gfx::Vec2 center, float radius, const Theme &theme, float density) {
    gfx::RoundedRect shape = circle(center, radius);
    shape.radii = gfx::CornerRadii::uniform(theme.cornerPx(radius, density));
    return shape;
}

Spring standardSpring(const Theme &theme) { return {theme.motion.springStiffness, theme.motion.springDamping}; }

std::string percent(float value) { return std::to_string(static_cast<int>(std::lround(value * 100))) + "%"; }

}  // namespace

// --- Switch ---

std::shared_ptr<Switch> Switch::create(SwitchProps props) {
    auto node = std::make_shared<Switch>();
    Switch *raw = node.get();
    node->setDebugName("Switch");
    node->setStyle({.width = kSwitchWidth, .height = kSwitchHeight});
    node->enabled_ = props.enabled;
    Prop<bool> checked = props.checked;
    auto onChange = std::move(props.onChange);
    node->pressable_ = makePressable(*node, {.onTap =
                                                     [checked, onChange] {
                                                         if (onChange) {
                                                             onChange(!checked.get());
                                                         }
                                                     },
                                             .enabled = props.enabled,
                                             .haptic = Haptic::toggle});
    std::string label = props.label;
    node->bind([raw, checked, label, first = std::make_shared<bool>(true)] {
        const bool on = checked.get();
        untrack([&] {
            const float target = on ? 1.0f : 0.0f;
            if (*first) {
                raw->knob_.snapTo(target);
                *first = false;
            } else if (raw->knob_.target() != target) {
                raw->knob_.animateTo(target, Tween{currentTheme().motion.knobMs, easing::kSpringy});
            }
            raw->updateSemantics([&](Semantics &s) {
                s.role = Role::toggleSwitch;
                s.label = label;
                s.flags = static_cast<std::uint16_t>((s.flags & semantics_flags::kDisabled) |
                                                     semantics_flags::kCheckable |
                                                     (on ? semantics_flags::kChecked : 0));
            });
        });
    });
    node->bind([raw] {
        (void)raw->knob_();
        (void)raw->pressable_->pressed();
        (void)raw->enabled_.get();
        (void)raw->trackFocused();
        untrack([raw] { raw->markPaintDirty(); });
    });
    return node;
}

void Switch::paint(PaintContext &context) {
    const Theme &theme = currentTheme();
    const bool on = untrack([this] { return enabled_.get(); });
    const float t = knob_.peek();
    const float fill = std::clamp(t, 0.0f, 1.0f);
    const gfx::Rect bounds = context.bounds();
    const gfx::RoundedRect track = pill(bounds, theme, context.density);
    recipes::sunken(context.list, track, theme, context.density);
    if (fill > 0) {
        const gfx::Color color = on ? theme.colors.accent : grayscale(theme.colors.accent, 0.6f);
        context.list.fill(track, color.withAlpha(fill));
    }
    const float radius = context.px((kSwitchHeight - 2 * kKnobInset) / 2);
    const float travel = context.px(kSwitchWidth - kSwitchHeight);
    const gfx::Vec2 center{context.px(kSwitchHeight / 2) + travel * t, bounds.height / 2};
    recipes::knob(context.list, knobShape(center, radius, theme, context.density), context.density);
    if (pressable_->pressed.peek()) {
        context.list.fill(knobShape(center, radius + context.px(8), theme, context.density),
                          theme.colors.ink.withAlpha(0.08f));
    }
    if (!on) {
        context.list.fill(track, theme.colors.ground.withAlpha(0.45f));
    }
}

void Switch::paintOverlay(PaintContext &context) {
    if (focused()) {
        const Theme &theme = currentTheme();
        recipes::focusRing(context.list, pill(context.bounds(), theme, context.density), theme, context.density);
    }
}

// --- Segmented control ---

std::shared_ptr<SegmentedControl> SegmentedControl::create(SegmentedProps props) {
    auto node = std::make_shared<SegmentedControl>();
    SegmentedControl *raw = node.get();
    node->setDebugName("SegmentedControl");
    node->setStyle({.direction = Direction::row, .padding = Edges::all(4), .height = 44});
    node->updateSemantics([&](Semantics &s) { s.label = props.label; });
    Prop<std::size_t> selected = props.selected;
    auto onSelect = std::make_shared<std::function<void(std::size_t)>>(std::move(props.onSelect));
    const Theme &initial = currentTheme();
    for (std::size_t i = 0; i < props.options.size(); ++i) {
        auto segment = Box::create({.justify = Justify::center, .alignItems = Align::center,
                                    .padding = Edges::symmetric(8, 0), .grow = 1, .basis = 0});
        auto text = Text::create(props.options[i], withSize(initial.type.button, 13));
        text->setMaxLines(1);
        text->setStyle({.shrink = 1});
        text->updateSemantics([](Semantics &s) { s.hidden = true; });
        segment->appendChild(text);
        auto pressable = makePressable(*segment, {.onTap =
                                                          [onSelect, i] {
                                                              if (*onSelect) {
                                                                  (*onSelect)(i);
                                                              }
                                                          },
                                                  .haptic = Haptic::tick});
        std::string option = props.options[i];
        segment->bind([seg = segment.get(), t = text.get(), selected, i, option, pressable] {
            const bool on = selected.get() == i;
            const Theme &th = theme();
            const bool pressed = pressable->pressed();
            untrack([&] {
                t->setColor(on ? th.colors.onAccent : ink(th, pressed ? 1.0f : th.text.body));
                seg->updateSemantics([&](Semantics &s) {
                    s.role = Role::button;
                    s.label = option;
                    s.flags = static_cast<std::uint16_t>(semantics_flags::kCheckable |
                                                         (on ? semantics_flags::kChecked | semantics_flags::kSelected : 0));
                });
            });
        });
        segment->setOverlayHook([seg = segment.get()](PaintContext &context) {
            if (seg->focused()) {
                const Theme &theme = currentTheme();
                recipes::focusRing(context.list, pill(context.bounds(), theme, context.density), theme, context.density);
            }
        });
        segment->bind([seg = segment.get()] {
            (void)seg->trackFocused();
            untrack([seg] { seg->markPaintDirty(); });
        });
        node->appendChild(segment);
    }
    node->bind([raw, selected, first = std::make_shared<bool>(true)] {
        const auto index = static_cast<float>(selected.get());
        untrack([&] {
            if (*first) {
                raw->indicator_.snapTo(index);
                *first = false;
            } else {
                raw->indicator_.animateTo(index, standardSpring(currentTheme()));
            }
        });
    });
    node->bind([raw] {
        (void)raw->indicator_();
        untrack([raw] { raw->markPaintDirty(); });
    });
    return node;
}

void SegmentedControl::paint(PaintContext &context) {
    const Theme &theme = currentTheme();
    const gfx::Rect bounds = context.bounds();
    recipes::sunken(context.list, pill(bounds, theme, context.density), theme, context.density);
    const std::size_t count = children().size();
    if (count == 0) {
        return;
    }
    const float pad = context.px(4);
    const float width = (bounds.width - 2 * pad) / static_cast<float>(count);
    const float x = pad + width * indicator_.peek();
    recipes::raised(context.list, pill({x, pad, width, bounds.height - 2 * pad}, theme, context.density),
                    theme.colors.accent, context.density);
}

// --- Slider ---

std::shared_ptr<Slider> Slider::create(SliderProps props) {
    auto node = std::make_shared<Slider>();
    Slider *raw = node.get();
    node->setDebugName("Slider");
    node->setStyle({.height = 48, .minWidth = 120});
    node->value_ = props.value;
    node->onChange_ = std::move(props.onChange);
    node->paintTrack_ = std::move(props.paintTrack);
    auto format = props.format ? props.format : percent;
    const float step = props.step;

    auto tap = std::make_shared<TapRecognizer>();
    auto tapX = std::make_shared<float>(0);
    tap->onTapDown = [tapX](gfx::Vec2 local) { *tapX = local.x; };
    tap->onTap = [raw, tapX] { raw->setFromX(*tapX); };
    node->addGestureRecognizer(tap);
    auto drag = std::make_shared<DragRecognizer>(DragAxis::horizontal);
    drag->onStart = [raw](gfx::Vec2 local) {
        raw->finger_.start(local.x);
        raw->setFromX(raw->finger_.x());
    };
    drag->onUpdate = [raw](gfx::Vec2 delta) {
        raw->finger_.update(delta.x);
        raw->setFromX(raw->finger_.x());
    };
    node->addGestureRecognizer(drag);
    node->setFocusable(true);

    const auto stepBy = [raw, step](float direction) {
        const float next = std::clamp(raw->value_.get() + direction * step, 0.0f, 1.0f);
        if (raw->onChange_) {
            raw->onChange_(next);
        }
    };
    node->setKeyHandler([stepBy](const KeyEvent &event) {
        if (event.action != KeyAction::down) {
            return event.keyCode == keys::kDpadLeft || event.keyCode == keys::kDpadRight;
        }
        if (event.keyCode == keys::kDpadLeft || event.keyCode == keys::kDpadRight) {
            stepBy(event.keyCode == keys::kDpadRight ? 1.0f : -1.0f);
            return true;
        }
        return false;
    });
    std::string label = props.label;
    Prop<bool> enabled = props.enabled;
    node->bind([raw, label, format, stepBy, enabled, tap, drag] {
        const float value = raw->value_.get();
        const bool on = enabled.get();
        (void)raw->trackFocused();
        untrack([&] {
            tap->setEnabled(on);
            drag->setEnabled(on);
            raw->updateSemantics([&](Semantics &s) {
                s.role = Role::slider;
                s.label = label;
                s.value = format(value);
                s.flags = on ? 0 : semantics_flags::kDisabled;
                s.actions = on ? Action::scrollForward | Action::scrollBackward : 0;
                s.onAction = [stepBy](Action action, const ActionArgs &) {
                    if (action == Action::scrollForward || action == Action::scrollBackward) {
                        stepBy(action == Action::scrollForward ? 1.0f : -1.0f);
                        return true;
                    }
                    return false;
                };
            });
            raw->markPaintDirty();
        });
    });
    return node;
}

void Slider::setFromX(float xPx) {
    const float density = environment().density.peek();
    const float width = frame().width * density;
    const float inset = 14 * density;
    const float usable = std::max(1.0f, width - 2 * inset);
    const float value = std::clamp((xPx - inset) / usable, 0.0f, 1.0f);
    if (onChange_) {
        onChange_(value);
    }
}

void Slider::paint(PaintContext &context) {
    const Theme &theme = currentTheme();
    const float value = std::clamp(untrack([this] { return value_.get(); }), 0.0f, 1.0f);
    const gfx::Rect bounds = context.bounds();
    const float inset = context.px(14);
    const float cy = bounds.height / 2;
    const float h = context.px(12);
    const float x = inset + (bounds.width - 2 * inset) * value;
    if (paintTrack_) {
        const float tall = context.px(18);
        const gfx::Rect trackRect{inset - context.px(9), cy - tall / 2, bounds.width - 2 * inset + context.px(18), tall};
        paintTrack_(context.list, pill(trackRect, theme, context.density), context.density);
        recipes::knob(context.list, knobShape({x, cy}, context.px(14), theme, context.density), context.density);
        return;
    }
    const gfx::Rect track{inset - context.px(6), cy - h / 2, bounds.width - 2 * inset + context.px(12), h};
    recipes::sunken(context.list, pill(track, theme, context.density), theme, context.density);
    const gfx::Rect filled{track.x, track.y, x - track.x + h / 2, h};
    context.list.fill(pill(filled, theme, context.density), theme.colors.accent);
    recipes::knob(context.list, knobShape({x, cy}, context.px(14), theme, context.density), context.density);
    // A small accent dot in the thumb's center, the "live" mark.
    context.list.fill(circle({x, cy}, context.px(3)), theme.colors.accent);
}

void Slider::paintOverlay(PaintContext &context) {
    if (focused()) {
        const gfx::Rect bounds = context.bounds();
        const Theme &theme = currentTheme();
        recipes::focusRing(context.list,
                           pill({0, bounds.height / 2 - context.px(14), bounds.width, context.px(28)}, theme, context.density),
                           theme, context.density);
    }
}

// --- Checkbox ---

std::shared_ptr<Checkbox> Checkbox::create(CheckboxProps props) {
    auto node = std::make_shared<Checkbox>();
    Checkbox *raw = node.get();
    node->setDebugName("Checkbox");
    const bool hasText = !props.text.empty();
    node->setStyle({.direction = Direction::row,
                    .justify = hasText ? Justify::start : Justify::center,
                    .alignItems = Align::center,
                    .gap = 12,
                    .minWidth = 48,
                    .minHeight = 48});
    Prop<bool> checked = props.checked;
    auto box = Box::create({.justify = Justify::center, .alignItems = Align::center, .width = 22, .height = 22});
    auto check = Icon::create("check", 18);
    gfx::TextStyle checkStyle = check->textStyle();
    checkStyle.weight = 700;
    check->setTextStyle(checkStyle);
    box->appendChild(check);
    box->setPaintHook([checked](PaintContext &context) {
        const Theme &theme = currentTheme();
        const gfx::RoundedRect shape{context.bounds(), gfx::CornerRadii::uniform(context.px(theme.radii.tag))};
        if (untrack([&] { return checked.get(); })) {
            // A stamp: flat and solid, sharp-cornered.
            context.list.fill(shape, theme.colors.accent);
            context.list.stroke(shape, std::max(1.0f, context.density * 0.6f), gfx::Color{0, 0, 0, 0.3f});
        } else {
            recipes::sunken(context.list, shape, theme, context.density);
            context.list.stroke(shape, context.px(1.5f), ink(theme, 0.5f));
        }
    });
    node->appendChild(box);
    std::shared_ptr<Text> text;
    if (hasText) {
        text = Text::create(props.text, currentTheme().type.body);
        text->setStyle({.shrink = 1});
        text->updateSemantics([](Semantics &s) { s.hidden = true; });
        node->appendChild(text);
    }
    auto onChange = std::move(props.onChange);
    auto pressable = makePressable(*node, {.onTap =
                                                   [checked, onChange] {
                                                       if (onChange) {
                                                           onChange(!checked.get());
                                                       }
                                                   },
                                           .enabled = props.enabled,
                                           .haptic = Haptic::toggle});
    std::string label = props.label.empty() ? props.text : props.label;
    node->bind([raw, b = box.get(), c = check.get(), t = text.get(), checked, label, pressable] {
        const bool on = checked.get();
        const Theme &th = theme();
        (void)raw->trackFocused();
        (void)pressable->pressed();
        untrack([&] {
            c->setColor(th.colors.onAccent);
            c->setOpacity(on ? 1 : 0);
            if (t != nullptr) {
                t->setColor(ink(th, th.text.primary));
            }
            b->markPaintDirty();
            raw->markPaintDirty();
            raw->updateSemantics([&](Semantics &s) {
                s.role = Role::checkbox;
                s.label = label;
                s.flags = static_cast<std::uint16_t>((s.flags & semantics_flags::kDisabled) |
                                                     semantics_flags::kCheckable |
                                                     (on ? semantics_flags::kChecked : 0));
            });
        });
    });
    return node;
}

void Checkbox::paintOverlay(PaintContext &context) {
    if (children().empty()) {
        return;
    }
    const gfx::Rect box = children()[0]->frame();
    const gfx::RoundedRect shape{{box.x * context.density, box.y * context.density, box.width * context.density,
                                  box.height * context.density},
                                 gfx::CornerRadii::uniform(context.px(4))};
    if (focused()) {
        recipes::focusRing(context.list, shape, currentTheme(), context.density);
    }
}

// --- Meter ring ---

std::shared_ptr<MeterRing> MeterRing::create(MeterRingProps props) {
    auto node = std::make_shared<MeterRing>();
    MeterRing *raw = node.get();
    node->setDebugName("MeterRing");
    node->size_ = props.size;
    node->value_ = props.value;
    node->setStyle({.justify = Justify::center, .alignItems = Align::center, .width = props.size, .height = props.size});
    const Theme &initial = currentTheme();
    gfx::TextStyle numeralStyle = initial.type.display;
    numeralStyle.size = std::round(props.size * 0.3f);
    numeralStyle.lineHeight = 0.9f;
    auto numeral = Text::create({}, numeralStyle);
    auto unit = Text::create({}, initial.type.label);
    node->appendChild(numeral);
    node->appendChild(unit);
    Prop<std::string> numeralProp = props.numeral;
    Prop<std::string> unitProp = props.unit;
    std::string label = props.label;
    node->bind([raw, n = numeral.get(), u = unit.get(), numeralProp, unitProp, label] {
        const float value = std::clamp(raw->value_.get(), 0.0f, 1.0f);
        std::string number = numeralProp.reactive() ? numeralProp.get() : numeralProp.get();
        if (number.empty()) {
            number = std::to_string(static_cast<int>(std::lround(value * 100)));
        }
        const std::string unitText = unitProp.get();
        const Theme &th = theme();
        untrack([&] {
            n->setText(number);
            u->setText(unitText);
            n->setColor(ink(th, 1));
            u->setColor(textInk(th, th.text.label, 11));
            raw->markPaintDirty();
            raw->updateSemantics([&](Semantics &s) {
                s.role = Role::progress;
                s.label = label;
                s.value = percent(value);
                s.mergeDescendants = false;
            });
            n->updateSemantics([](Semantics &s) { s.hidden = true; });
            u->updateSemantics([](Semantics &s) { s.hidden = true; });
        });
    });
    return node;
}

void MeterRing::paint(PaintContext &context) {
    const Theme &theme = currentTheme();
    const ColorTokens &c = theme.colors;
    const float value = std::clamp(untrack([this] { return value_.get(); }), 0.0f, 1.0f);
    const gfx::Rect bounds = context.bounds();
    const gfx::Vec2 center = bounds.center();
    const float outer = std::min(bounds.width, bounds.height) / 2;
    const float thickness = std::max(context.px(8), outer * 0.17f);
    // The arc shows from the raised disc's edge out to just inside the track.
    const float arcOuter = outer - context.px(1);
    const float inner = outer - thickness - context.px(3);
    const gfx::RoundedRect disc = circle(center, outer);
    recipes::sunken(context.list, disc, theme, context.density);
    if (value > 0) {
        const auto &s = c.spectrum;
        const float start = -std::numbers::pi_v<float> / 2;
        gfx::GradientStops stops;
        if (value >= 1) {
            stops = {{0, s[0]}, {0.5f, s[1]}, {1, s[2]}};
        } else {
            stops = {{0, s[0]}, {value * 0.5f, s[1]}, {value, s[2]}, {value, s[2].withAlpha(0)}};
        }
        // The conic fill's ends stay square.
        context.list.fill(circle(center, arcOuter), gfx::ConicGradient{center, start, stops});
    }
    // The centre disc: the deep ground, flat, with the panels' hairline.
    const gfx::RoundedRect top = circle(center, inner);
    context.list.fill(top, c.groundDeep);
    context.list.stroke(top, context.density, c.glassTint.withAlpha(0.13f));
}

}  // namespace filepan::ui::components
