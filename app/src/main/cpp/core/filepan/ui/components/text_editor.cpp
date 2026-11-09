// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/components/text_editor.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

#include "filepan/ui/gesture.hpp"
#include "filepan/ui/input.hpp"

namespace filepan::ui::components {

namespace {

// The IME's window: this many bytes either side of the selection, moved once the selection gets
// within kWindowMargin of an edge.
constexpr std::uint32_t kWindowRadius = 4096;
constexpr std::uint32_t kWindowMargin = 1024;
constexpr float kFontSize = 13;  // sp
constexpr float kLineHeight = 1.45f;
constexpr float kGutterPad = 12;  // dp after the numbers.
constexpr float kEndPad = 8;      // dp after the text.
// A row shapes at most this much of its line, then a marker: shaping a megabyte-long line (a
// minified file) on every bind would stall scrolling. The caret stays within what's shown.
constexpr std::uint32_t kMaxShownLine = 16 * 1024;
constexpr std::string_view kCutMarker = " \u2026";

bool isContinuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

bool isWordByte(char c) {
    const auto b = static_cast<unsigned char>(c);
    return b >= 0x80 || std::isalnum(b) || b == '_';
}

gfx::TextStyle editorStyle() {
    return {.family = gfx::FontFamily::mono, .size = kFontSize, .weight = 400, .lineHeight = kLineHeight};
}

TextRange ordered(TextRange range) { return {std::min(range.start, range.end), std::max(range.start, range.end)}; }

}  // namespace

// --- TextLineRow ---

// One line of the document: its number in the gutter, then its text, wrapped or not.
class TextLineRow final : public Node {
public:
    explicit TextLineRow(TextEditor *editor) : editor_(editor) {
        setDebugName("TextLineRow");
        setHitTestBehavior(HitTestBehavior::opaque);
        auto tap = std::make_shared<TapRecognizer>();
        auto at = std::make_shared<gfx::Vec2>();
        tap->onTapDown = [at](gfx::Vec2 local) { *at = local; };
        tap->onTap = [this, at] {
            editor_->requestFocus();
            editor_->goalX_ = -1;
            const std::uint32_t offset = editor_->offsetAt(*this, *at);
            editor_->setSelection({static_cast<std::int32_t>(offset), static_cast<std::int32_t>(offset)});
        };
        addGestureRecognizer(tap);
        // A long press selects a word; moving on extends the selection to the finger.
        auto press = std::make_shared<LongPressRecognizer>();
        auto word = std::make_shared<TextRange>();
        press->onLongPressStart = [this, word](gfx::Vec2 local) {
            editor_->requestFocus();
            editor_->selectWordAt(editor_->offsetAt(*this, local));
            *word = ordered(editor_->peekSelection());
        };
        press->onLongPressMove = [this, word](gfx::Vec2 local, gfx::Vec2) {
            const auto offset = static_cast<std::int32_t>(editor_->offsetAt(*this, local));
            if (offset < word->start) {
                editor_->setSelection({word->end, offset});
            } else {
                editor_->setSelection({word->start, std::max(offset, word->end)});
            }
        };
        addGestureRecognizer(press);
    }

    void setLine(std::size_t line) {
        line_ = line;
        shaped_.reset();
        markLayoutDirty();
        markPaintDirty();
        updateSemantics([&](Semantics &s) {
            s.role = Role::text;
            s.label = std::string(editor_->document().line(line).substr(0, 500));
        });
    }
    [[nodiscard]] std::size_t line() const { return line_; }

    [[nodiscard]] bool hasMeasure() const override { return true; }
    gfx::Size measureContent(const LayoutContext &context, float maxWidth, float) override {
        if (context.text == nullptr) {
            return {};
        }
        const float widthPx = maxWidth < kInfinity ? std::floor(maxWidth * context.density) : 0;
        const auto &shaped = shape(*context.text, context.density, context.fontScale, widthPx);
        const float height = std::max(shaped->size.height, lineHeightPx(context.density, context.fontScale));
        return {maxWidth < kInfinity ? maxWidth : 0, std::ceil(height) / context.density};
    }

