// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/gfx/vulkan/frame_plan.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

namespace filepan::gfx::vulkan::detail {

namespace {

template <typename... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

// Longest stretch of commands canFoldOpacity() looks through before giving up and using a layer.
constexpr int kFoldScanLimit = 64;
// Largest tap offset makeBlurSetup() uses before adding a level; bigger offsets leave gaps.
constexpr float kMaxBlurOffset = 1.75f;
constexpr int kMaxBlurLevels = 6;

std::uint32_t packColor(const Color &color, float alpha) {
    const auto quantize = [](float value) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255));
    };
    return quantize(color.r) | quantize(color.g) << 8 | quantize(color.b) << 16 |
           quantize(color.a * alpha) << 24;
}

std::uint32_t packUnorm16(float low, float high) {
    const auto quantize = [](float value) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 65535));
    };
    return quantize(low) | quantize(high) << 16;
}

// Scales radii down, like CSS, so adjacent corners never overlap; huge radii give pills.
CornerRadii clampRadii(const RoundedRect &shape) {
    CornerRadii r{std::max(shape.radii.topLeft, 0.0f), std::max(shape.radii.topRight, 0.0f),
                  std::max(shape.radii.bottomRight, 0.0f), std::max(shape.radii.bottomLeft, 0.0f)};
    const float width = std::max(shape.rect.width, 0.0f);
    const float height = std::max(shape.rect.height, 0.0f);
    float factor = 1;
    const auto limit = [&factor](float side, float a, float b) {
        if (a + b > side) {
            factor = std::min(factor, side / (a + b));
        }
    };
    limit(width, r.topLeft, r.topRight);
    limit(width, r.bottomLeft, r.bottomRight);
    limit(height, r.topLeft, r.bottomLeft);
    limit(height, r.topRight, r.bottomRight);
    return {r.topLeft * factor, r.topRight * factor, r.bottomRight * factor,
            r.bottomLeft * factor};
}

void setShape(GpuInstance &instance, const RoundedRect &shape) {
    const CornerRadii radii = clampRadii(shape);
    instance.rect[0] = shape.rect.x;
    instance.rect[1] = shape.rect.y;
    instance.rect[2] = shape.rect.width;
    instance.rect[3] = shape.rect.height;
    instance.radii[0] = radii.topLeft;
    instance.radii[1] = radii.topRight;
    instance.radii[2] = radii.bottomRight;
    instance.radii[3] = radii.bottomLeft;
}

std::uint32_t header(InstanceKind kind, PaintKind paint = PaintKind::solid,
                     std::uint32_t stops = 0) {
    return static_cast<std::uint32_t>(kind) | static_cast<std::uint32_t>(paint) << 8 |
           stops << 12;
}

void setStops(GpuInstance &instance, const GradientStops &stops, float alpha) {
    const std::uint32_t count = std::min<std::uint32_t>(stops.count, GradientStops::kMaxStops);
    float offsets[GradientStops::kMaxStops] = {};
    for (std::uint32_t i = 0; i < count; ++i) {
        instance.colors[i] = packColor(stops.stops[i].color, alpha);
        offsets[i] = stops.stops[i].offset;
    }
    instance.offsets[0] = packUnorm16(offsets[0], offsets[1]);
    instance.offsets[1] = packUnorm16(offsets[2], offsets[3]);
    instance.header |= count << 12;
}

bool contains(const Rect &outer, const Rect &inner) {
    return !outer.empty() && inner.x >= outer.x && inner.y >= outer.y &&
           inner.right() <= outer.right() && inner.bottom() <= outer.bottom();
}

IRect outward(const Rect &rect) {
    return {static_cast<int>(std::floor(rect.x)), static_cast<int>(std::floor(rect.y)),
            static_cast<int>(std::ceil(rect.right())), static_cast<int>(std::ceil(rect.bottom()))};
}

Rect toRect(const IRect &rect) {
    return Rect::fromEdges(static_cast<float>(rect.x0), static_cast<float>(rect.y0),
                           static_cast<float>(rect.x1), static_cast<float>(rect.y1));
}

int levelSize(int size, int level) { return (size + (1 << level) - 1) >> level; }

