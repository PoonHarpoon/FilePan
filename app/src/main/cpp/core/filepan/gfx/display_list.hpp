// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

#include "filepan/gfx/geometry.hpp"
#include "filepan/gfx/paint.hpp"
#include "filepan/gfx/text.hpp"

// A recorded frame: the UI framework records it on the UI thread and the renderer plays it back
// on the render thread. Commands draw in order, back to front, in the current transform and clip.
namespace filepan::gfx {

// An image the renderer owns, such as a thumbnail. 0 is never a valid image.
using ImageId = std::uint32_t;

// An image whose frames come from outside the renderer, such as a video decoder. 0 is never
// valid.
using ExternalImageId = std::uint32_t;

struct FillCommand {
    RoundedRect shape;
    Paint paint;
};

// A border drawn inside the shape's edge, like CSS borders.
struct StrokeCommand {
    RoundedRect shape;
    float width = 1;
    Paint paint;
};

struct ShadowCommand {
    RoundedRect shape;
    Shadow shadow;
};

enum class BlurSource : std::uint8_t {
    // Everything drawn so far this frame. Costs a blur every frame; use it for glass over
    // scrolling content.
    live,
    // Only the commands before BackgroundEndCommand. The renderer blurs that once and reuses
    // it until the background changes, so static glass panels are nearly free.
    background,
};

// Replaces the shape's area with a blurred (and saturated) copy of what's underneath, like
// CSS backdrop-filter: blur(radius) saturate(saturation). Glass panels draw this, then their fill.
struct BackdropBlurCommand {
    RoundedRect shape;
    float radius = 22;
    float saturation = 1;
    BlurSource source = BlurSource::live;
};

// Marks the end of the background (glows, grid, texture) for BlurSource::background.
struct BackgroundEndCommand {};

// Draws a shaped text at origin (the layout's top left). The text is shared so cached display
// lists stay cheap to copy.
struct TextCommand {
    std::shared_ptr<const ShapedText> text;
    Vec2 origin;
    Color color;
};

struct ImageCommand {
    ImageId image = 0;
    Rect destination;
    CornerRadii radii;
    float opacity = 1;
};

// The newest frame presented for an external image (a video), through
// vulkan::ExternalImageStore; nothing until one is. The frame is stretched to destination.
struct ExternalImageCommand {
    ExternalImageId image = 0;
    Rect destination;
    CornerRadii radii;
    float opacity = 1;
};

// Pushes state that later commands draw in, until the matching PopCommand.
struct PushClipCommand {
    RoundedRect shape;
};
struct PushTransformCommand {
    Transform transform;
};
// Draws the group into a layer and composites it with opacity, like CSS opacity on a container.
struct PushOpacityCommand {
    float opacity = 1;
};
struct PopCommand {};

class DisplayList;

// Plays back another display list in place, so the UI framework can cache a subtree's
// recording and reuse it across frames without copying it.
struct DisplayListCommand {
    std::shared_ptr<const DisplayList> list;
};

using DisplayCommand =
        std::variant<FillCommand, StrokeCommand, ShadowCommand, BackdropBlurCommand,
                     BackgroundEndCommand, TextCommand, ImageCommand, ExternalImageCommand,
                     PushClipCommand, PushTransformCommand, PushOpacityCommand, PopCommand,
                     DisplayListCommand>;

class DisplayList {
public:
    void fill(const RoundedRect &shape, const Paint &paint) { add(FillCommand{shape, paint}); }
    void fill(const Rect &rect, const Paint &paint) { fill(RoundedRect{rect, {}}, paint); }
    void stroke(const RoundedRect &shape, float width, const Paint &paint) {
        add(StrokeCommand{shape, width, paint});
    }
    void shadow(const RoundedRect &shape, const Shadow &shadow) { add(ShadowCommand{shape, shadow}); }
    void backdropBlur(const RoundedRect &shape, float radius, float saturation = 1,
                      BlurSource source = BlurSource::live) {
        add(BackdropBlurCommand{shape, radius, saturation, source});
    }
    void backgroundEnd() { add(BackgroundEndCommand{}); }
    void text(std::shared_ptr<const ShapedText> text, Vec2 origin, Color color) {
        add(TextCommand{std::move(text), origin, color});
    }
    void image(ImageId image, const Rect &destination, CornerRadii radii = {}, float opacity = 1) {
        add(ImageCommand{image, destination, radii, opacity});
    }
    void externalImage(ExternalImageId image, const Rect &destination, CornerRadii radii = {},
                       float opacity = 1) {
        add(ExternalImageCommand{image, destination, radii, opacity});
    }
    void pushClip(const RoundedRect &shape) { add(PushClipCommand{shape}); }
    void pushTransform(const Transform &transform) { add(PushTransformCommand{transform}); }
    void pushOpacity(float opacity) { add(PushOpacityCommand{opacity}); }
    void pop() { add(PopCommand{}); }
    void draw(std::shared_ptr<const DisplayList> list) { add(DisplayListCommand{std::move(list)}); }

    void add(DisplayCommand command) { commands_.push_back(std::move(command)); }
    void clear() { commands_.clear(); }

    [[nodiscard]] const std::vector<DisplayCommand> &commands() const { return commands_; }
    [[nodiscard]] bool empty() const { return commands_.empty(); }

private:
    std::vector<DisplayCommand> commands_;
};

}  // namespace filepan::gfx