    // Where the text starts in the row, in px: after the gutter, less the sideways scroll.
    [[nodiscard]] float textX(float density) const {
        return std::round(editor_->gutterDp() * density) - (editor_->wrap() ? 0 : editor_->scrollX_);
    }

    // The byte of the line under a point in the row's px.
    [[nodiscard]] std::uint32_t byteAt(gfx::Vec2 local, float density) const {
        if (!shaped_) {
            return 0;
        }
        return std::min(shown_, shaped_->hitTest({local.x - textX(density),
                                                  std::clamp(local.y, 0.0f, std::max(0.0f, shaped_->size.height - 1))}));
    }

    // The caret before byte, in the row's px.
    [[nodiscard]] gfx::Rect caretAt(std::uint32_t byte, float density, float fontScale) const {
        const float x0 = textX(density);
        if (!shaped_ || shaped_->lines.empty()) {
            return {x0, 0, 0, lineHeightPx(density, fontScale)};
        }
        const gfx::Rect caret = shaped_->caretRect(std::min(byte, shown_));
        return {caret.x + x0, caret.y, 0, caret.height};
    }

    [[nodiscard]] float textWidth() const { return shaped_ ? shaped_->size.width : 0; }

protected:
    void paint(PaintContext &context) override {
        if (context.text == nullptr) {
            return;
        }
        const Theme &theme = currentTheme();
        const ColorTokens &c = theme.colors;
        const gfx::Rect bounds = context.bounds();
        const auto &shaped = shape(*context.text, context.density, context.fontScale, bounds.width);
        const text::TextDocument &doc = editor_->document();
        const std::uint32_t lineStart = doc.lineStart(line_);
        const std::uint32_t lineEnd = doc.lineEnd(line_);
        const TextRange selection = editor_->peekSelection();
        const auto caret = static_cast<std::uint32_t>(selection.end);
        const bool caretHere = caret >= lineStart && caret <= lineEnd;
        const float x0 = textX(context.density);
        const float gutter = std::round(editor_->gutterDp() * context.density);

        // The text, clipped clear of the gutter when it scrolls sideways.
        context.list.pushClip({{gutter, 0, std::max(0.0f, bounds.width - gutter), bounds.height}, {}});
        const TextRange range = ordered(selection);
        if (!range.collapsed() && static_cast<std::uint32_t>(range.start) <= lineEnd &&
            static_cast<std::uint32_t>(range.end) > lineStart) {
            const std::uint32_t from = std::min(std::max<std::uint32_t>(range.start, lineStart) - lineStart, shown_);
            const std::uint32_t to = std::min(std::min<std::uint32_t>(range.end, lineEnd) - lineStart, shown_);
            // The line break is selected too: the band runs on past the text.
            const bool pastEnd = static_cast<std::uint32_t>(range.end) > lineEnd;
            paintSelection(context, *shaped, from, to, pastEnd, x0);
        }
        context.list.text(shaped, {x0, 0}, c.ink);
        const TextRange composing = editor_->composing_;
        if (composing.valid() && !composing.collapsed() && static_cast<std::uint32_t>(composing.start) >= lineStart &&
            static_cast<std::uint32_t>(composing.end) <= lineEnd) {
            const gfx::Rect a = caretAt(static_cast<std::uint32_t>(composing.start) - lineStart, context.density,
                                        context.fontScale);
            const gfx::Rect b = caretAt(static_cast<std::uint32_t>(composing.end) - lineStart, context.density,
                                        context.fontScale);
            const float t = std::max(1.0f, std::round(context.density));
            context.list.fill(gfx::Rect{std::min(a.x, b.x), a.bottom() - t, std::abs(b.x - a.x), t}, ink(theme, 0.7f));
        }
        if (caretHere && editor_->focused() && !editor_->readOnly()) {
            const gfx::Rect r = caretAt(caret - lineStart, context.density, context.fontScale);
            const float w = std::max(2.0f, std::round(context.px(2)));
            context.list.fill(gfx::RoundedRect{{std::round(r.x - w / 2), r.y + context.px(1), w, r.height - context.px(2)},
                                               gfx::CornerRadii::uniform(w / 2)},
                              c.accent);
        }
        context.list.pop();

        // The number, right-aligned in the gutter, on the line's first row.
        gfx::TextStyle numberStyle = editorStyle();
        numberStyle.size *= context.density * context.fontScale;
        gfx::TextLayoutOptions options;
        options.maxLines = 1;
        auto number = std::make_shared<const gfx::ShapedText>(
                context.text->shape(std::to_string(line_ + 1), numberStyle, options));
        const float numberX = gutter - context.px(kGutterPad) - number->size.width;
        context.list.text(number, {numberX, 0}, caretHere ? ink(theme, theme.text.body)
                                                          : textInk(theme, theme.text.quiet, kFontSize));
    }

private:
    [[nodiscard]] static float lineHeightPx(float density, float fontScale) {
        return std::ceil(kFontSize * kLineHeight * density * fontScale);
    }