class Hasher {
public:
    void add(std::uint64_t value) {
        hash_ ^= value + 0x9e3779b97f4a7c15ull + (hash_ << 6) + (hash_ >> 2);
    }
    void add(float value) { add(static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(value))); }
    void add(Vec2 value) {
        add(value.x);
        add(value.y);
    }
    void add(const Rect &rect) {
        add(rect.x);
        add(rect.y);
        add(rect.width);
        add(rect.height);
    }
    void add(const CornerRadii &radii) {
        add(radii.topLeft);
        add(radii.topRight);
        add(radii.bottomRight);
        add(radii.bottomLeft);
    }
    void add(const RoundedRect &shape) {
        add(shape.rect);
        add(shape.radii);
    }
    void add(const Color &color) {
        add(color.r);
        add(color.g);
        add(color.b);
        add(color.a);
    }
    void add(const GradientStops &stops) {
        add(static_cast<std::uint64_t>(stops.count));
        for (std::uint8_t i = 0; i < stops.count && i < GradientStops::kMaxStops; ++i) {
            add(stops.stops[i].offset);
            add(stops.stops[i].color);
        }
    }
    void add(const Paint &paint) {
        add(static_cast<std::uint64_t>(paint.index()));
        std::visit(Overloaded{
                           [this](const Color &color) { add(color); },
                           [this](const LinearGradient &g) {
                               add(g.from);
                               add(g.to);
                               add(g.stops);
                           },
                           [this](const RadialGradient &g) {
                               add(g.center);
                               add(g.radius);
                               add(g.stops);
                           },
                           [this](const ConicGradient &g) {
                               add(g.center);
                               add(g.startAngle);
                               add(g.stops);
                           },
                           [this](const Stripes &s) {
                               add(s.angle);
                               add(s.width);
                               add(s.period);
                               add(s.phase);
                               add(s.color);
                           },
                   },
                   paint);
    }
    void add(const Transform &t) {
        add(t.a);
        add(t.b);
        add(t.c);
        add(t.d);
        add(t.tx);
        add(t.ty);
    }

    [[nodiscard]] std::uint64_t value() const { return hash_; }
    void reset(std::uint64_t value) { hash_ = value; }

private:
    std::uint64_t hash_ = 0;
};

}  // namespace

BlurSetup makeBlurSetup(float sigma, IRect area, int targetWidth, int targetHeight) {
    // Each pass adds variance, in level-0 pixels, for texel size s = 2^i at level i:
    //   blit to level 1: 1/4;  downsampling from level i: s^2 (1/4 + o^2 / 2);
    //   upsampling into level i: s^2 (3/4 + 4 o^2 / 3).
    // With A = sum of s^2 over levels 0..n-1 and B = A - 1 (downsampling skips level 0, which
    // the blit replaces), sigma^2 = 1/4 + B (1/4 + o^2/2) + A (3/4 + 4 o^2 / 3).
    BlurSetup setup;
    const float variance = sigma * sigma;
    for (int levels = 1; levels <= kMaxBlurLevels; ++levels) {
        const float a = static_cast<float>((1 << (2 * levels)) - 1) / 3;
        const float b = a - 1;
        const float base = 0.25f + b * 0.25f + a * 0.75f;
        const float perOffset = b * 0.5f + a * 4.0f / 3.0f;
        const float offsetSquared = (variance - base) / perOffset;
        if (offsetSquared <= kMaxBlurOffset * kMaxBlurOffset || levels == kMaxBlurLevels) {
            setup.levels = levels;
            setup.offset = std::sqrt(std::max(offsetSquared, 0.0f));
            break;
        }
    }
    const int align = 1 << setup.levels;
    const auto down = [align](int value) { return value >= 0 ? value / align * align : 0; };
    const auto up = [align](int value) { return (value + align - 1) / align * align; };
    IRect region{down(std::max(area.x0, 0)), down(std::max(area.y0, 0)),
                 std::min(up(area.x1), targetWidth), std::min(up(area.y1), targetHeight)};
    setup.regions.push_back(region);
    for (int level = 1; level <= setup.levels; ++level) {
        const IRect &above = setup.regions.back();
        setup.regions.push_back({above.x0 / 2, above.y0 / 2,
                                 std::min((above.x1 + 1) / 2, levelSize(targetWidth, level)),
                                 std::min((above.y1 + 1) / 2, levelSize(targetHeight, level))});
    }
    return setup;
}

