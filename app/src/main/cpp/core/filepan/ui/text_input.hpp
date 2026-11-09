// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "filepan/gfx/geometry.hpp"

// Text input: the editing state a text field exposes to the IME, and the service that connects
// the focused field to the platform's input method.
//
// Offsets are UTF-8 byte offsets into the text. The Android bridge converts to and from Java's
// UTF-16 offsets on its side of the JNI boundary.
namespace filepan::ui {

class Node;

struct TextRange {
    std::int32_t start = -1;
    std::int32_t end = -1;

    [[nodiscard]] constexpr bool valid() const { return start >= 0 && end >= start; }
    [[nodiscard]] constexpr bool collapsed() const { return start == end; }
    constexpr bool operator==(const TextRange &) const = default;
};

struct TextEditingValue {
    std::string text;
    TextRange selection{0, 0};   // The caret when collapsed.
    TextRange composing;         // The IME's in-progress region, or invalid.

    bool operator==(const TextEditingValue &) const = default;
};

enum class ImeAction : std::uint8_t { unspecified, none, go, search, send, next, done, previous };

// numberPassword: digits, hidden as they are typed, for a PIN.
enum class KeyboardType : std::uint8_t { text, number, phone, email, uri, password, visiblePassword, numberPassword };

struct TextInputConfig {
    KeyboardType keyboard = KeyboardType::text;
    ImeAction action = ImeAction::done;
    bool multiline = false;
    // No field asks for suggestions, autocorrection or autofill: the platform layer turns them all
    // off (ImeBridge), and the IME is asked not to learn from what is typed.

    constexpr bool operator==(const TextInputConfig &) const = default;
};

// Implemented by an editable text node. The InputConnection-style operations below are built on
// editingValue/updateEditingValue, so a field implements just those.
class TextInputClient {
public:
    virtual ~TextInputClient() = default;

    [[nodiscard]] virtual const TextEditingValue &editingValue() const = 0;
    // The IME (or a hardware key) edited the text.
    virtual void updateEditingValue(const TextEditingValue &value) = 0;
    virtual void performAction(ImeAction) {}
    [[nodiscard]] virtual TextInputConfig inputConfig() const { return {}; }
    // The caret in window px, for the IME's cursor anchor.
    [[nodiscard]] virtual gfx::Rect caretRect() const { return {}; }

    // Replaces the composing region (or the selection) with text and ends composing.
    // newCursorPosition is as in Android: > 0 is relative to the end of the inserted text (1 puts
    // the caret right after it), <= 0 relative to its start.
    void commitText(std::string_view text, int newCursorPosition = 1);
    // Like commitText, but the inserted text becomes the composing region.
    void setComposingText(std::string_view text, int newCursorPosition = 1);
    void setComposingRegion(int start, int end);
    void finishComposingText();
    // Deletes bytes before the selection start and after its end.
    void deleteSurroundingText(int before, int after);
    void setSelection(int start, int end);
};

// The platform's input method; the Android bridge implements it over InputMethodManager.
class TextInputPlatform {
public:
    virtual void show(const TextInputConfig &config) = 0;
    virtual void hide() = 0;
    // The text changed from the app side (not the IME); restart or update the IME's view.
    virtual void updateEditingValue(const TextEditingValue &value) = 0;

protected:
    ~TextInputPlatform() = default;
};

// Connects the focused TextInputClient to the platform. UiRoot owns one; focusing a node with a
// client attaches it and asks for the keyboard.
class TextInputService {
public:
    void setPlatform(TextInputPlatform *platform) { platform_ = platform; }

    void attach(TextInputClient *client);
    void detach(TextInputClient *client);
    [[nodiscard]] TextInputClient *client() const { return client_; }
    [[nodiscard]] bool visible() const { return visible_; }

    void requestShow();
    void requestHide();
    // The attached client changed its text itself (e.g. a rename prefill).
    void notifyValueChanged();

private:
    TextInputPlatform *platform_ = nullptr;
    TextInputClient *client_ = nullptr;
    bool visible_ = false;
};

}  // namespace filepan::ui
