// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "filepan/ui/components/common.hpp"
#include "filepan/ui/node.hpp"
#include "filepan/ui/text_input.hpp"

// The text field (§7.6): a visible label above, a sunken 16 dp-radius well, and a mono helper
// line below. Focus draws the accent border and a 4 dp ring at 25%.
namespace filepan::ui::components {

// The editable line inside a TextField: a TextInputClient that the IME and hardware keys edit.
// It paints the selection, the composing underline and the caret from the shaped text's caret
// rects, scrolls horizontally to keep the caret in view, and moves the caret where it's tapped
// or dragged.
//
// With more than one line it's a multi-line editor instead: the text wraps (long words between
// graphemes), Enter inserts a newline, it grows with its text up to lines, and past that it
// scrolls vertically to keep the caret in view.
class EditableText final : public Node, public TextInputClient {
public:
    struct Options {
        std::string placeholder;
        TextInputConfig config;
        // The most lines shown; more than 1 makes it multi-line.
        int lines = 1;
        std::function<void(const std::string &)> onChange;
        std::function<void()> onSubmit;
    };

    static std::shared_ptr<EditableText> create(Options options);

    // TextInputClient.
    [[nodiscard]] const TextEditingValue &editingValue() const override { return value_.peek(); }
    void updateEditingValue(const TextEditingValue &value) override;
    void performAction(ImeAction action) override;
    [[nodiscard]] TextInputConfig inputConfig() const override { return options_.config; }
    [[nodiscard]] gfx::Rect caretRect() const override;

    // Tracked.
    [[nodiscard]] const Signal<TextEditingValue> &value() const { return value_; }
    // Replaces the text from the app side (a rename prefill), without calling onChange. The IME
    // isn't told yet: the screen calls TextInputService::notifyValueChanged.
    void setText(std::string text, TextRange selection = {});
    // The shaped text as last painted; null before the first paint.
    [[nodiscard]] const std::shared_ptr<const gfx::ShapedText> &shaped() const { return shaped_; }
    // KeyboardType::password and numberPassword: drawn (and spoken) as a bullet per character.
    [[nodiscard]] bool obscured() const;
    [[nodiscard]] bool multiline() const { return options_.lines > 1; }
    // Places the caret at a point in the node's local px.
    void placeCaret(gfx::Vec2 local, bool extend = false);

    [[nodiscard]] bool hasMeasure() const override { return true; }
    gfx::Size measureContent(const LayoutContext &context, float maxWidth, float maxHeight) override;

protected:
    void paint(PaintContext &context) override;

private:
    // widthPx wraps a multi-line editor's text; a single line ignores it.
    const std::shared_ptr<const gfx::ShapedText> &shape(gfx::TextSystem &text, float density, float fontScale,
                                                        float widthPx);
    void paintSelection(PaintContext &context, const gfx::ShapedText &shaped, std::uint32_t from,
                        std::uint32_t to) const;
    // What's shaped: the text, or its mask. Offsets map between the two.
    [[nodiscard]] std::string displayText() const;
    [[nodiscard]] std::uint32_t toDisplay(std::uint32_t byte) const;
    [[nodiscard]] std::uint32_t fromDisplay(std::uint32_t byte) const;
    [[nodiscard]] std::uint32_t previousBoundary(std::uint32_t byte) const;
    [[nodiscard]] std::uint32_t nextBoundary(std::uint32_t byte) const;
    bool handleKey(const KeyEvent &event);

    Options options_;
    Signal<TextEditingValue> value_;
    std::shared_ptr<const gfx::ShapedText> shaped_;
    std::string shapedText_;
    float shapedDensity_ = 0;
    float shapedFontScale_ = 0;
    float shapedWidth_ = 0;
    float scrollX_ = 0;  // px
    float scrollY_ = 0;  // px, multi-line only
    FingerTracker finger_;  // px, local
    float dragY_ = 0;       // px, local: the line a caret drag moves along
};

struct TextFieldProps {
    std::string label;  // Always visible, above the well.
    std::string placeholder;
    std::string text;   // Initial text.
    std::string icon;   // Optional leading icon.
    Prop<std::string> helper;
    // When non-empty, replaces the helper and turns the field destructive.
    Prop<std::string> error;
    TextInputConfig config;
    std::function<void(const std::string &)> onChange;
    std::function<void()> onSubmit;
    // The most lines shown. More than 1 makes a multi-line field: its well grows with the text up
    // to that many lines and then scrolls, Enter inserts a newline (so onSubmit isn't called),
    // and the IME is told it's multi-line.
    int lines = 1;
};

class TextField final : public Node {
public:
    static std::shared_ptr<TextField> create(TextFieldProps props);

    [[nodiscard]] EditableText &editor() const { return *editor_; }
    [[nodiscard]] Node &well() const { return *well_; }

private:
    std::shared_ptr<EditableText> editor_;
    std::shared_ptr<Node> well_;
};

}  // namespace filepan::ui::components
