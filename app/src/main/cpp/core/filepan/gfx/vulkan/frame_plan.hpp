// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <variant>
#include <vector>

#include "filepan/gfx/display_list.hpp"
#include "filepan/gfx/vulkan/glyph_atlas.hpp"

// Internal: turns a display list into a frame plan, which is instance data for the GPU and a list
// of operations (draws, layers, blurs) the renderer records. Building it touches no Vulkan, so
// the batching and culling logic is plain C++.
namespace filepan::gfx::vulkan::detail {

// Kept in sync with shaders/common.glsl.
enum class InstanceKind : std::uint32_t {
    fill = 0,
    stroke = 1,
    dropShadow = 2,
    insetShadow = 3,
    glyph = 4,
    colorGlyph = 5,
    image = 6,
    backdrop = 7,
    layer = 8,
};

enum class PaintKind : std::uint32_t {
    solid = 0,
    linear = 1,
    radial = 2,
    conic = 3,
    stripes = 4,
};

// One instanced quad: std430 layout of `Instance` in shaders/common.glsl.
struct GpuInstance {
    float rect[4];       // x, y, width, height in the instance's local space.
    float radii[4];      // Clamped corner radii; texture coordinates for glyphs.
    float transform[4];  // a, b, c, d: local to target pixels.
    float translate[2];
    std::uint32_t header;  // kind | paint << 8 | stop count << 12 | flags << 16.
    std::uint32_t clip;    // Index into the clip buffer; 0 means unclipped.
    float params[4];       // Per kind: paint geometry, shadow or blur parameters.
    std::uint32_t colors[4];   // Non-premultiplied RGBA8 colors (gradient stops).
    std::uint32_t offsets[2];  // Stop offsets as unorm16 pairs; blur clamp rect for backdrops.
    float extra[2];            // Stroke width or opacity.
};
static_assert(sizeof(GpuInstance) == 112);

// A rounded-rect clip: std430 layout of `Clip` in shaders/common.glsl.
struct GpuClip {
    float inverse[4];  // Target pixels to the clip's local space.
    float inverseTranslate[2];
    float scale;  // Local units to pixels, for antialiasing.
    std::uint32_t parent;
    float rect[4];
    float radii[4];
};
static_assert(sizeof(GpuClip) == 64);

struct IRect {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;

    [[nodiscard]] bool empty() const { return x1 <= x0 || y1 <= y0; }
    [[nodiscard]] int width() const { return x1 - x0; }
    [[nodiscard]] int height() const { return y1 - y0; }
    bool operator==(const IRect &) const = default;
};

// A dual-Kawase blur: a blit to half size, levels - 1 more downsampling passes, levels - 1
// upsampling passes back to half size, and a last upsampling tap pattern where it's composited.
struct BlurSetup {
    int levels = 1;
    float offset = 0;  // Tap offset in source texels.
    // regions[i] is the area at level i (level 0 is the target); regions[0] is aligned so
    // every level is an exact half of the one above it.
    std::vector<IRect> regions;
};

// Picks levels and offset so the blur's standard deviation matches sigma (in target pixels),
// and the regions covering area at each level.
BlurSetup makeBlurSetup(float sigma, IRect area, int targetWidth, int targetHeight);

// What a draw samples besides the atlas.
struct Aux {
    // external draws go through their own pipeline, so they never batch with other kinds.
    enum class Kind : std::uint8_t { none, image, layer, liveBlur, backgroundBlur, external };
    Kind kind = Kind::none;
    std::uint32_t index = 0;  // ImageId, layer depth, background blur entry or ExternalImageId.
    bool operator==(const Aux &) const = default;
};

struct DrawOp {
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    Aux aux;
};
// Later draws go into layer image `layer` (by nesting depth), cleared within bounds.
struct BeginLayerOp {
    std::uint32_t layer = 0;
    IRect bounds;
};
// Back to the parent; the DrawOp after it composites the layer.
struct EndLayerOp {
    std::uint32_t layer = 0;
};
// Blurs the current destination into the live blur image for the DrawOps after it.
struct LiveBlurOp {
    BlurSetup setup;
};
using PlanOp = std::variant<DrawOp, BeginLayerOp, EndLayerOp, LiveBlurOp>;

struct BackgroundBlur {
    float sigma = 0;
    BlurSetup setup;
};

struct FramePlan {
    std::vector<GpuInstance> instances;
    std::vector<GpuClip> clips;  // clips[0] is unused.
    std::vector<PlanOp> ops;
    std::uint32_t maxLayerDepth = 0;
    std::uint32_t drawCalls = 0;
    std::uint32_t liveBlurs = 0;