void FramePlan::clear() {
    instances.clear();
    clips.clear();
    ops.clear();
    maxLayerDepth = 0;
    drawCalls = 0;
    liveBlurs = 0;
    backgroundOps.reset();
    backgroundLayerDepth = 0;
    backgroundHash = 0;
    backgroundBlurs.clear();
}

PlanBuilder::PlanBuilder(TextSystem &text, GlyphAtlas &atlas, ImageResident imageResident,
                         ExternalResident externalResident)
    : text_(text),
      atlas_(atlas),
      imageResident_(std::move(imageResident)),
      externalResident_(std::move(externalResident)) {}

void PlanBuilder::build(const DisplayList &list, int width, int height, Color clearColor,
                        std::uint64_t frame, FramePlan &plan) {
    plan.clear();
    plan_ = &plan;
    width_ = width;
    height_ = height;
    frame_ = frame;
    state_ = {};
    stack_.clear();
    layerBounds_.clear();
    layerDepth_ = 0;
    backgroundDone_ = false;
    noMerge_ = false;

    const Rect target{0, 0, static_cast<float>(width), static_cast<float>(height)};
    plan.clips.push_back({});
    clipInfo_.clear();
    clipInfo_.push_back({target, target});

    Hasher hasher;
    hasher.add(static_cast<std::uint64_t>(width) << 32 | static_cast<std::uint32_t>(height));
    hasher.add(clearColor);
    hash_ = hasher.value();

    play(list.commands());
    while (!stack_.empty()) {
        pop();
    }
    plan_ = nullptr;
}

void PlanBuilder::play(const std::vector<DisplayCommand> &commands) {
    for (std::size_t i = 0; i < commands.size(); ++i) {
        command(commands, i);
    }
}

void PlanBuilder::hash(const DisplayCommand &command) {
    Hasher hasher;
    hasher.reset(hash_);
    hasher.add(static_cast<std::uint64_t>(command.index()));
    std::visit(Overloaded{
                       [&](const FillCommand &c) {
                           hasher.add(c.shape);
                           hasher.add(c.paint);
                       },
                       [&](const StrokeCommand &c) {
                           hasher.add(c.shape);
                           hasher.add(c.width);
                           hasher.add(c.paint);
                       },
                       [&](const ShadowCommand &c) {
                           hasher.add(c.shape);
                           hasher.add(c.shadow.offset);
                           hasher.add(c.shadow.blur);
                           hasher.add(c.shadow.spread);
                           hasher.add(c.shadow.color);
                           hasher.add(static_cast<std::uint64_t>(c.shadow.inset));
                       },
                       [&](const BackdropBlurCommand &c) {
                           hasher.add(c.shape);
                           hasher.add(c.radius);
                           hasher.add(c.saturation);
                           hasher.add(static_cast<std::uint64_t>(c.source));
                       },
                       [&](const BackgroundEndCommand &) {},
                       [&](const TextCommand &c) {
                           // Shaped text is immutable and shared, so its identity is its content.
                           hasher.add(static_cast<std::uint64_t>(
                                   reinterpret_cast<std::uintptr_t>(c.text.get())));
                           hasher.add(c.origin);
                           hasher.add(c.color);
                       },
                       [&](const ImageCommand &c) {
                           hasher.add(static_cast<std::uint64_t>(c.image));
                           hasher.add(static_cast<std::uint64_t>(imageResident_(c.image)));
                           hasher.add(c.destination);
                           hasher.add(c.radii);
                           hasher.add(c.opacity);
                       },
                       [&](const ExternalImageCommand &c) {
                           hasher.add(static_cast<std::uint64_t>(c.image));
                           const auto view = externalResident_ ? externalResident_(c.image) : std::nullopt;
                           hasher.add(static_cast<std::uint64_t>(view ? view->version : 0));
                           hasher.add(c.destination);
                           hasher.add(c.radii);
                           hasher.add(c.opacity);
                       },
                       [&](const PushClipCommand &c) { hasher.add(c.shape); },
                       [&](const PushTransformCommand &c) { hasher.add(c.transform); },
                       [&](const PushOpacityCommand &c) { hasher.add(c.opacity); },
                       [&](const PopCommand &) {},
                       // The nested commands hash themselves as they're played.
                       [&](const DisplayListCommand &) {},
               },
               command);
    hash_ = hasher.value();
}

