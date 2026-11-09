// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "filepan/ui/components/common.hpp"
#include "filepan/ui/components/surfaces.hpp"
#include "filepan/ui/node.hpp"

// Buttons (§7.1). Each is a real button for accessibility (role, label, disabled state, click),
// is focusable with a focus ring, activates on Enter/Space, gives haptic feedback, and moves 1 dp
// down and brightens while pressed. Disabled buttons desaturate and dim.
namespace filepan::ui::components {

enum class ButtonVariant : std::uint8_t {
    primary,  // A solid accent fill: one per view.
    glass,    // A panel, light text: secondary actions.
    outline,  // Transparent with an accent hairline and mono text: tertiary ("details").
    danger,   // The primary recipe in the destructive color.
    text,     // Just accent text: dialog actions.
};

struct ButtonProps {
    Prop<std::string> label;
    ButtonVariant variant = ButtonVariant::primary;
    std::string icon;  // Optional leading icon (ligature name).
    Prop<bool> enabled = true;
    std::function<void()> onTap;
    Haptic haptic = Haptic::tap;
    // Read instead of the label, e.g. "Delete 12 files" for "Delete".
    std::string accessibilityLabel;
};

class Button final : public Node {
public:
    static std::shared_ptr<Button> create(ButtonProps props);

    [[nodiscard]] const Pressable &pressable() const { return *pressable_; }
    [[nodiscard]] ButtonVariant variant() const { return variant_; }

protected:
    void paint(PaintContext &context) override;
    void paintOverlay(PaintContext &context) override;

private:
    [[nodiscard]] gfx::RoundedRect shape(const PaintContext &context) const;

    ButtonVariant variant_ = ButtonVariant::primary;
    std::shared_ptr<Pressable> pressable_;
    Prop<bool> enabled_;
};

enum class IconButtonStyle : std::uint8_t {
    glass,   // A glass circle: toolbars over content.
    filled,  // The accent recipe: "add".
    plain,   // No body until pressed: inside bars and the dock.
};

struct IconButtonProps {
    std::string icon;
    std::string label;  // Required: icon-only buttons need a spoken label.
    IconButtonStyle style = IconButtonStyle::glass;
    float size = 44;  // Visual diameter, dp; the hit target is at least 48.
    Prop<bool> enabled = true;
    // Highlights the icon in the accent (a current view mode).
    Prop<bool> selected = false;
    // The icon's filled form, in the usual ink: a star when the folder is a favorite.
    Prop<bool> filled = false;
    // Draws the icon in the destructive color (delete in the dock).
    bool destructive = false;
    std::function<void()> onTap;
    Haptic haptic = Haptic::tap;
};

class IconButton final : public Node {
public:
    static std::shared_ptr<IconButton> create(IconButtonProps props);

    [[nodiscard]] const Pressable &pressable() const { return *pressable_; }

protected:
    void paint(PaintContext &context) override;
    void paintOverlay(PaintContext &context) override;

private:
    IconButtonStyle style_ = IconButtonStyle::glass;
    std::shared_ptr<Pressable> pressable_;
    Prop<bool> enabled_;
    Prop<bool> selected_;
    Prop<bool> filled_;
};

}  // namespace filepan::ui::components
