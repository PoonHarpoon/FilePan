// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "filepan/gfx/display_list.hpp"
#include "filepan/ui/animation.hpp"
#include "filepan/ui/components/common.hpp"
#include "filepan/ui/node.hpp"

// Controls (§7.2-§7.5): switch, segmented control, slider, checkbox and meter ring.
//
// They're controlled: the value comes in as a Prop (usually bound to a signal) and user input
// calls onChange with the new value; the control shows whatever the Prop says. Raised parts
// (knobs, the selected segment) and sunken parts (tracks) follow the tactile rule (§2.4).
namespace filepan::ui::components {

// --- Switch (§7.2) ---

struct SwitchProps {
    Prop<bool> checked = false;
    std::function<void(bool)> onChange;
    std::string label;  // Spoken; the visible label is the row's.
    Prop<bool> enabled = true;
};

// A 52x32 dp track (the hit target is 48 dp tall). Off is a sunken dark track; on fills it with the
// solid accent. The knob slides on the
// overshooting curve over 240 ms, and snaps under reduced motion.
class Switch final : public Node {
public:
    static std::shared_ptr<Switch> create(SwitchProps props);

    // 0 off, 1 on; overshoots in between while animating.
    [[nodiscard]] float knobPosition() const { return knob_.peek(); }

protected:
    void paint(PaintContext &context) override;
    void paintOverlay(PaintContext &context) override;

private:
    AnimatedValue<float> knob_{0};
    Prop<bool> enabled_;
    std::shared_ptr<Pressable> pressable_;
};

// --- Segmented control (§7.3) ---

struct SegmentedProps {
    std::vector<std::string> options;
    Prop<std::size_t> selected = std::size_t{0};
    std::function<void(std::size_t)> onSelect;
    std::string label;  // Spoken name of the group.
};

// A sunken pill; the selected segment is a raised accent pill that springs between segments.
// Each segment is a checkable button in semantics.
class SegmentedControl final : public Node {
public:
    static std::shared_ptr<SegmentedControl> create(SegmentedProps props);

    [[nodiscard]] Node &segment(std::size_t index) const { return *children()[index]; }

protected:
    void paint(PaintContext &context) override;

private:
    AnimatedValue<float> indicator_{0};
};

// --- Slider (§7.4) ---

struct SliderProps {
    Prop<float> value = 0.0f;  // 0-1.
    std::function<void(float)> onChange;
    std::string label;
    // Accessibility and keyboard step.
    float step = 0.05f;
    // The spoken value; a percentage by default.
    std::function<std::string(float)> format;
    Prop<bool> enabled = true;
    // Draws the track instead of the sunken well and its fill, e.g. a color picker's hue
    // spectrum. Given the track's pill in px.
    std::function<void(gfx::DisplayList &list, const gfx::RoundedRect &track, float density)> paintTrack;
};

// A 12 dp sunken track with a solid accent fill and a 28 dp flat thumb. Tap or drag anywhere on
// it; D-pad left/right and the accessibility scroll actions step it.
class Slider final : public Node {
public:
    static std::shared_ptr<Slider> create(SliderProps props);

protected:
    void paint(PaintContext &context) override;
    void paintOverlay(PaintContext &context) override;

private:
    void setFromX(float xPx);

    Prop<float> value_;
    std::function<void(float)> onChange_;
    std::function<void(gfx::DisplayList &, const gfx::RoundedRect &, float)> paintTrack_;
    FingerTracker finger_;
};

// --- Checkbox ---

struct CheckboxProps {
    Prop<bool> checked = false;
    std::function<void(bool)> onChange;
    // Shown to the right when set, and the whole row toggles; otherwise just spoken.
    std::string text;
    std::string label;
    Prop<bool> enabled = true;
};

// A 22 dp square with a 4 dp radius: sunken when off, a solid accent stamp with a check when on.
class Checkbox final : public Node {
public:
    static std::shared_ptr<Checkbox> create(CheckboxProps props);

protected:
    void paintOverlay(PaintContext &context) override;
};

// --- Meter ring (§7.5) ---

struct MeterRingProps {
    Prop<float> value = 0.0f;  // 0-1.
    Prop<std::string> numeral;  // "62" (defaults to the percentage).
    Prop<std::string> unit = std::string("%");
    std::string label;          // "Internal storage used".
    float size = 132;           // dp.
};

// A conic gradient fill to the value around a sunken track, with a flat dark disc in the
// middle carrying a condensed numeral and a mono unit.
class MeterRing final : public Node {
public:
    static std::shared_ptr<MeterRing> create(MeterRingProps props);

protected:
    void paint(PaintContext &context) override;

private:
    Prop<float> value_;
    float size_ = 132;
};

}  // namespace filepan::ui::components