    const std::shared_ptr<const gfx::ShapedText> &shape(gfx::TextSystem &text, float density, float fontScale,
                                                        float rowWidthPx) {
        const float width = editor_->wrap()
                                    ? std::max(1.0f, rowWidthPx - std::round(editor_->gutterDp() * density) -
                                                             std::round(kEndPad * density))
                                    : 0;
        if (!shaped_ || shapedWidth_ != width || density_ != density || fontScale_ != fontScale) {
            gfx::TextStyle style = editorStyle();
            style.size *= density * fontScale;
            gfx::TextLayoutOptions options;
            options.overflow = gfx::TextOverflow::clip;
            options.maxLines = 0;
            if (width > 0) {
                options.maxWidth = width;
            }
            const std::string_view line = editor_->document().line(line_);
            shown_ = static_cast<std::uint32_t>(line.size());
            std::string cut;
            if (shown_ > kMaxShownLine) {
                shown_ = kMaxShownLine;
                while (shown_ > 0 && isContinuation(line[shown_])) {
                    --shown_;
                }
                cut = std::string(line.substr(0, shown_)) + std::string(kCutMarker);
            }
            shaped_ = std::make_shared<const gfx::ShapedText>(text.shape(cut.empty() ? line : cut, style, options));
            shapedWidth_ = width;
            density_ = density;
            fontScale_ = fontScale;
        }
        return shaped_;
    }

    // A band per visual line the selection touches, on to the line's end where it goes on.
    void paintSelection(PaintContext &context, const gfx::ShapedText &shaped, std::uint32_t from, std::uint32_t to,
                        bool pastEnd, float x0) const {
        const gfx::Color color = currentTheme().colors.accent.withAlpha(0.35f);
        const auto band = [&](float left, float right, float top, float height) {
            if (right > left) {
                context.list.fill(gfx::RoundedRect{{x0 + left, top, right - left, height},
                                                   gfx::CornerRadii::uniform(context.px(3))},
                                  color);
            }
        };
        if (shaped.lines.empty()) {
            // An empty line whose break is selected.
            band(0, context.px(6), 0, lineHeightPx(context.density, context.fontScale));
            return;
        }
        for (std::size_t i = 0; i < shaped.lines.size(); ++i) {
            const gfx::TextLine &line = shaped.lines[i];
            const bool last = i + 1 == shaped.lines.size();
            const std::uint32_t start = std::max(from, line.startByte);
            const std::uint32_t end = std::min(to, line.endByte);
            const bool runsOn = to > line.endByte || (last && pastEnd);
            if (start > end || (start == end && !runsOn)) {
                continue;
            }
            const float left = start == line.startByte ? 0 : shaped.caretRect(start).x;
            const float right = runsOn || end >= line.endByte ? std::max(line.width, left) + (runsOn ? context.px(6) : 0)
                                                               : shaped.caretRect(end).x;
            band(left, right, line.top, line.height);
        }
    }