    // Set when the list had a BackgroundEndCommand at the top level of layers: ops before
    // backgroundOps draw the background, which hashes to backgroundHash.
    std::optional<std::size_t> backgroundOps;
    std::uint32_t backgroundLayerDepth = 0;  // Layers the background itself uses.
    std::uint64_t backgroundHash = 0;
    std::vector<BackgroundBlur> backgroundBlurs;  // Distinct blurs of the background used.

    void clear();
};

// Builds plans. Owned by the renderer so its scratch storage is reused across frames.
class PlanBuilder {
public:
    // An uploaded image's version, which changes when its pixels do; 0 if it can't be drawn.
    using ImageResident = std::function<std::uint32_t(ImageId)>;
    // An external image's current frame: the part of it to sample (u0, v0, u1, v1) and a
    // version that changes with every frame; nothing if it has none that can be drawn.
    struct ExternalView {
        float uv[4] = {0, 0, 1, 1};
        std::uint32_t version = 0;
    };
    using ExternalResident = std::function<std::optional<ExternalView>(ExternalImageId)>;

    PlanBuilder(TextSystem &text, GlyphAtlas &atlas, ImageResident imageResident,
                ExternalResident externalResident = {});

    void build(const DisplayList &list, int width, int height, Color clearColor,
               std::uint64_t frame, FramePlan &plan);

private:
    struct State {
        Transform transform;
        std::uint32_t clip = 0;
        float alpha = 1;  // Opacity folded into colors instead of a layer.
    };
    enum class Push : std::uint8_t { clip, transform, foldedOpacity, layerOpacity, none };
    struct StackEntry {
        Push kind = Push::none;
        State saved;
        std::size_t layerOp = 0;
        float opacity = 1;
    };
    struct ClipInfo {
        Rect outer;    // Device-space bounds of everything the clip chain lets through.
        Rect inner;    // Device-space rect the chain doesn't touch at all; may be empty.
    };

    void play(const std::vector<DisplayCommand> &commands);
    void command(const std::vector<DisplayCommand> &commands, std::size_t index);
    void hash(const DisplayCommand &command);
    void pop();
    bool canFoldOpacity(const std::vector<DisplayCommand> &commands, std::size_t index) const;

    // Fills in the transform and clip of an instance, culls it, and batches it. bounds is the
    // instance's local-space extent including antialiasing, shadow blur and so on.
    void emit(GpuInstance &instance, const Rect &bounds, Aux aux = {});
    void emitDevice(const GpuInstance &instance, const Rect &deviceBounds, Aux aux);
    [[nodiscard]] Rect deviceBounds(const Rect &local) const;
    [[nodiscard]] float scale() const;

    void fill(const RoundedRect &shape, const Paint &paint, InstanceKind kind, float strokeWidth);
    void shadow(const ShadowCommand &command);
    void backdrop(const BackdropBlurCommand &command);
    void text(const TextCommand &command);
    void image(const ImageCommand &command);
    void externalImage(const ExternalImageCommand &command);
    void pushClip(const RoundedRect &shape);
    void pushLayer(float opacity);
    void popLayer(const StackEntry &entry);

    TextSystem &text_;
    GlyphAtlas &atlas_;
    ImageResident imageResident_;
    ExternalResident externalResident_;

    FramePlan *plan_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    std::uint64_t frame_ = 0;
    State state_;
    std::vector<StackEntry> stack_;
    std::vector<ClipInfo> clipInfo_;
    std::vector<Rect> layerBounds_;  // Per open layer, the device area drawn into it.
    std::uint32_t layerDepth_ = 0;
    bool backgroundDone_ = false;
    bool noMerge_ = false;  // The next instance starts a new draw.
    std::uint64_t hash_ = 0;
};

}  // namespace filepan::gfx::vulkan::detail