void PlanBuilder::command(const std::vector<DisplayCommand> &commands, std::size_t index) {
    const DisplayCommand &command = commands[index];
    if (!backgroundDone_) {
        hash(command);
    }
    std::visit(Overloaded{
                       [&](const FillCommand &c) { fill(c.shape, c.paint, InstanceKind::fill, 0); },
                       [&](const StrokeCommand &c) {
                           if (c.width > 0) {
                               fill(c.shape, c.paint, InstanceKind::stroke, c.width);
                           }
                       },
                       [&](const ShadowCommand &c) { shadow(c); },
                       [&](const BackdropBlurCommand &c) { backdrop(c); },
                       [&](const BackgroundEndCommand &) {
                           // A background that ends inside a layer can't be drawn on its own.
                           if (!backgroundDone_ && layerDepth_ == 0) {
                               backgroundDone_ = true;
                               plan_->backgroundOps = plan_->ops.size();
                               plan_->backgroundLayerDepth = plan_->maxLayerDepth;
                               plan_->backgroundHash = hash_;
                               // Later instances must not join the background's last draw.
                               noMerge_ = true;
                           }
                       },
                       [&](const TextCommand &c) { text(c); },
                       [&](const ImageCommand &c) { image(c); },
                       [&](const ExternalImageCommand &c) { externalImage(c); },
                       [&](const PushClipCommand &c) { pushClip(c.shape); },
                       [&](const PushTransformCommand &c) {
                           stack_.push_back({Push::transform, state_, 0, 1});
                           state_.transform = state_.transform * c.transform;
                       },
                       [&](const PushOpacityCommand &c) {
                           const float opacity = std::clamp(c.opacity, 0.0f, 1.0f);
                           if (opacity >= 1 || opacity <= 0 || canFoldOpacity(commands, index)) {
                               stack_.push_back({Push::foldedOpacity, state_, 0, opacity});
                               state_.alpha *= opacity;
                           } else {
                               pushLayer(opacity);
                           }
                       },
                       [&](const PopCommand &) { pop(); },
                       [&](const DisplayListCommand &c) {
                           if (c.list) {
                               const std::size_t depth = stack_.size();
                               play(c.list->commands());
                               // Pops whatever the nested list left pushed.
                               while (stack_.size() > depth) {
                                   pop();
                               }
                           }
                       },
               },
               command);
}

bool PlanBuilder::canFoldOpacity(const std::vector<DisplayCommand> &commands,
                                 std::size_t index) const {
    // A group with at most one drawing command looks the same with its opacity folded into
    // that command's colors, without the cost of a layer.
    int depth = 0;
    int draws = 0;
    for (std::size_t i = index + 1; i < commands.size(); ++i) {
        if (static_cast<int>(i - index) > kFoldScanLimit) {
            return false;
        }
        const DisplayCommand &command = commands[i];
        if (std::holds_alternative<PopCommand>(command)) {
            if (depth == 0) {
                return true;
            }
            --depth;
        } else if (std::holds_alternative<PushClipCommand>(command) ||
                   std::holds_alternative<PushTransformCommand>(command) ||
                   std::holds_alternative<PushOpacityCommand>(command)) {
            ++depth;
        } else if (std::holds_alternative<BackdropBlurCommand>(command) ||
                   std::holds_alternative<DisplayListCommand>(command)) {
            return false;
        } else if (!std::holds_alternative<BackgroundEndCommand>(command)) {
            if (++draws > 1) {
                return false;
            }
        }
    }
    return true;
}

void PlanBuilder::pop() {
    if (stack_.empty()) {
        return;
    }
    const StackEntry entry = stack_.back();
    stack_.pop_back();
    if (entry.kind == Push::layerOpacity) {
        popLayer(entry);
    }
    state_ = entry.saved;
}

