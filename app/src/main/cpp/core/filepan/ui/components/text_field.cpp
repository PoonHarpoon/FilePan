// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/components/text_field.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "filepan/ui/components/recipes.hpp"
#include "filepan/ui/components/surfaces.hpp"
#include "filepan/ui/input.hpp"

namespace filepan::ui::components {

namespace {

constexpr float kWellHeight = 48;

bool isContinuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

std::string utf8(std::uint32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

gfx::TextStyle editorStyle() { return currentTheme().type.body; }

// A password field draws a bullet (3 bytes of UTF-8) per code point.
constexpr std::string_view kMask = "\u2022";

std::string masked(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (!isContinuation(c)) {
            out += kMask;
        }
    }
    return out;
}

// A byte offset in the text to the matching one in its masked form, and back.
std::uint32_t toMasked(std::string_view text, std::uint32_t byte) {
    std::uint32_t points = 0;
    for (std::uint32_t i = 0; i < byte && i < text.size(); ++i) {
        points += isContinuation(text[i]) ? 0 : 1;
    }
    return points * static_cast<std::uint32_t>(kMask.size());
}

std::uint32_t fromMasked(std::string_view text, std::uint32_t byte) {
    std::uint32_t points = byte / static_cast<std::uint32_t>(kMask.size());
    std::uint32_t i = 0;
    while (i < text.size()) {
        if (!isContinuation(text[i])) {
            if (points == 0) {
                break;
            }
            --points;
        }
        ++i;
    }
    return i;
}

}  // namespace

// --- EditableText ---

bool EditableText::obscured() const {
    return options_.config.keyboard == KeyboardType::password || options_.config.keyboard == KeyboardType::numberPassword;
}

std::string EditableText::displayText() const {
    return obscured() ? masked(value_.peek().text) : value_.peek().text;
}

std::uint32_t EditableText::toDisplay(std::uint32_t byte) const {
    return obscured() ? toMasked(value_.peek().text, byte) : byte;
}

std::uint32_t EditableText::fromDisplay(std::uint32_t byte) const {
    return obscured() ? fromMasked(value_.peek().text, byte) : byte;
}

std::shared_ptr<EditableText> EditableText::create(Options options) {
    auto node = std::make_shared<EditableText>();
    EditableText *raw = node.get();
    node->setDebugName("EditableText");
    node->options_ = std::move(options);
    node->setFocusable(true);
    node->setTextInputClient(raw);
    node->setStyle({.grow = 1, .shrink = 1, .basis = 0});

    auto tap = std::make_shared<TapRecognizer>();
    auto tapAt = std::make_shared<gfx::Vec2>();
    tap->onTapDown = [tapAt](gfx::Vec2 local) { *tapAt = local; };
    tap->onTap = [raw, tapAt] {
        raw->requestFocus();
        raw->placeCaret(*tapAt);
    };
    node->addGestureRecognizer(tap);
    // Dragging along the line moves the caret, like Android's cursor drag.
    auto drag = std::make_shared<DragRecognizer>(DragAxis::horizontal);
    drag->onStart = [raw](gfx::Vec2 local) {
        raw->requestFocus();
        raw->finger_.start(local.x);
        raw->dragY_ = local.y;
        raw->placeCaret({raw->finger_.x(), raw->dragY_});
    };
    drag->onUpdate = [raw](gfx::Vec2 delta) {
        raw->finger_.update(delta.x);
        raw->placeCaret({raw->finger_.x(), raw->dragY_});
    };
    node->addGestureRecognizer(drag);
    node->setKeyHandler([raw](const KeyEvent &event) { return raw->handleKey(event); });

    node->bind([raw] {
        const TextEditingValue &value = raw->value_();
        (void)raw->trackFocused();
        (void)theme();
        untrack([&] {
            raw->markPaintDirty();
            raw->updateSemantics([&](Semantics &s) {
                s.role = Role::textField;
                s.value = raw->obscured() ? masked(value.text) : value.text;
                s.actions = Action::setText | Action::setSelection;
                s.onAction = [raw](Action action, const ActionArgs &args) {
                    if (action == Action::setText) {
                        const auto end = static_cast<std::int32_t>(args.text.size());
                        raw->updateEditingValue({args.text, {end, end}, {}});
                        return true;
                    }
                    if (action == Action::setSelection) {
                        raw->setSelection(args.selectionStart, args.selectionEnd);
                        return true;
                    }
                    return false;
                };
            });
        });
    });
    return node;
}

void EditableText::updateEditingValue(const TextEditingValue &value) {
    const bool textChanged = value.text != value_.peek().text;
    value_.set(value);
    if (textChanged) {
        markLayoutDirty();
        if (options_.onChange) {
            options_.onChange(value.text);
        }
    }
}

void EditableText::performAction(ImeAction action) {
    if ((action == ImeAction::done || action == ImeAction::go || action == ImeAction::search ||
         action == ImeAction::send) &&
        options_.onSubmit) {
        options_.onSubmit();
    }
}

void EditableText::setText(std::string text, TextRange selection) {
    const auto end = static_cast<std::int32_t>(text.size());
    if (!selection.valid() || selection.end > end) {
        selection = {end, end};
    }
    // From the app, not the user: no onChange echo.
    value_.set({std::move(text), selection, {}});
    markLayoutDirty();
}

const std::shared_ptr<const gfx::ShapedText> &EditableText::shape(gfx::TextSystem &text, float density,
                                                                  float fontScale, float widthPx) {
    const std::string current = displayText();
    const float width = multiline() ? widthPx : 0;
    if (!shaped_ || shapedText_ != current || shapedDensity_ != density || shapedFontScale_ != fontScale ||
        shapedWidth_ != width) {
        gfx::TextStyle style = editorStyle();
        style.size *= density * fontScale;
        gfx::TextLayoutOptions options;
        options.overflow = gfx::TextOverflow::clip;
        if (multiline() && width > 0 && width < kInfinity) {
            options.maxLines = 0;
            options.maxWidth = width;
        } else {
            options.maxLines = 1;
        }
        shaped_ = std::make_shared<const gfx::ShapedText>(text.shape(current, style, options));
        shapedText_ = current;
        shapedDensity_ = density;
        shapedFontScale_ = fontScale;
        shapedWidth_ = width;
    }
    return shaped_;
}

gfx::Size EditableText::measureContent(const LayoutContext &context, float maxWidth, float) {
    if (context.text == nullptr) {
        return {};
    }
    const float widthPx = maxWidth < kInfinity ? std::floor(maxWidth * context.density) : kInfinity;
    const auto &shaped = shape(*context.text, context.density, context.fontScale, widthPx);
    float height = std::ceil(shaped->size.height) / context.density;
    if (multiline() && !shaped->lines.empty()) {
        // Up to lines lines; at least one, even when empty.
        const float line = shaped->lines.front().height;
        height = std::ceil(std::clamp(shaped->size.height, line, line * static_cast<float>(options_.lines))) /
                 context.density;
    }
    const float width = maxWidth < kInfinity ? maxWidth : std::ceil(shaped->size.width) / context.density;
    return {width, height};
}

std::uint32_t EditableText::previousBoundary(std::uint32_t byte) const {
    if (shaped_ && shapedText_ == value_.peek().text) {
        std::uint32_t best = 0;
        bool found = false;
        for (const auto &cluster : shaped_->clusters) {
            if (cluster.byte < byte && (!found || cluster.byte > best)) {
                best = cluster.byte;
                found = true;
            }
        }
        if (found) {
            return best;
        }
    }
    const std::string &text = value_.peek().text;
    std::uint32_t b = byte;
    if (b > 0) {
        --b;
        while (b > 0 && isContinuation(text[b])) {
            --b;
        }
    }
    return b;
}

std::uint32_t EditableText::nextBoundary(std::uint32_t byte) const {
    const std::string &text = value_.peek().text;
    const auto size = static_cast<std::uint32_t>(text.size());
    if (shaped_ && shapedText_ == text) {
        std::uint32_t best = size;
        for (const auto &cluster : shaped_->clusters) {
            if (cluster.byte > byte && cluster.byte < best) {
                best = cluster.byte;
            }
        }
        return best;
    }
    std::uint32_t b = std::min(byte + 1, size);
    while (b < size && isContinuation(text[b])) {
        ++b;
    }
    return b;
}

void EditableText::placeCaret(gfx::Vec2 local, bool extend) {
    if (!shaped_) {
        return;
    }
    const gfx::Vec2 point = multiline() ? gfx::Vec2{local.x, local.y + scrollY_}
                                        : gfx::Vec2{local.x + scrollX_, shaped_->size.height / 2};
    const auto byte = static_cast<std::int32_t>(fromDisplay(shaped_->hitTest(point)));
    TextEditingValue next = value_.peek();
    next.selection = {extend ? next.selection.start : byte, byte};
    next.composing = {};
    updateEditingValue(next);
}

bool EditableText::handleKey(const KeyEvent &event) {
    if (event.action != KeyAction::down) {
        return event.keyCode == keys::kDel || event.keyCode == keys::kForwardDel || event.keyCode == keys::kDpadLeft ||
               event.keyCode == keys::kDpadRight || event.codePoint != 0;
    }
    TextEditingValue next = value_.peek();
    const auto size = static_cast<std::int32_t>(next.text.size());
    TextRange sel{std::clamp(next.selection.start, 0, size), std::clamp(next.selection.end, 0, size)};
    const TextRange ordered{std::min(sel.start, sel.end), std::max(sel.start, sel.end)};
    const bool shift = (event.modifiers & modifiers::kShift) != 0;
    const auto erase = [&](std::int32_t from, std::int32_t to) {
        next.text.erase(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from));
        next.selection = {from, from};
        next.composing = {};
        updateEditingValue(next);
    };
    switch (event.keyCode) {
    case keys::kDel:
        if (!ordered.collapsed()) {
            erase(ordered.start, ordered.end);
        } else if (ordered.start > 0) {
            erase(static_cast<std::int32_t>(previousBoundary(static_cast<std::uint32_t>(ordered.start))), ordered.start);
        }
        return true;
    case keys::kForwardDel:
        if (!ordered.collapsed()) {
            erase(ordered.start, ordered.end);
        } else if (ordered.end < size) {
            erase(ordered.end, static_cast<std::int32_t>(nextBoundary(static_cast<std::uint32_t>(ordered.end))));
        }
        return true;
    case keys::kDpadLeft:
    case keys::kDpadRight: {
        const bool left = event.keyCode == keys::kDpadLeft;
        std::int32_t caret;
        if (!shift && !ordered.collapsed()) {
            caret = left ? ordered.start : ordered.end;
        } else {
            const auto from = static_cast<std::uint32_t>(sel.end);
            caret = static_cast<std::int32_t>(left ? previousBoundary(from) : nextBoundary(from));
        }
        next.selection = {shift ? sel.start : caret, caret};
        updateEditingValue(next);
        return true;
    }
    case keys::kMoveHome:
    case keys::kMoveEnd: {
        const std::int32_t caret = event.keyCode == keys::kMoveHome ? 0 : size;
        next.selection = {shift ? sel.start : caret, caret};
        updateEditingValue(next);
        return true;
    }
    case keys::kEnter:
        if (multiline()) {
            commitText("\n");
        } else {
            performAction(options_.config.action);
        }
        return true;
    default:
        break;
    }
    if (event.codePoint >= 0x20 && event.codePoint != 0x7F) {
        commitText(utf8(event.codePoint));
        return true;
    }
    return false;
}

