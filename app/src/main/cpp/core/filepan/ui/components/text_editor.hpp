// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "filepan/text/text_document.hpp"
#include "filepan/ui/components/common.hpp"
#include "filepan/ui/node.hpp"
#include "filepan/ui/text_input.hpp"
#include "filepan/ui/virtual_list.hpp"

// A text file's editor: monospace lines with numbers in a gutter, virtualized so a file of a
// hundred thousand lines scrolls as easily as ten.
namespace filepan::ui::components {

class TextEditor final : public Node, public TextInputClient {
public:
    struct Options {
        // Selecting and copying only; no keyboard.
        bool readOnly = false;
        // Long lines wrap at the width; without it they run on and the text scrolls sideways.
        bool wrap = true;
        // The text changed (by the user or the IME).
        std::function<void()> onChange;
        std::function<void(const std::string &)> onCopy;
        std::function<std::optional<std::string>()> onPaste;
        // Ctrl+S.
        std::function<void()> onSave;
    };

    static std::shared_ptr<TextEditor> create(text::TextDocument document, Options options);

    [[nodiscard]] const text::TextDocument &document() const { return document_; }
    // Bumps on every change to the text. Tracked.
    [[nodiscard]] const Signal<std::uint64_t> &revision() const { return revision_; }

    [[nodiscard]] bool readOnly() const { return options_.readOnly; }
    [[nodiscard]] bool wrap() const { return options_.wrap; }
    void setWrap(bool wrap);

    // The selection as anchor and caret, in bytes of the document's text; collapsed for a caret.
    // Tracked.
    [[nodiscard]] TextRange selection() const { return selection_(); }
    [[nodiscard]] TextRange peekSelection() const { return selection_.peek(); }
    void select(std::uint32_t anchor, std::uint32_t caret);
    void selectAll();
    [[nodiscard]] std::string selectedText() const;
    void copy();
    void cut();
    void paste();
    // Replaces the selection (or inserts at the caret), leaving the caret after the text.
    void replaceSelection(std::string_view text);

    // The line a document offset is on, scrolled into view.
    void revealOffset(std::uint32_t offset);
    [[nodiscard]] VirtualList &list() const { return *list_; }

    // TextInputClient. The IME sees a window of the text around the selection, not all of it:
    // it only ever reads near the caret, and a whole long file on every keystroke would be slow.
    [[nodiscard]] const TextEditingValue &editingValue() const override { return window_; }
    void updateEditingValue(const TextEditingValue &value) override;
    [[nodiscard]] TextInputConfig inputConfig() const override {
        return {.action = ImeAction::none, .multiline = true};
    }
    [[nodiscard]] gfx::Rect caretRect() const override;
    // Where the IME's window starts in the document.
    [[nodiscard]] std::uint32_t windowStart() const { return windowStart_; }

    // The document offset under a point in a line row's local px; the point may be outside the
    // row (a drag that went on to other lines).
    [[nodiscard]] std::uint32_t offsetAt(const Node &row, gfx::Vec2 local) const;

private:
    friend class TextLineRow;

    void replace(std::uint32_t start, std::uint32_t end, std::string_view text, bool fromIme = false);
    void setSelection(TextRange selection, bool keepGoal = false);
    void selectWordAt(std::uint32_t offset);
    bool handleKey(const KeyEvent &event);
    void moveVertically(int lines, bool extend);
    void repaintRows();
    // Keeps window_ in step with the text and selection, moving it when the selection nears its
    // edge (unless the IME is composing, which the move would disturb).
    void syncWindow(bool allowMove);
    [[nodiscard]] Node *rowFor(std::size_t line) const;
    [[nodiscard]] float gutterDp() const;
    [[nodiscard]] std::uint32_t previousCharacter(std::uint32_t offset) const;
    [[nodiscard]] std::uint32_t nextCharacter(std::uint32_t offset) const;

    Options options_;
    text::TextDocument document_;
    std::shared_ptr<VirtualList> list_;
    Signal<std::uint64_t> revision_{0};
    Signal<TextRange> selection_{TextRange{0, 0}};
    TextRange composing_;  // In the document, or invalid.
    // The x (row px) vertical moves keep to, through shorter lines; < 0 when none.
    float goalX_ = -1;
    float scrollX_ = 0;  // px, without wrapping.
    std::size_t gutterDigits_ = 0;

    std::uint32_t windowStart_ = 0;
    std::uint32_t windowEnd_ = 0;
    TextEditingValue window_;
};

}  // namespace filepan::ui::components