float PlanBuilder::scale() const {
    const Transform &t = state_.transform;
    return std::sqrt(std::abs(t.a * t.d - t.b * t.c));
}

Rect PlanBuilder::deviceBounds(const Rect &local) const {
    const Transform &t = state_.transform;
    if (t.isTranslation()) {
        return local.translated({t.tx, t.ty});
    }
    const Vec2 corners[] = {t.apply({local.x, local.y}), t.apply({local.right(), local.y}),
                            t.apply({local.x, local.bottom()}),
                            t.apply({local.right(), local.bottom()})};
    float left = corners[0].x;
    float top = corners[0].y;
    float right = left;
    float bottom = top;
    for (const Vec2 &corner : corners) {
        left = std::min(left, corner.x);
        top = std::min(top, corner.y);
        right = std::max(right, corner.x);
        bottom = std::max(bottom, corner.y);
    }
    return Rect::fromEdges(left, top, right, bottom);
}

void PlanBuilder::emit(GpuInstance &instance, const Rect &bounds, Aux aux) {
    const Transform &t = state_.transform;
    instance.transform[0] = t.a;
    instance.transform[1] = t.b;
    instance.transform[2] = t.c;
    instance.transform[3] = t.d;
    instance.translate[0] = t.tx;
    instance.translate[1] = t.ty;
    instance.clip = state_.clip;

    const Rect device = deviceBounds(bounds);
    const ClipInfo &clip = clipInfo_[state_.clip];
    const Rect visible = device.intersect(clip.outer);
    if (visible.empty()) {
        return;
    }
    // Most instances, like rows well inside a list's viewport, needn't evaluate the clip.
    if (state_.clip != 0 && contains(clip.inner, device)) {
        instance.clip = 0;
    }
    emitDevice(instance, visible, aux);
}

void PlanBuilder::emitDevice(const GpuInstance &instance, const Rect &deviceBounds, Aux aux) {
    FramePlan &plan = *plan_;
    const auto index = static_cast<std::uint32_t>(plan.instances.size());
    plan.instances.push_back(instance);
    if (!layerBounds_.empty()) {
        layerBounds_.back() = layerBounds_.back().unite(deviceBounds);
    }
    if (!noMerge_ && !plan.ops.empty()) {
        if (auto *draw = std::get_if<DrawOp>(&plan.ops.back());
            draw != nullptr && draw->first + draw->count == index &&
            (draw->aux == aux ||
             ((aux.kind == Aux::Kind::none || draw->aux.kind == Aux::Kind::none) &&
              aux.kind != Aux::Kind::external && draw->aux.kind != Aux::Kind::external))) {
            ++draw->count;
            if (draw->aux.kind == Aux::Kind::none) {
                draw->aux = aux;
            }
            return;
        }
    }
    noMerge_ = false;
    plan.ops.push_back(DrawOp{index, 1, aux});
    ++plan.drawCalls;
}

void PlanBuilder::fill(const RoundedRect &shape, const Paint &paint, InstanceKind kind,
                       float strokeWidth) {
    if (shape.rect.empty()) {
        return;
    }
    GpuInstance instance{};
    setShape(instance, shape);
    instance.extra[0] = strokeWidth;
    const float alpha = state_.alpha;
    bool visible = true;
    std::visit(Overloaded{
                       [&](const Color &color) {
                           instance.header = header(kind, PaintKind::solid);
                           instance.colors[0] = packColor(color, alpha);
                           visible = color.a * alpha > 0;
                       },
                       [&](const LinearGradient &g) {
                           instance.header = header(kind, PaintKind::linear);
                           instance.params[0] = g.from.x;
                           instance.params[1] = g.from.y;
                           instance.params[2] = g.to.x;
                           instance.params[3] = g.to.y;
                           setStops(instance, g.stops, alpha);
                       },
                       [&](const RadialGradient &g) {
                           instance.header = header(kind, PaintKind::radial);
                           instance.params[0] = g.center.x;
                           instance.params[1] = g.center.y;
                           instance.params[2] = g.radius;
                           setStops(instance, g.stops, alpha);
                       },
                       [&](const ConicGradient &g) {
                           instance.header = header(kind, PaintKind::conic);
                           instance.params[0] = g.center.x;
                           instance.params[1] = g.center.y;
                           instance.params[2] = g.startAngle;
                           setStops(instance, g.stops, alpha);
                       },
                       [&](const Stripes &s) {
                           instance.header = header(kind, PaintKind::stripes);
                           instance.params[0] = s.angle;
                           instance.params[1] = s.width;
                           instance.params[2] = s.period;
                           instance.params[3] = s.phase;
                           instance.colors[0] = packColor(s.color, alpha);
                           visible = s.color.a * alpha > 0 && s.width > 0 && s.period > 0;
                       },
               },
               paint);
    if (!visible || alpha <= 0) {
        return;
    }
    emit(instance, shape.rect.inset(-1 / std::max(scale(), 1e-6f)));
}