    TextEditor *editor_;
    std::size_t line_ = 0;
    std::uint32_t shown_ = 0;  // The bytes of the line shaped; the rest is cut.
    std::shared_ptr<const gfx::ShapedText> shaped_;
    float shapedWidth_ = -1;
    float density_ = 0;
    float fontScale_ = 0;
};

// --- TextEditor ---

std::shared_ptr<TextEditor> TextEditor::create(text::TextDocument document, Options options) {
    auto node = std::make_shared<TextEditor>();
    TextEditor *raw = node.get();
    node->setDebugName("TextEditor");
    node->options_ = std::move(options);
    node->document_ = std::move(document);
    node->gutterDigits_ = std::to_string(node->document_.lineCount()).size();
    node->setFocusable(true);
    if (!node->options_.readOnly) {
        node->setTextInputClient(raw);
    }
    node->setStyle({.grow = 1, .shrink = 1, .basis = 0});
    node->setClip(true);

    node->list_ = VirtualList::create();
    node->list_->setStyle({.grow = 1, .shrink = 1, .basis = 0});
    node->list_->setEstimatedItemExtent(std::ceil(kFontSize * kLineHeight));
    node->list_->setCreate([raw](std::uint32_t) { return std::make_shared<TextLineRow>(raw); });
    node->list_->setBind([](std::size_t index, Node &row) { static_cast<TextLineRow &>(row).setLine(index); });
    node->list_->setItemCount(node->document_.lineCount());
    node->appendChild(node->list_);

    // Without wrapping, a sideways drag scrolls the lines.
    auto sideways = std::make_shared<DragRecognizer>(DragAxis::horizontal);
    sideways->onUpdate = [raw](gfx::Vec2 delta) {
        if (raw->wrap()) {
            return;
        }
        float widest = 0;
        const auto [first, last] = raw->list_->boundRange();
        for (std::size_t i = first; i < last; ++i) {
            if (Node *row = raw->list_->nodeFor(i)) {
                widest = std::max(widest, static_cast<TextLineRow *>(row)->textWidth());
            }
        }
        const float density = environment().density.peek();
        const float room = raw->frame().width * density - std::round(raw->gutterDp() * density) - kEndPad * density;
        raw->scrollX_ = std::clamp(raw->scrollX_ - delta.x, 0.0f, std::max(0.0f, widest - room));
        raw->repaintRows();
    };
    node->addGestureRecognizer(sideways);

    node->setKeyHandler([raw](const KeyEvent &event) { return raw->handleKey(event); });
    node->bind([raw] {
        (void)raw->trackFocused();
        untrack([&] { raw->repaintRows(); });
    });
    node->bind([raw] {
        (void)theme();
        untrack([&] { raw->list_->notifyChanged(0, raw->document_.lineCount()); });
    });
    node->updateSemantics([&](Semantics &s) {
        s.role = Role::textField;
        s.label = node->options_.readOnly ? "Text" : "Text editor";
    });
    node->syncWindow(true);
    return node;
}

float TextEditor::gutterDp() const {
    // JetBrains Mono's figures are 0.6 em wide.
    return std::ceil(static_cast<float>(std::max<std::size_t>(gutterDigits_, 2)) * kFontSize * 0.6f +
                     kGutterPad + kGutterPad);
}

void TextEditor::setWrap(bool wrap) {
    if (options_.wrap == wrap) {
        return;
    }
    options_.wrap = wrap;
    scrollX_ = 0;
    list_->notifyChanged(0, document_.lineCount());
}

Node *TextEditor::rowFor(std::size_t line) const { return list_->nodeFor(line); }

void TextEditor::repaintRows() {
    const auto [first, last] = list_->boundRange();
    for (std::size_t i = first; i < last; ++i) {
        if (Node *row = list_->nodeFor(i)) {
            row->markPaintDirty();
        }
    }
}

std::uint32_t TextEditor::previousCharacter(std::uint32_t offset) const {
    const std::string &text = document_.text();
    if (offset == 0) {
        return 0;
    }
    --offset;
    while (offset > 0 && isContinuation(text[offset])) {
        --offset;
    }
    return offset;
}

std::uint32_t TextEditor::nextCharacter(std::uint32_t offset) const {
    const std::string &text = document_.text();
    const std::uint32_t size = document_.size();
    if (offset >= size) {
        return size;
    }
    ++offset;
    while (offset < size && isContinuation(text[offset])) {
        ++offset;
    }
    return offset;
}

void TextEditor::setSelection(TextRange selection, bool keepGoal) {
    const auto size = static_cast<std::int32_t>(document_.size());
    selection = {std::clamp(selection.start, 0, size), std::clamp(selection.end, 0, size)};
    if (!keepGoal) {
        goalX_ = -1;
    }
    if (selection != selection_.peek()) {
        composing_ = {};
        selection_.set(selection);
        repaintRows();
    }
    syncWindow(true);
}

void TextEditor::select(std::uint32_t anchor, std::uint32_t caret) {
    setSelection({static_cast<std::int32_t>(anchor), static_cast<std::int32_t>(caret)});
}

void TextEditor::selectAll() { select(0, document_.size()); }

void TextEditor::selectWordAt(std::uint32_t offset) {
    const std::string &text = document_.text();
    std::uint32_t start = std::min(offset, document_.size());
    std::uint32_t end = start;
    while (start > 0 && isWordByte(text[start - 1])) {
        --start;
    }
    while (end < document_.size() && isWordByte(text[end])) {
        ++end;
    }
    if (start == end && end < document_.size()) {
        end = nextCharacter(end);
    }
    select(start, end);
}

std::string TextEditor::selectedText() const {
    const TextRange range = ordered(selection_.peek());
    return document_.text().substr(static_cast<std::size_t>(range.start),
                                   static_cast<std::size_t>(range.end - range.start));
}

void TextEditor::copy() {
    if (!selection_.peek().collapsed() && options_.onCopy) {
        options_.onCopy(selectedText());
    }
}

void TextEditor::cut() {
    if (options_.readOnly || selection_.peek().collapsed()) {
        return;
    }
    copy();
    replaceSelection("");
}

void TextEditor::paste() {
    if (options_.readOnly || !options_.onPaste) {
        return;
    }
    if (const auto text = options_.onPaste(); text && !text->empty()) {
        replaceSelection(*text);
    }
}

void TextEditor::replaceSelection(std::string_view text) {
    if (options_.readOnly) {
        return;
    }
    const TextRange range = ordered(selection_.peek());
    replace(static_cast<std::uint32_t>(range.start), static_cast<std::uint32_t>(range.end), text);
}

void TextEditor::replace(std::uint32_t start, std::uint32_t end, std::string_view text, bool fromIme) {
    const std::size_t linesBefore = document_.lineCount();
    const std::uint32_t sizeBefore = document_.size();
    start = std::min(start, sizeBefore);
    end = std::clamp(end, start, sizeBefore);
    const std::size_t firstLine = document_.lineOf(start);
    const std::size_t lastLine = document_.lineOf(end);
    document_.replace(start, end, text);
    const std::uint32_t inserted = document_.size() + (end - start) - sizeBefore;

    // Rows: the first line changed, the rest of the old range went, and the new lines came in.
    const std::size_t removed = lastLine - firstLine;
    const std::size_t added = removed + document_.lineCount() - linesBefore;
    if (removed > 0) {
        list_->notifyRemoved(firstLine + 1, removed);
    }
    if (added > 0) {
        list_->notifyInserted(firstLine + 1, added);
    }
    list_->notifyChanged(firstLine);
    if (const std::size_t digits = std::to_string(document_.lineCount()).size(); digits != gutterDigits_) {
        gutterDigits_ = digits;
        list_->notifyChanged(0, document_.lineCount());
    }

    // The window moves with text before it, and stretches with text inside it.
    const std::int64_t delta = static_cast<std::int64_t>(inserted) - (end - start);
    if (!fromIme) {
        if (end <= windowStart_ && start < windowStart_) {
            windowStart_ = static_cast<std::uint32_t>(windowStart_ + delta);
            windowEnd_ = static_cast<std::uint32_t>(windowEnd_ + delta);
        } else if (start <= windowEnd_) {
            windowEnd_ = static_cast<std::uint32_t>(std::max<std::int64_t>(windowStart_, windowEnd_ + delta));
        }
    }
    goalX_ = -1;
    revision_.set(revision_.peek() + 1);
    if (!fromIme) {
        const auto caret = static_cast<std::int32_t>(start + inserted);
        composing_ = {};
        selection_.set({caret, caret});
        syncWindow(true);
        repaintRows();
        revealOffset(static_cast<std::uint32_t>(caret));
    }
    if (options_.onChange) {
        options_.onChange();
    }
}

void TextEditor::updateEditingValue(const TextEditingValue &value) {
    const std::uint32_t base = windowStart_;
    if (!options_.readOnly && value.text != window_.text) {
        const std::string &old = window_.text;
        std::size_t prefix = 0;
        const std::size_t shorter = std::min(old.size(), value.text.size());
        while (prefix < shorter && old[prefix] == value.text[prefix]) {
            ++prefix;
        }
        std::size_t suffix = 0;
        while (suffix < shorter - prefix && old[old.size() - 1 - suffix] == value.text[value.text.size() - 1 - suffix]) {
            ++suffix;
        }
        const std::string_view middle = std::string_view(value.text).substr(prefix, value.text.size() - prefix - suffix);
        replace(static_cast<std::uint32_t>(base + prefix), static_cast<std::uint32_t>(base + old.size() - suffix), middle,
                true);
        windowEnd_ = base + static_cast<std::uint32_t>(value.text.size());
    }
    const auto at = [&](std::int32_t offset) { return static_cast<std::int32_t>(base) + std::max(0, offset); };
    const auto size = static_cast<std::int32_t>(document_.size());
    const TextRange selection{std::min(at(value.selection.start), size), std::min(at(value.selection.end), size)};
    composing_ = value.composing.valid() && !options_.readOnly
                         ? TextRange{std::min(at(value.composing.start), size), std::min(at(value.composing.end), size)}
                         : TextRange{};
    selection_.set(selection);
    syncWindow(true);
    repaintRows();
    revealOffset(static_cast<std::uint32_t>(selection.end));
}

void TextEditor::syncWindow(bool allowMove) {
    const std::uint32_t size = document_.size();
    const TextRange selection = ordered(selection_.peek());
    auto lo = static_cast<std::uint32_t>(selection.start);
    auto hi = static_cast<std::uint32_t>(selection.end);
    const bool composing = composing_.valid();
    if (composing) {
        lo = std::min(lo, static_cast<std::uint32_t>(composing_.start));
        hi = std::max(hi, static_cast<std::uint32_t>(composing_.end));
    }
    const bool fits = windowEnd_ <= size && windowStart_ <= lo && hi <= windowEnd_;
    const bool nearEdge = (windowStart_ > 0 && lo - windowStart_ < kWindowMargin) ||
                          (windowEnd_ < size && windowEnd_ - hi < kWindowMargin);
    if (!fits || (allowMove && !composing && nearEdge)) {
        const std::string &text = document_.text();
        windowStart_ = lo > kWindowRadius ? lo - kWindowRadius : 0;
        while (windowStart_ > 0 && isContinuation(text[windowStart_])) {
            --windowStart_;
        }
        windowEnd_ = std::min(size, hi + kWindowRadius);
        while (windowEnd_ < size && isContinuation(text[windowEnd_])) {
            ++windowEnd_;
        }
    }
    const auto relative = [&](std::int32_t offset) { return offset - static_cast<std::int32_t>(windowStart_); };
    window_.text = document_.text().substr(windowStart_, windowEnd_ - windowStart_);
    window_.selection = {relative(selection_.peek().start), relative(selection_.peek().end)};
    window_.composing = composing ? TextRange{relative(composing_.start), relative(composing_.end)} : TextRange{};
}

void TextEditor::revealOffset(std::uint32_t offset) {
    const std::size_t line = document_.lineOf(std::min(offset, document_.size()));
    const float top = list_->itemOffset(line);
    Node *row = rowFor(line);
    const float height = row != nullptr ? row->frame().height : std::ceil(kFontSize * kLineHeight);
    const float viewport = list_->viewportExtent();
    if (viewport <= 0) {
        list_->scrollToItem(line);
    } else if (top < list_->offset()) {
        list_->scrollTo(top);
    } else if (top + height > list_->offset() + viewport) {
        list_->scrollTo(std::min(top, top + height - viewport));
    }
    // Sideways, without wrapping: keep the caret clear of the edges.
    if (!wrap() && row != nullptr) {
        auto &r = static_cast<TextLineRow &>(*row);
        const float density = environment().density.peek();
        const float fontScale = environment().fontScale.peek();
        const float x = r.caretAt(offset - document_.lineStart(line), density, fontScale).x + scrollX_ -
                        std::round(gutterDp() * density);
        const float room = frame().width * density - std::round(gutterDp() * density) - kEndPad * density;
        if (x - scrollX_ > room) {
            scrollX_ = x - room;
        } else if (x < scrollX_) {
            scrollX_ = std::max(0.0f, x - room / 4);
        }
        repaintRows();
    }
}

std::uint32_t TextEditor::offsetAt(const Node &from, gfx::Vec2 local) const {
    const float density = environment().density.peek();
    const auto [first, last] = list_->boundRange();
    if (first >= last) {
        return 0;
    }
    // The point in the list's content dp, then the row there (or the nearest bound one).
    const float y = from.frame().y + local.y / density;
    const TextLineRow *target = nullptr;
    for (std::size_t i = first; i < last; ++i) {
        const auto *row = static_cast<const TextLineRow *>(list_->nodeFor(i));
        if (row == nullptr) {
            continue;
        }
        if (target == nullptr || y >= row->frame().y) {
            target = row;
        }
        if (y < row->frame().bottom()) {
            break;
        }
    }
    if (target == nullptr) {
        return 0;
    }
    const float rowY = (y - target->frame().y) * density;
    const std::uint32_t byte = target->byteAt({local.x, rowY}, density);
    return std::min(document_.lineStart(target->line()) + byte, document_.lineEnd(target->line()));
}

gfx::Rect TextEditor::caretRect() const {
    const auto caret = static_cast<std::uint32_t>(selection_.peek().end);
    const std::size_t line = document_.lineOf(caret);
    const auto *row = static_cast<const TextLineRow *>(rowFor(line));
    if (row == nullptr) {
        return {};
    }
    const float density = environment().density.peek();
    const gfx::Rect local = row->caretAt(caret - document_.lineStart(line), density, environment().fontScale.peek());
    const gfx::Transform toWindow = nodeToWindow(*row, density);
    const gfx::Vec2 top = toWindow.apply({local.x, local.y});
    const gfx::Vec2 bottom = toWindow.apply({local.x, local.bottom()});
    return {top.x, top.y, 0, bottom.y - top.y};
}

void TextEditor::moveVertically(int lines, bool extend) {
    const TextRange selection = selection_.peek();
    const auto caret = static_cast<std::uint32_t>(selection.end);
    const std::size_t line = document_.lineOf(caret);
    const float density = environment().density.peek();
    const float fontScale = environment().fontScale.peek();
    std::uint32_t next = caret;
    const auto *row = static_cast<const TextLineRow *>(rowFor(line));
    const auto column = [&](std::size_t target) {
        // Off screen: the same byte column, on a character boundary.
        std::uint32_t offset = std::min(document_.lineStart(target) + (caret - document_.lineStart(line)),
                                        document_.lineEnd(target));
        while (offset > document_.lineStart(target) && isContinuation(document_.text()[offset])) {
            --offset;
        }
        return offset;
    };
    if (std::abs(lines) == 1 && row != nullptr) {
        const gfx::Rect at = row->caretAt(caret - document_.lineStart(line), density, fontScale);
        if (goalX_ < 0) {
            goalX_ = at.x;
        }
        const float y = lines < 0 ? at.y - 1 : at.bottom() + 1;
        if (y >= 0 && y < row->frame().height * density) {
            // Another visual line of the same wrapped line.
            next = document_.lineStart(line) + row->byteAt({goalX_, y}, density);
        } else if (lines < 0 ? line == 0 : line + 1 >= document_.lineCount()) {
            next = lines < 0 ? 0 : document_.size();
        } else {
            const std::size_t target = lines < 0 ? line - 1 : line + 1;
            const auto *other = static_cast<const TextLineRow *>(rowFor(target));
            next = other != nullptr ? document_.lineStart(target) +
                                              other->byteAt({goalX_, lines < 0 ? other->frame().height * density - 1 : 0},
                                                            density)
                                    : column(target);
        }
    } else {
        const auto target = static_cast<std::size_t>(
                std::clamp<std::int64_t>(static_cast<std::int64_t>(line) + lines, 0,
                                         static_cast<std::int64_t>(document_.lineCount()) - 1));
        next = column(target);
    }
    const auto end = static_cast<std::int32_t>(next);
    setSelection({extend ? selection.start : end, end}, true);
    revealOffset(next);
}

bool TextEditor::handleKey(const KeyEvent &event) {
    const bool ctrl = (event.modifiers & modifiers::kCtrl) != 0;
    const bool shift = (event.modifiers & modifiers::kShift) != 0;
    const bool editable = !options_.readOnly;
    if (event.action != KeyAction::down) {
        switch (event.keyCode) {
        case keys::kDel:
        case keys::kForwardDel:
        case keys::kDpadLeft:
        case keys::kDpadRight:
        case keys::kDpadUp:
        case keys::kDpadDown:
        case keys::kPageUp:
        case keys::kPageDown:
        case keys::kMoveHome:
        case keys::kMoveEnd:
        case keys::kEnter:
        case keys::kTab:
            return true;
        default:
            return event.codePoint != 0 || ctrl;
        }
    }
    const TextRange selection = selection_.peek();
    const TextRange range = ordered(selection);
    const auto caret = static_cast<std::uint32_t>(selection.end);
    const auto moveTo = [&](std::uint32_t offset) {
        const auto end = static_cast<std::int32_t>(offset);
        setSelection({shift ? selection.start : end, end});
        revealOffset(offset);
    };
    if (ctrl) {
        switch (event.keyCode) {
        case keys::kA:
            selectAll();
            return true;
        case keys::kC:
            copy();
            return true;
        case keys::kX:
            cut();
            return true;
        case keys::kV:
            paste();
            return true;
        case keys::kS:
            if (options_.onSave) {
                options_.onSave();
            }
            return true;
        case keys::kMoveHome:
            moveTo(0);
            return true;
        case keys::kMoveEnd:
            moveTo(document_.size());
            return true;
        default:
            return false;
        }
    }
    switch (event.keyCode) {
    case keys::kDel:
        if (editable && !range.collapsed()) {
            replaceSelection("");
        } else if (editable && caret > 0) {
            replace(previousCharacter(caret), caret, "");
        }
        return true;
    case keys::kForwardDel:
        if (editable && !range.collapsed()) {
            replaceSelection("");
        } else if (editable && caret < document_.size()) {
            replace(caret, nextCharacter(caret), "");
        }
        return true;
    case keys::kDpadLeft:
    case keys::kDpadRight: {
        const bool left = event.keyCode == keys::kDpadLeft;
        if (!shift && !range.collapsed()) {
            moveTo(static_cast<std::uint32_t>(left ? range.start : range.end));
        } else {
            moveTo(left ? previousCharacter(caret) : nextCharacter(caret));
        }
        return true;
    }
    case keys::kDpadUp:
    case keys::kDpadDown:
        moveVertically(event.keyCode == keys::kDpadUp ? -1 : 1, shift);
        return true;
    case keys::kPageUp:
    case keys::kPageDown: {
        const float lineDp = std::ceil(kFontSize * kLineHeight);
        const int page = std::max(1, static_cast<int>(list_->viewportExtent() / lineDp) - 1);
        moveVertically(event.keyCode == keys::kPageUp ? -page : page, shift);
        return true;
    }
    case keys::kMoveHome:
    case keys::kMoveEnd: {
        const std::size_t line = document_.lineOf(caret);
        moveTo(event.keyCode == keys::kMoveHome ? document_.lineStart(line) : document_.lineEnd(line));
        return true;
    }
    case keys::kEnter:
        if (editable) {
            replaceSelection("\n");
        }
        return true;
    case keys::kTab:
        if (editable) {
            replaceSelection("\t");
        }
        return true;
    default:
        break;
    }
    if (editable && event.codePoint >= 0x20 && event.codePoint != 0x7F) {
        std::string utf8;
        const std::uint32_t cp = event.codePoint;
        if (cp < 0x80) {
            utf8 += static_cast<char>(cp);
        } else if (cp < 0x800) {
            utf8 += static_cast<char>(0xC0 | (cp >> 6));
            utf8 += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            utf8 += static_cast<char>(0xE0 | (cp >> 12));
            utf8 += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            utf8 += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            utf8 += static_cast<char>(0xF0 | (cp >> 18));
            utf8 += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            utf8 += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            utf8 += static_cast<char>(0x80 | (cp & 0x3F));
        }
        replaceSelection(utf8);
        return true;
    }
    return false;
}

}  // namespace filepan::ui::components