gfx::Rect EditableText::caretRect() const {
    if (!shaped_) {
        return {};
    }
    const float density = environment().density.peek();
    const gfx::Rect local =
            shaped_->caretRect(toDisplay(static_cast<std::uint32_t>(std::max(0, value_.peek().selection.end))));
    const gfx::Transform toWindow = nodeToWindow(*this, density);
    const gfx::Vec2 top = toWindow.apply({local.x - scrollX_, local.y - scrollY_});
    const gfx::Vec2 bottom = toWindow.apply({local.x - scrollX_, local.bottom() - scrollY_});
    return {top.x, top.y, 0, bottom.y - top.y};
}

void EditableText::paint(PaintContext &context) {
    if (context.text == nullptr) {
        return;
    }
    const Theme &theme = currentTheme();
    const ColorTokens &c = theme.colors;
    const gfx::Rect bounds = context.bounds();
    const auto &shaped = shape(*context.text, context.density, context.fontScale, std::floor(bounds.width));
    const TextEditingValue &value = value_.peek();
    const bool hasFocus = focused();

    // Keep the caret in view, with a little room past it.
    const auto caretByte = static_cast<std::uint32_t>(std::max(0, value.selection.end));
    const gfx::Rect caret = shaped->caretRect(toDisplay(caretByte));
    float y;
    if (multiline()) {
        scrollX_ = 0;
        if (caret.bottom() - scrollY_ > bounds.height) {
            scrollY_ = caret.bottom() - bounds.height;
        } else if (caret.y - scrollY_ < 0) {
            scrollY_ = caret.y;
        }
        scrollY_ = std::clamp(scrollY_, 0.0f, std::max(0.0f, shaped->size.height - bounds.height));
        y = -scrollY_;
    } else {
        const float margin = context.px(2);
        if (caret.x - scrollX_ > bounds.width - margin) {
            scrollX_ = caret.x - bounds.width + margin;
        } else if (caret.x - scrollX_ < 0) {
            scrollX_ = caret.x;
        }
        scrollX_ = std::clamp(scrollX_, 0.0f, std::max(0.0f, shaped->size.width - bounds.width + margin));
        y = std::round((bounds.height - shaped->size.height) / 2);
    }

    context.list.pushClip({{0, 0, bounds.width, bounds.height}, {}});
    context.list.pushTransform(gfx::Transform::translation({-scrollX_, y}));
    if (value.text.empty()) {
        if (!options_.placeholder.empty()) {
            gfx::TextStyle style = editorStyle();
            style.size *= context.density * context.fontScale;
            gfx::TextLayoutOptions options;
            options.maxLines = multiline() ? options_.lines : 1;
            options.maxWidth = bounds.width;
            auto placeholder =
                    std::make_shared<const gfx::ShapedText>(context.text->shape(options_.placeholder, style, options));
            context.list.text(placeholder, {0, 0}, ink(theme, theme.text.meta));
        }
    } else {
        const auto &sel = value.selection;
        if (sel.valid() || (sel.start > sel.end && sel.end >= 0)) {
            const auto a = static_cast<std::uint32_t>(std::min(sel.start, sel.end));
            const auto b = static_cast<std::uint32_t>(std::max(sel.start, sel.end));
            if (a != b && multiline()) {
                paintSelection(context, *shaped, toDisplay(a), toDisplay(b));
            } else if (a != b) {
                const gfx::Rect ra = shaped->caretRect(toDisplay(a));
                const gfx::Rect rb = shaped->caretRect(toDisplay(b));
                const float left = std::min(ra.x, rb.x);
                const float right = std::max(ra.x, rb.x);
                context.list.fill(gfx::RoundedRect{{left, ra.y, right - left, ra.height},
                                                   gfx::CornerRadii::uniform(context.px(3))},
                                  c.accent.withAlpha(0.35f));
            }
        }
        if (value.composing.valid() && !value.composing.collapsed()) {
            const gfx::Rect ra = shaped->caretRect(toDisplay(static_cast<std::uint32_t>(value.composing.start)));
            const gfx::Rect rb = shaped->caretRect(toDisplay(static_cast<std::uint32_t>(value.composing.end)));
            const float t = std::max(1.0f, std::round(context.density));
            context.list.fill(gfx::Rect{std::min(ra.x, rb.x), ra.bottom() - t, std::abs(rb.x - ra.x), t},
                              ink(theme, 0.7f));
        }
        context.list.text(shaped, {0, 0}, c.ink);
    }
    if (hasFocus) {
        const float w = std::max(2.0f, std::round(context.px(2)));
        context.list.fill(gfx::RoundedRect{{std::round(caret.x - w / 2), caret.y + context.px(1), w,
                                            caret.height - context.px(2)},
                                           gfx::CornerRadii::uniform(w / 2)},
                          c.accent);
    }
    context.list.pop();
    context.list.pop();
}