void PlanBuilder::shadow(const ShadowCommand &command) {
    const Shadow &s = command.shadow;
    if (command.shape.rect.empty() || s.color.a * state_.alpha <= 0) {
        return;
    }
    GpuInstance instance{};
    setShape(instance, command.shape);
    const float sigma = std::max(s.blur, 0.0f) / 2;
    instance.header = header(s.inset ? InstanceKind::insetShadow : InstanceKind::dropShadow);
    instance.params[0] = s.offset.x;
    instance.params[1] = s.offset.y;
    instance.params[2] = sigma;
    instance.params[3] = s.spread;
    instance.colors[0] = packColor(s.color, state_.alpha);
    const float aa = 1 / std::max(scale(), 1e-6f);
    Rect bounds = s.inset ? command.shape.rect.inset(-aa)
                          : command.shape.rect.translated(s.offset).inset(
                                    -(s.spread + 3 * sigma + aa));
    if (bounds.empty()) {
        return;
    }
    emit(instance, bounds);
}

void PlanBuilder::backdrop(const BackdropBlurCommand &command) {
    if (command.shape.rect.empty() || state_.alpha <= 0) {
        return;
    }
    FramePlan &plan = *plan_;
    const float sigma = std::max(command.radius, 0.0f);
    GpuInstance instance{};
    setShape(instance, command.shape);
    instance.header = header(InstanceKind::backdrop);
    instance.params[0] = std::max(command.saturation, 0.0f);
    instance.params[2] = 1.0f / static_cast<float>(levelSize(width_, 1));
    instance.params[3] = 1.0f / static_cast<float>(levelSize(height_, 1));
    instance.extra[0] = state_.alpha;
    const Rect bounds = command.shape.rect.inset(-1 / std::max(scale(), 1e-6f));
    const auto setRegion = [&instance](const BlurSetup &setup) {
        instance.params[1] = setup.offset;
        const IRect &region = setup.regions[1];
        instance.offsets[0] = static_cast<std::uint32_t>(region.x0) |
                              static_cast<std::uint32_t>(region.y0) << 16;
        instance.offsets[1] = static_cast<std::uint32_t>(region.x1) |
                              static_cast<std::uint32_t>(region.y1) << 16;
    };

    if (command.source == BlurSource::background && backgroundDone_) {
        std::uint32_t entry = 0;
        while (entry < plan.backgroundBlurs.size() &&
               std::abs(plan.backgroundBlurs[entry].sigma - sigma) > 0.01f) {
            ++entry;
        }
        if (entry == plan.backgroundBlurs.size()) {
            plan.backgroundBlurs.push_back(
                    {sigma, makeBlurSetup(sigma, {0, 0, width_, height_}, width_, height_)});
        }
        setRegion(plan.backgroundBlurs[entry].setup);
        emit(instance, bounds, {Aux::Kind::backgroundBlur, entry});
        return;
    }

    // Live: blur what's been drawn so far around the visible part of the shape.
    const Rect visible = deviceBounds(bounds).intersect(clipInfo_[state_.clip].outer);
    if (visible.empty()) {
        return;
    }
    const float margin = std::ceil(3 * sigma) + 2;
    const IRect area = outward(visible.inset(-margin));
    BlurSetup setup = makeBlurSetup(sigma, area, width_, height_);
    setRegion(setup);
    // The blur reads the layer around the shape, so that area must be cleared too.
    if (!layerBounds_.empty()) {
        layerBounds_.back() = layerBounds_.back().unite(toRect(setup.regions[0]));
    }
    plan.ops.push_back(LiveBlurOp{std::move(setup)});
    ++plan.liveBlurs;
    emit(instance, bounds, {Aux::Kind::liveBlur, 0});
}

