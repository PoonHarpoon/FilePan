// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// The renderer's CPU side: batching, culling, clip elision, opacity folding, background hashing
// and blur setup. Needs no GPU.

#include <cmath>
#include <cstdio>
#include <variant>

#include "filepan/gfx/vulkan/frame_plan.hpp"
#include "gfx_test_util.hpp"

using namespace filepan::gfx;
using namespace filepan::gfx::vulkan::detail;
using filepan::test::FakeTextSystem;

namespace {

struct Fixture {
    FakeTextSystem text;
    GlyphAtlas atlas;
    PlanBuilder builder{text, atlas, [](ImageId id) { return id == 1 || id == 2; }};
    FramePlan plan;
    std::uint64_t frame = 1;

    const FramePlan &build(const DisplayList &list, int width = 200, int height = 200) {
        builder.build(list, width, height, Color{}, frame++, plan);
        return plan;
    }
};

template <typename Op>
int count(const FramePlan &plan) {
    int n = 0;
    for (const auto &op : plan.ops) {
        n += std::holds_alternative<Op>(op) ? 1 : 0;
    }
    return n;
}

void testBatching() {
    Fixture f;
    DisplayList list;
    for (int i = 0; i < 50; ++i) {
        const float y = static_cast<float>(i) * 4;
        list.fill(RoundedRect{{0, y, 100, 3}, CornerRadii::uniform(2)}, Color{1, 0, 0, 1});
        list.stroke(RoundedRect{{0, y, 100, 3}, {}}, 1, Color{0, 1, 0, 1});
        list.shadow(RoundedRect{{0, y, 100, 3}, {}}, Shadow{{0, 1}, 2, 0, Color{0, 0, 0, 1}, false});
        list.text(filepan::test::fakeText(f.text, "ab", 10), {0, y}, Color{1, 1, 1, 1});
    }
    const FramePlan &plan = f.build(list);
    EXPECT(plan.drawCalls == 1);
    EXPECT(plan.ops.size() == 1);
    EXPECT(plan.instances.size() == 50 * 5);

    // Images split batches only where the bound image changes.
    DisplayList images;
    images.image(1, Rect{0, 0, 10, 10});
    images.fill(Rect{0, 0, 10, 10}, Color{1, 1, 1, 1});
    images.image(1, Rect{20, 0, 10, 10});
    images.image(2, Rect{40, 0, 10, 10});
    images.image(3, Rect{60, 0, 10, 10});  // Not resident: skipped.
    EXPECT(f.build(images).drawCalls == 2);
    EXPECT(f.plan.instances.size() == 4);

    // External images have their own pipeline: they never share a draw with anything else,
    // and they sample the frame's crop.
    PlanBuilder external{f.text, f.atlas, [](ImageId) { return 0u; }, [](ExternalImageId id) {
                             return id == 5 ? std::optional<PlanBuilder::ExternalView>(
                                                      PlanBuilder::ExternalView{{0.25f, 0, 1, 0.5f}, 3})
                                            : std::nullopt;
                         }};
    DisplayList video;
    video.fill(Rect{0, 0, 10, 10}, Color{1, 1, 1, 1});
    video.externalImage(5, Rect{0, 0, 10, 10});
    video.externalImage(5, Rect{20, 0, 10, 10});
    video.fill(Rect{0, 0, 10, 10}, Color{1, 1, 1, 1});
    video.externalImage(6, Rect{0, 0, 10, 10});  // No frame: skipped.
    external.build(video, 200, 200, Color{}, 1, f.plan);
    EXPECT(f.plan.drawCalls == 3);
    EXPECT(f.plan.instances.size() == 4);
    const auto &draw = std::get<DrawOp>(f.plan.ops[1]);
    EXPECT(draw.aux.kind == Aux::Kind::external && draw.aux.index == 5 && draw.count == 2);
    EXPECT(f.plan.instances[1].params[0] == 0.25f && f.plan.instances[1].params[3] == 0.5f);
}

void testCullingAndClips() {
    Fixture f;
    DisplayList list;
    list.fill(Rect{-50, -50, 20, 20}, Color{1, 1, 1, 1});    // Off target.
    list.fill(Rect{250, 10, 20, 20}, Color{1, 1, 1, 1});     // Off target.
    list.pushClip(RoundedRect{{10, 10, 100, 100}, CornerRadii::uniform(10)});
    list.fill(Rect{20, 20, 50, 50}, Color{1, 1, 1, 1});      // Inside: needs no clip.
    list.fill(Rect{0, 0, 30, 30}, Color{1, 1, 1, 1});        // Crosses the rounded corner.
    list.fill(Rect{150, 150, 10, 10}, Color{1, 1, 1, 1});    // Clipped away entirely.
    list.pop();
    const FramePlan &plan = f.build(list);
    EXPECT(plan.instances.size() == 2);
    EXPECT(plan.instances[0].clip == 0);
    EXPECT(plan.instances[1].clip == 1);
    EXPECT(plan.clips.size() == 2);

    // Under rotation, the clip is always evaluated.
    DisplayList rotated;
    rotated.pushTransform(Transform{0.8f, 0.6f, -0.6f, 0.8f, 100, 0});
    rotated.pushClip(RoundedRect{{0, 0, 100, 100}, {}});
    rotated.fill(Rect{40, 40, 10, 10}, Color{1, 1, 1, 1});
    rotated.pop();
    rotated.pop();
    EXPECT(f.build(rotated).instances.size() == 1);
    EXPECT(f.plan.instances[0].clip != 0);
}

void testOpacity() {
    Fixture f;
    DisplayList folded;
    folded.pushOpacity(0.5f);
    folded.pushTransform(Transform::translation({5, 5}));
    folded.fill(Rect{0, 0, 10, 10}, Color{1, 0, 0, 1});
    folded.pop();
    folded.pop();
    EXPECT(count<BeginLayerOp>(f.build(folded)) == 0);
    EXPECT(f.plan.instances[0].colors[0] >> 24 == 128);  // 0.5 * 255, rounded.

    DisplayList layered;
    layered.fill(Rect{0, 0, 200, 200}, Color{0, 0, 0, 1});
    layered.pushOpacity(0.5f);
    layered.fill(Rect{10, 10, 20, 20}, Color{1, 0, 0, 1});
    layered.fill(Rect{20, 20, 20, 20}, Color{0, 1, 0, 1});
    layered.pop();
    const FramePlan &plan = f.build(layered);
    EXPECT(count<BeginLayerOp>(plan) == 1);
    EXPECT(count<EndLayerOp>(plan) == 1);
    EXPECT(plan.maxLayerDepth == 1);
    const auto &begin = std::get<BeginLayerOp>(plan.ops[1]);
    EXPECT((begin.bounds == IRect{9, 9, 41, 41}));  // Drawn area plus antialiasing.
    // The composite is the last instance, a layer quad at half opacity.
    EXPECT((plan.instances.back().header & 0xff) == static_cast<std::uint32_t>(InstanceKind::layer));
    EXPECT(plan.instances.back().extra[0] == 0.5f);

    // A group that draws nothing visible costs nothing.
    DisplayList empty;
    empty.pushOpacity(0.5f);
    empty.fill(Rect{-100, -100, 10, 10}, Color{1, 1, 1, 1});
    empty.fill(Rect{-100, -100, 10, 10}, Color{1, 1, 1, 1});
    empty.pop();
    EXPECT(f.build(empty).ops.empty());
}

void testBackgroundHash() {
    Fixture f;
    const auto frame = [](float backgroundX, float contentX) {
        DisplayList list;
        list.fill(Rect{backgroundX, 0, 50, 50}, Color{1, 0, 0, 1});
        list.backgroundEnd();
        list.fill(Rect{contentX, 60, 50, 50}, Color{0, 1, 0, 1});
        list.backdropBlur(RoundedRect{{0, 0, 100, 100}, {}}, 10, 1, BlurSource::background);
        list.backdropBlur(RoundedRect{{0, 0, 100, 100}, {}}, 10, 1, BlurSource::background);
        list.backdropBlur(RoundedRect{{0, 0, 100, 100}, {}}, 4, 1, BlurSource::background);
        return list;
    };
    f.build(frame(0, 0));
    const std::uint64_t hash = f.plan.backgroundHash;
    EXPECT(f.plan.backgroundOps == 1u);
    EXPECT(f.plan.backgroundBlurs.size() == 2);  // Two distinct radii.
    EXPECT(f.plan.liveBlurs == 0);
    f.build(frame(0, 30));
    EXPECT(f.plan.backgroundHash == hash);  // Content after the marker doesn't count.
    f.build(frame(10, 0));
    EXPECT(f.plan.backgroundHash != hash);
    EXPECT(f.build(frame(0, 0), 300, 200).backgroundHash != hash);  // Nor does the size.

    // Without a marker first, background blurs fall back to live ones.
    DisplayList noMarker;
    noMarker.backdropBlur(RoundedRect{{0, 0, 100, 100}, {}}, 10, 1, BlurSource::background);
    EXPECT(f.build(noMarker).liveBlurs == 1);
    EXPECT(!f.plan.backgroundOps.has_value());
}

void testBlurSetup() {
    int previousLevels = 0;
    for (float sigma : {0.5f, 2.0f, 5.0f, 10.0f, 22.0f, 40.0f, 80.0f}) {
        const BlurSetup setup = makeBlurSetup(sigma, {13, 7, 151, 99}, 1080, 2400);
        EXPECT(setup.levels >= previousLevels);
        EXPECT(setup.offset >= 0 && setup.offset <= 1.75f);
        EXPECT(static_cast<int>(setup.regions.size()) == setup.levels + 1);
        const int align = 1 << setup.levels;
        EXPECT(setup.regions[0].x0 % align == 0 && setup.regions[0].y0 % align == 0);
        EXPECT(setup.regions[0].x0 <= 13 && setup.regions[0].x1 >= 151);
        for (int i = 1; i <= setup.levels; ++i) {
            EXPECT(setup.regions[i].x0 * 2 == setup.regions[i - 1].x0);
        }
        previousLevels = setup.levels;
    }
    // Clamped to the target.
    const BlurSetup edge = makeBlurSetup(10, {-40, -40, 2000, 2000}, 100, 61);
    EXPECT((edge.regions[0] == IRect{0, 0, 100, 61}));
    EXPECT(edge.regions[1].y1 == 31);
}

void testAtlas() {
    FakeTextSystem text;
    GlyphAtlas atlas;
    const auto *a = atlas.find(text, FakeTextSystem::kAlphaFace, 'a', 16, 0, 1);
    EXPECT(a != nullptr && !a->color);
    EXPECT(atlas.find(text, FakeTextSystem::kAlphaFace, ' ', 16, 0, 1) == nullptr);
    const auto *shifted = atlas.find(text, FakeTextSystem::kAlphaFace, 'a', 16, 2, 1);
    EXPECT(shifted != nullptr && shifted != a);
    EXPECT(atlas.find(text, FakeTextSystem::kColorFace, 'e', 16, 0, 1)->color);
    EXPECT(atlas.uploads().size() == 3);
    atlas.clearUploads();
    const int calls = text.rasterizeCalls;
    atlas.find(text, FakeTextSystem::kAlphaFace, 'a', 16, 0, 2);
    atlas.find(text, FakeTextSystem::kAlphaFace, ' ', 16, 0, 2);
    EXPECT(text.rasterizeCalls == calls);
    EXPECT(atlas.uploads().empty());

    // Fill every page, then a new frame evicts the least recently used one.
    std::uint64_t frame = 3;
    std::uint32_t glyph = 1000;
    while (atlas.evictions() == 0) {
        EXPECT(atlas.find(text, FakeTextSystem::kAlphaFace, glyph++, 120, 0, frame) != nullptr);
        if (glyph % 64 == 0) {
            ++frame;
        }
    }
    bool cleared = false;
    for (int page = 0; page < GlyphAtlas::kAlphaPages; ++page) {
        cleared = cleared || atlas.alphaPageNeedsClear(page);
    }
    EXPECT(cleared);
}

}  // namespace

int main() {
    testBatching();
    testCullingAndClips();
    testOpacity();
    testBackgroundHash();
    testBlurSetup();
    testAtlas();
    std::printf("frame_plan_test passed\n");
    return 0;
}
