// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/text_input.hpp"

#include <algorithm>

namespace filepan::ui {

namespace {

bool isContinuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

// Moves offset back (or forward) to the start of a UTF-8 sequence.
std::int32_t alignBack(const std::string &text, std::int32_t offset) {
    while (offset > 0 && offset < static_cast<std::int32_t>(text.size()) && isContinuation(text[offset])) {
        --offset;
    }
    return offset;
}

std::int32_t alignForward(const std::string &text, std::int32_t offset) {
    while (offset < static_cast<std::int32_t>(text.size()) && isContinuation(text[offset])) {
        ++offset;
    }
    return offset;
}

std::int32_t clampOffset(const std::string &text, std::int32_t offset) {
    return std::clamp(offset, 0, static_cast<std::int32_t>(text.size()));
}

TextRange ordered(TextRange range, const std::string &text) {
    TextRange r{std::min(range.start, range.end), std::max(range.start, range.end)};
    r.start = alignBack(text, clampOffset(text, r.start));
    r.end = alignForward(text, clampOffset(text, r.end));
    return r;
}

TextEditingValue replace(const TextEditingValue &value, std::string_view text, int newCursorPosition,
                         bool composing) {
    TextEditingValue next = value;
    const TextRange target = value.composing.valid() ? ordered(value.composing, value.text)
                                                     : ordered(value.selection, value.text);
    next.text.replace(static_cast<std::size_t>(target.start), static_cast<std::size_t>(target.end - target.start),
                      text);
    const auto insertedEnd = static_cast<std::int32_t>(target.start + text.size());
    std::int32_t cursor = newCursorPosition > 0 ? insertedEnd + newCursorPosition - 1 : target.start + newCursorPosition;
    cursor = alignBack(next.text, clampOffset(next.text, cursor));
    next.selection = {cursor, cursor};
    next.composing = composing && !text.empty() ? TextRange{target.start, insertedEnd} : TextRange{};
    return next;
}

}  // namespace

void TextInputClient::commitText(std::string_view text, int newCursorPosition) {
    updateEditingValue(replace(editingValue(), text, newCursorPosition, false));
}

void TextInputClient::setComposingText(std::string_view text, int newCursorPosition) {
    updateEditingValue(replace(editingValue(), text, newCursorPosition, true));
}

void TextInputClient::setComposingRegion(int start, int end) {
    TextEditingValue next = editingValue();
    const TextRange region = ordered({start, end}, next.text);
    next.composing = region.collapsed() ? TextRange{} : region;
    updateEditingValue(next);
}

void TextInputClient::finishComposingText() {
    TextEditingValue next = editingValue();
    if (!next.composing.valid()) {
        return;
    }
    next.composing = {};
    updateEditingValue(next);
}

void TextInputClient::deleteSurroundingText(int before, int after) {
    TextEditingValue next = editingValue();
    const TextRange selection = ordered(next.selection, next.text);
    const std::int32_t afterEnd = alignForward(next.text, clampOffset(next.text, selection.end + std::max(0, after)));
    const std::int32_t beforeStart = alignBack(next.text, clampOffset(next.text, selection.start - std::max(0, before)));
    next.text.erase(static_cast<std::size_t>(selection.end), static_cast<std::size_t>(afterEnd - selection.end));
    next.text.erase(static_cast<std::size_t>(beforeStart), static_cast<std::size_t>(selection.start - beforeStart));
    const std::int32_t removedBefore = selection.start - beforeStart;
    next.selection = {beforeStart, selection.end - removedBefore};
    if (next.composing.valid()) {
        const TextRange c = next.composing;
        if (c.end <= beforeStart) {
            // Before the deleted text: unchanged.
        } else if (c.start >= afterEnd) {
            const std::int32_t removed = removedBefore + (afterEnd - selection.end);
            next.composing = {c.start - removed, c.end - removed};
        } else {
            next.composing = {};
        }
    }
    updateEditingValue(next);
}

void TextInputClient::setSelection(int start, int end) {
    TextEditingValue next = editingValue();
    next.selection = {alignBack(next.text, clampOffset(next.text, start)),
                      alignBack(next.text, clampOffset(next.text, end))};
    updateEditingValue(next);
}

void TextInputService::attach(TextInputClient *client) {
    if (client_ == client) {
        return;
    }
    client_ = client;
    if (client_ != nullptr && visible_ && platform_ != nullptr) {
        platform_->show(client_->inputConfig());
        platform_->updateEditingValue(client_->editingValue());
    }
}

void TextInputService::detach(TextInputClient *client) {
    if (client_ != client || client == nullptr) {
        return;
    }
    client_ = nullptr;
    requestHide();
}

void TextInputService::requestShow() {
    if (client_ == nullptr) {
        return;
    }
    visible_ = true;
    if (platform_ != nullptr) {
        platform_->show(client_->inputConfig());
        platform_->updateEditingValue(client_->editingValue());
    }
}

void TextInputService::requestHide() {
    if (!visible_) {
        return;
    }
    visible_ = false;
    if (platform_ != nullptr) {
        platform_->hide();
    }
}

void TextInputService::notifyValueChanged() {
    if (platform_ != nullptr && client_ != nullptr) {
        platform_->updateEditingValue(client_->editingValue());
    }
}

}  // namespace filepan::ui