void PlanBuilder::text(const TextCommand &command) {
    if (!command.text || state_.alpha * command.color.a <= 0) {
        return;
    }
    const ShapedText &shaped = *command.text;
    float maxSize = 0;
    for (const GlyphRun &run : shaped.runs) {
        maxSize = std::max(maxSize, run.size);
    }
    // Glyphs can overhang the layout box, e.g. accents and italics.
    const Rect layout{command.origin.x, command.origin.y, shaped.size.width, shaped.size.height};
    if (deviceBounds(layout.inset(-maxSize - 2)).intersect(clipInfo_[state_.clip].outer).empty()) {
        return;
    }

    const Transform &t = state_.transform;
    const bool snap = t.isTranslation();
    constexpr float kPage = GlyphAtlas::kPageSize;
    for (const GlyphRun &run : shaped.runs) {
        const std::uint32_t end = std::min<std::uint32_t>(
                run.firstGlyph + run.glyphCount, static_cast<std::uint32_t>(shaped.glyphs.size()));
        const std::uint32_t tint =
                run.color ? packColor({1, 1, 1, 1}, command.color.a * state_.alpha)
                          : packColor(command.color, state_.alpha);
        for (std::uint32_t i = run.firstGlyph; i < end; ++i) {
            const Vec2 pen = command.origin + shaped.glyphs[i].position;
            // Glyphs land on whole pixels vertically and quarter pixels horizontally, with the
            // quarter baked into the bitmap; under other transforms they're drawn as quads.
            int subpixel = 0;
            Vec2 origin = pen;
            if (snap) {
                const float x = pen.x + t.tx;
                float whole = std::floor(x);
                subpixel = static_cast<int>(
                        std::lround((x - whole) * GlyphAtlas::kSubpixelSteps));
                if (subpixel == GlyphAtlas::kSubpixelSteps) {
                    subpixel = 0;
                    whole += 1;
                }
                origin = {whole - t.tx, std::round(pen.y + t.ty) - t.ty};
            }
            const GlyphAtlas::Glyph *glyph =
                    atlas_.find(text_, run.face, shaped.glyphs[i].glyph, run.size, subpixel, frame_);
            if (glyph == nullptr) {
                continue;
            }
            GpuInstance instance{};
            instance.header = header(glyph->color ? InstanceKind::colorGlyph : InstanceKind::glyph);
            instance.rect[0] = origin.x + glyph->left;
            instance.rect[1] = origin.y - glyph->top;
            instance.rect[2] = glyph->width;
            instance.rect[3] = glyph->height;
            instance.radii[0] = glyph->x / kPage;
            instance.radii[1] = glyph->y / kPage;
            instance.radii[2] = (glyph->x + glyph->width) / kPage;
            instance.radii[3] = (glyph->y + glyph->height) / kPage;
            instance.params[0] = glyph->page;
            instance.colors[0] = tint;
            emit(instance, {instance.rect[0], instance.rect[1], instance.rect[2],
                            instance.rect[3]});
        }
    }
}

void PlanBuilder::image(const ImageCommand &command) {
    if (command.image == 0 || command.destination.empty() || command.opacity * state_.alpha <= 0 ||
        !imageResident_(command.image)) {
        return;
    }
    GpuInstance instance{};
    setShape(instance, {command.destination, command.radii});
    instance.header = header(InstanceKind::image);
    instance.extra[0] = std::clamp(command.opacity, 0.0f, 1.0f) * state_.alpha;
    emit(instance, command.destination.inset(-1 / std::max(scale(), 1e-6f)),
         {Aux::Kind::image, command.image});
}

void PlanBuilder::externalImage(const ExternalImageCommand &command) {
    if (command.image == 0 || command.destination.empty() || command.opacity * state_.alpha <= 0 ||
        !externalResident_) {
        return;
    }
    const auto view = externalResident_(command.image);
    if (!view) {
        return;
    }
    GpuInstance instance{};
    setShape(instance, {command.destination, command.radii});
    instance.header = header(InstanceKind::image);
    std::copy_n(view->uv, 4, instance.params);
    instance.extra[0] = std::clamp(command.opacity, 0.0f, 1.0f) * state_.alpha;
    emit(instance, command.destination.inset(-1 / std::max(scale(), 1e-6f)),
         {Aux::Kind::external, command.image});
}