// A selection across wrapped lines: a band per line it touches, to the line's end where it goes on.
void EditableText::paintSelection(PaintContext &context, const gfx::ShapedText &shaped, std::uint32_t from,
                                  std::uint32_t to) const {
    const gfx::Color color = currentTheme().colors.accent.withAlpha(0.35f);
    for (const gfx::TextLine &line : shaped.lines) {
        const std::uint32_t start = std::max(from, line.startByte);
        const std::uint32_t end = std::min(to, line.endByte);
        if (start > end || (start == end && to <= line.endByte)) {
            continue;
        }
        const float left = start == line.startByte ? 0 : shaped.caretRect(start).x;
        // At the line's end, caretRect would give the next line's start.
        const float right = end >= line.endByte ? std::max(line.width, left + context.px(4)) : shaped.caretRect(end).x;
        if (right > left) {
            context.list.fill(gfx::RoundedRect{{left, line.top, right - left, line.height},
                                               gfx::CornerRadii::uniform(context.px(3))},
                              color);
        }
    }
}

// --- TextField ---

std::shared_ptr<TextField> TextField::create(TextFieldProps props) {
    auto node = std::make_shared<TextField>();
    TextField *raw = node.get();
    node->setDebugName("TextField");
    node->setStyle({.gap = 8});
    const Theme &initial = currentTheme();

    auto label = Text::create(props.label, initial.type.label);
    label->updateSemantics([](Semantics &s) { s.hidden = true; });
    node->appendChild(label);

    const bool multiline = props.lines > 1;
    // A multi-line well grows from the single-line height, its first line where a single one is.
    constexpr float kMultilinePadding = 13;
    auto well = Box::create(multiline ? Style{.direction = Direction::row,
                                              .alignItems = Align::start,
                                              .gap = 10,
                                              .padding = Edges::symmetric(16, kMultilinePadding),
                                              .minHeight = kWellHeight}
                                      : Style{.direction = Direction::row,
                                              .alignItems = Align::center,
                                              .gap = 10,
                                              .padding = Edges::symmetric(16, 0),
                                              .height = kWellHeight});
    node->well_ = well;
    std::shared_ptr<Icon> icon;
    if (!props.icon.empty()) {
        icon = Icon::create(props.icon, 20);
        if (multiline) {
            icon->updateStyle([](Style &s) { s.margin.top = 1; });
        }
        well->appendChild(icon);
    }
    TextInputConfig config = props.config;
    if (multiline) {
        config.multiline = true;
        // Enter is a newline; the IME shows no action key.
        config.action = ImeAction::none;
    }
    node->editor_ = EditableText::create({.placeholder = props.placeholder,
                                          .config = config,
                                          .lines = std::max(props.lines, 1),
                                          .onChange = std::move(props.onChange),
                                          .onSubmit = std::move(props.onSubmit)});
    node->editor_->setText(props.text);
    well->appendChild(node->editor_);
    // A tap anywhere in the well focuses the field and puts the caret at the end.
    auto wellTap = std::make_shared<TapRecognizer>();
    wellTap->onTap = [raw] {
        EditableText &editor = *raw->editor_;
        editor.requestFocus();
        const auto end = static_cast<std::int32_t>(editor.editingValue().text.size());
        editor.setSelection(end, end);
    };
    well->addGestureRecognizer(wellTap);
    node->appendChild(well);

    auto helper = Text::create({}, initial.type.label);
    helper->setMaxLines(2);
    helper->updateSemantics([](Semantics &s) { s.hidden = true; });
    node->appendChild(helper);

    Prop<std::string> helperProp = props.helper;
    Prop<std::string> errorProp = props.error;
    const std::string labelText = props.label;
    Signal<bool> errorState(false);
    well->setPaintHook([w = well.get(), ed = node->editor_.get(), errorState](PaintContext &context) {
        const Theme &theme = currentTheme();
        const gfx::RoundedRect shape{context.bounds(), gfx::CornerRadii::uniform(context.px(theme.radii.input))};
        recipes::sunken(context.list, shape, theme, context.density);
        const bool error = errorState.peek();
        const gfx::Color edge = error ? legible(theme.colors.destructive, theme.colors.ground, 3) : theme.colors.accent;
        if (ed->focused() || error) {
            context.list.stroke(shape, context.px(1.5f), edge);
        }
        (void)w;
    });
    node->bind([raw, l = label.get(), h = helper.get(), i = icon.get(), w = well.get(), helperProp, errorProp,
                labelText, errorState, placeholder = props.placeholder] {
        const Theme &th = theme();
        const std::string error = errorProp.get();
        const std::string help = helperProp.get();
        const bool focused = raw->editor_->trackFocused();
        untrack([&] {
            errorState.set(!error.empty());
            const gfx::Color errorInk = legible(mix(th.colors.destructive, th.colors.ink, 0.2f), th.colors.ground);
            l->setColor(error.empty() ? (focused ? legible(th.colors.accent, th.colors.ground)
                                                 : textInk(th, th.text.label, 11))
                                      : errorInk);
            h->setText(error.empty() ? help : error);
            h->setColor(error.empty() ? textInk(th, th.text.meta, 11) : errorInk);
            h->updateStyle([&](Style &s) { s.display = !(error.empty() ? help : error).empty(); });
            if (i != nullptr) {
                i->setColor(ink(th, focused ? 0.9f : th.text.label));
            }
            w->markPaintDirty();
            raw->editor_->updateSemantics([&](Semantics &s) {
                s.label = labelText;
                s.hint = !error.empty() ? error : !help.empty() ? help : placeholder;
            });
        });
    });
    return node;
}

}  // namespace filepan::ui::components