void PlanBuilder::pushClip(const RoundedRect &shape) {
    stack_.push_back({Push::clip, state_, 0, 1});
    FramePlan &plan = *plan_;
    const Transform &t = state_.transform;
    const ClipInfo &parent = clipInfo_[state_.clip];
    GpuClip clip{};
    ClipInfo info;
    const float det = t.a * t.d - t.b * t.c;
    if (std::abs(det) > 1e-12f && !shape.rect.empty()) {
        const float a = t.d / det;
        const float b = -t.b / det;
        const float c = -t.c / det;
        const float d = t.a / det;
        clip.inverse[0] = a;
        clip.inverse[1] = b;
        clip.inverse[2] = c;
        clip.inverse[3] = d;
        clip.inverseTranslate[0] = -(a * t.tx + c * t.ty);
        clip.inverseTranslate[1] = -(b * t.tx + d * t.ty);
        clip.scale = std::sqrt(std::abs(det));
        info.outer = deviceBounds(shape.rect).intersect(parent.outer);
        if (t.b == 0 && t.c == 0 && !parent.inner.empty()) {
            const CornerRadii radii = clampRadii(shape);
            const float radius = std::max({radii.topLeft, radii.topRight, radii.bottomRight,
                                           radii.bottomLeft});
            // The corner arcs stay outside a rect inset by r (1 - 1/sqrt(2)).
            info.inner = deviceBounds(shape.rect.inset(radius * 0.2929f)).intersect(parent.inner);
        }
    }
    clip.parent = state_.clip;
    GpuInstance shapeData{};
    setShape(shapeData, shape);
    std::copy(std::begin(shapeData.rect), std::end(shapeData.rect), clip.rect);
    std::copy(std::begin(shapeData.radii), std::end(shapeData.radii), clip.radii);
    state_.clip = static_cast<std::uint32_t>(plan.clips.size());
    plan.clips.push_back(clip);
    clipInfo_.push_back(info);
}

void PlanBuilder::pushLayer(float opacity) {
    FramePlan &plan = *plan_;
    stack_.push_back({Push::layerOpacity, state_, plan.ops.size(), opacity});
    plan.ops.push_back(BeginLayerOp{layerDepth_, {}});
    ++layerDepth_;
    plan.maxLayerDepth = std::max(plan.maxLayerDepth, layerDepth_);
    layerBounds_.push_back({});
    // Folded opacity from outside applies to the composite instead.
    state_.alpha = 1;
}

void PlanBuilder::popLayer(const StackEntry &entry) {
    FramePlan &plan = *plan_;
    --layerDepth_;
    const Rect target{0, 0, static_cast<float>(width_), static_cast<float>(height_)};
    const Rect bounds = layerBounds_.back().intersect(target);
    layerBounds_.pop_back();
    const float alpha = entry.opacity * entry.saved.alpha;
    if (bounds.empty() || alpha <= 0) {
        // Nothing was drawn, or nothing shows: drop the layer and everything in it.
        plan.ops.resize(entry.layerOp);
        noMerge_ = true;
        return;
    }
    const IRect area = outward(bounds);
    std::get<BeginLayerOp>(plan.ops[entry.layerOp]).bounds = area;
    plan.ops.push_back(EndLayerOp{layerDepth_});

    GpuInstance instance{};
    instance.header = header(InstanceKind::layer);
    instance.rect[0] = static_cast<float>(area.x0);
    instance.rect[1] = static_cast<float>(area.y0);
    instance.rect[2] = static_cast<float>(area.width());
    instance.rect[3] = static_cast<float>(area.height());
    instance.transform[0] = 1;
    instance.transform[3] = 1;
    instance.extra[0] = alpha;
    emitDevice(instance, toRect(area), {Aux::Kind::layer, layerDepth_});
}

}  // namespace filepan::gfx::vulkan::detail
