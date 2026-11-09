// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Per-frame work: uploads, playing the frame plan back into command buffers, blurs, submission.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <variant>

#ifdef __ANDROID__
#include <android/native_window.h>
#endif

#include "filepan/gfx/vulkan/renderer_impl.hpp"
#include "filepan/gfx/vulkan/swapchain.hpp"

namespace filepan::gfx::vulkan {

namespace {

using detail::Aux;
using detail::BlurSetup;
using detail::FramePlan;
using detail::Image;
using detail::IRect;
using detail::useImage;

using Clock = std::chrono::steady_clock;

std::int64_t nanoseconds(Clock::duration duration) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

int levelSize(int size, int level) { return (size + (1 << level) - 1) >> level; }

VkClearColorValue premultipliedClear(const Color &color) {
    VkClearColorValue value{};
    const float a = std::clamp(color.a, 0.0f, 1.0f);
    value.float32[0] = std::clamp(color.r, 0.0f, 1.0f) * a;
    value.float32[1] = std::clamp(color.g, 0.0f, 1.0f) * a;
    value.float32[2] = std::clamp(color.b, 0.0f, 1.0f) * a;
    value.float32[3] = a;
    return value;
}

VkRect2D toVk(const IRect &rect) {
    return {{rect.x0, rect.y0},
            {static_cast<std::uint32_t>(rect.width()), static_cast<std::uint32_t>(rect.height())}};
}

// A rect in a width x height frame, in an image holding the frame turned transform quarter turns
// clockwise. A quarter turn takes the frame's (x, y) to (height - y, x).
IRect turned(const IRect &rect, int transform, int width, int height) {
    switch (transform) {
        case 1:
            return {height - rect.y1, rect.x0, height - rect.y0, rect.x1};
        case 2:
            return {width - rect.x1, height - rect.y1, width - rect.x0, height - rect.y0};
        case 3:
            return {rect.y0, width - rect.x1, rect.y1, width - rect.x0};
        default:
            return rect;
    }
}

// Whether the plan reads back what it has drawn into the target itself: a live blur outside
// any layer.
bool readsBase(const FramePlan &plan) {
    int depth = 0;
    for (const auto &op : plan.ops) {
        if (std::holds_alternative<detail::BeginLayerOp>(op)) {
            ++depth;
        } else if (std::holds_alternative<detail::EndLayerOp>(op)) {
            --depth;
        } else if (depth == 0 && std::holds_alternative<detail::LiveBlurOp>(op)) {
            return true;
        }
    }
    return false;
}

// Plays plan ops back, beginning and ending render passes as layers and blurs need.
class Recorder {
public:
    Recorder(Renderer::Impl &renderer, VkCommandBuffer cmd, detail::Frame &frame,
             RenderTarget::Impl &target)
        : r_(renderer), cmd_(cmd), frame_(frame), target_(target), p_(*target.pipelines) {}

    // Into base, which holds the frame turned transform quarter turns clockwise.
    void replay(std::size_t begin, std::size_t end, Image &base, const Color &clear,
                int transform = 0) {
        const FramePlan &plan = r_.plan;
        dests_.clear();
        dests_.push_back({&base, frameArea(base, transform), false, premultipliedClear(clear),
                          transform});
        for (std::size_t i = begin; i < end; ++i) {
            std::visit([this](const auto &op) { run(op); }, plan.ops[i]);
        }
        // Even an empty frame clears.
        ensurePass();
        endPass();
    }

    // Draws source, a whole unrotated frame, into output turned transform quarter turns, with
    // the plan's instance at index.
    void composite(Image &source, Image &output, int transform, std::uint32_t index,
                   const Color &clear) {
        useImage(cmd_, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        dests_.clear();
        dests_.push_back({&output, frameArea(output, transform), false, premultipliedClear(clear),
                          transform});
        ensurePass();
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, r_.uiLayout, 1, 1,
                                &source.set, 0, nullptr);
        vkCmdDraw(cmd_, 4, 1, 0, index);
        endPass();
    }

    // A dual-Kawase blur of source's regions[0] into level1 (half size), which ends up ready to
    // sample.
    void blur(Image &source, const BlurSetup &setup, Image &level1) {
        ++blurs;
        const IRect &r0 = setup.regions[0];
        const IRect &r1 = setup.regions[1];
        if (!r0.empty() && !r1.empty()) {
            useImage(cmd_, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            useImage(cmd_, level1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[0] = {r0.x0, r0.y0, 0};
            blit.srcOffsets[1] = {r0.x1, r0.y1, 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[0] = {r1.x0, r1.y0, 0};
            blit.dstOffsets[1] = {r1.x1, r1.y1, 1};
            vkCmdBlitImage(cmd_, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, level1.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            const auto level = [&](int i) -> Image & {
                return i == 1 ? level1 : target_.levels[i];
            };
            for (int i = 1; i < setup.levels; ++i) {
                pass(p_.blurDown, level(i), level(i + 1), setup.regions[i + 1], setup.regions[i],
                     setup.offset);
            }
            for (int i = setup.levels - 1; i >= 1; --i) {
                pass(p_.blurUp, level(i + 1), level(i), setup.regions[i], setup.regions[i + 1],
                     setup.offset);
            }
        }
        useImage(cmd_, level1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }

    std::uint32_t passes = 0;
    std::uint32_t blurs = 0;

private:
    struct Dest {
        Image *image;
        IRect area;  // In the frame, unrotated.
        bool cleared;
        VkClearColorValue clear;
        int transform = 0;  // Quarter turns clockwise the image holds the frame by.
    };

    // The whole frame an image holds turned transform quarter turns.
    static IRect frameArea(const Image &image, int transform) {
        const int w = static_cast<int>(image.width);
        const int h = static_cast<int>(image.height);
        return transform % 2 != 0 ? IRect{0, 0, h, w} : IRect{0, 0, w, h};
    }

    // dest.area in the image's own pixels.
    static IRect imageArea(const Dest &dest) {
        const IRect frame = frameArea(*dest.image, dest.transform);
        return turned(dest.area, dest.transform, frame.x1, frame.y1);
    }

    void ensurePass() {
        if (inPass_) {
            return;
        }
        Dest &dest = dests_.back();
        useImage(cmd_, *dest.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        VkClearValue clear{};
        clear.color = dest.clear;
        VkRenderPassBeginInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.renderPass = dest.cleared ? p_.loadPass : p_.clearPass;
        info.framebuffer = dest.image->framebuffer;
        info.renderArea = toVk(imageArea(dest));
        info.clearValueCount = 1;
        info.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd_, &info, VK_SUBPASS_CONTENTS_INLINE);
        dest.cleared = true;
        inPass_ = true;
        ++passes;

        const VkViewport viewport{0, 0, static_cast<float>(dest.image->width),
                                  static_cast<float>(dest.image->height), 0, 1};
        vkCmdSetViewport(cmd_, 0, 1, &viewport);
        const VkRect2D scissor = toVk(imageArea(dest));
        vkCmdSetScissor(cmd_, 0, 1, &scissor);
        bindUi();
    }

    void endPass() {
        if (inPass_) {
            vkCmdEndRenderPass(cmd_);
            inPass_ = false;
        }
    }

    VkDescriptorSet auxSet(const Aux &aux) const {
        switch (aux.kind) {
            case Aux::Kind::image:
                if (auto it = r_.residentImages.find(aux.index); it != r_.residentImages.end()) {
                    return it->second.set;
                }
                break;
            case Aux::Kind::layer:
                return target_.layers[aux.index].set;
            case Aux::Kind::liveBlur:
                return target_.levels[1].set;
            case Aux::Kind::backgroundBlur:
                return target_.backgroundBlurs[r_.backgroundSlots[aux.index]].image.set;
            case Aux::Kind::external:  // Drawn by runExternal.
            case Aux::Kind::none:
                break;
        }
        return r_.dummy.set;
    }

    void bindUi() {
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, p_.ui);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, r_.uiLayout, 0, 1,
                                &frame_.set, 0, nullptr);
        pushTargetSize(r_.uiLayout);
        boundAux_ = VK_NULL_HANDLE;
        externalBound_ = false;
    }

    void pushTargetSize(VkPipelineLayout layout) {
        const Dest &dest = dests_.back();
        const IRect frame = frameArea(*dest.image, dest.transform);
        const detail::UiPush push{{static_cast<float>(frame.x1), static_cast<float>(frame.y1)},
                                  static_cast<std::uint32_t>(dest.transform), 0};
        vkCmdPushConstants(cmd_, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);
    }

    // External images use their own pipeline: external.frag, with the YCbCr variant's layout
    // when the frame needs a conversion.
    void runExternal(const detail::DrawOp &op) {
        const detail::ExternalTexture *texture = r_.externalTexture(op.aux.index);
        if (texture == nullptr) {
            return;
        }
        VkPipeline pipeline = p_.external;
        VkPipelineLayout layout = r_.uiLayout;
        if (texture->variant != nullptr) {
            auto found = r_.ycbcrPipeline(*texture->variant, p_);
            if (!found) {
                return;
            }
            pipeline = *found;
            layout = texture->variant->layout;
        }
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &frame_.set,
                                0, nullptr);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1,
                                &texture->image.set, 0, nullptr);
        pushTargetSize(layout);
        vkCmdDraw(cmd_, 4, op.count, 0, op.first);
        externalBound_ = true;
        boundAux_ = VK_NULL_HANDLE;
    }

    void run(const detail::DrawOp &op) {
        ensurePass();
        if (op.aux.kind == Aux::Kind::external) {
            runExternal(op);
            return;
        }
        if (externalBound_) {
            bindUi();
        }
        const VkDescriptorSet set = auxSet(op.aux);
        if (set != boundAux_) {
            vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, r_.uiLayout, 1, 1, &set,
                                    0, nullptr);
            boundAux_ = set;
        }
        vkCmdDraw(cmd_, 4, op.count, 0, op.first);
    }

    void run(const detail::BeginLayerOp &op) {
        endPass();
        dests_.push_back({&target_.layers[op.layer], op.bounds, false, {}});
    }

    void run(const detail::EndLayerOp &op) {
        ensurePass();
        endPass();
        useImage(cmd_, target_.layers[op.layer], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        dests_.pop_back();
    }

    void run(const detail::LiveBlurOp &op) {
        // Make sure the destination has been cleared, then blur what's in it.
        ensurePass();
        endPass();
        blur(*dests_.back().image, op.setup, target_.levels[1]);
    }

    void pass(VkPipeline pipeline, Image &source, Image &dest, const IRect &area,
              const IRect &sourceArea, float offset) {
        if (area.empty()) {
            return;
        }
        useImage(cmd_, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        useImage(cmd_, dest, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderPassBeginInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.renderPass = p_.discardPass;
        info.framebuffer = dest.framebuffer;
        info.renderArea = toVk(area);
        vkCmdBeginRenderPass(cmd_, &info, VK_SUBPASS_CONTENTS_INLINE);
        ++passes;
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        const VkViewport viewport{0, 0, static_cast<float>(dest.width),
                                  static_cast<float>(dest.height), 0, 1};
        vkCmdSetViewport(cmd_, 0, 1, &viewport);
        const VkRect2D scissor = toVk(area);
        vkCmdSetScissor(cmd_, 0, 1, &scissor);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, r_.blurLayout, 0, 1,
                                &source.set, 0, nullptr);
        struct {
            float clampRect[4];
            float invSourceSize[2];
            float offset;
        } constants{{static_cast<float>(sourceArea.x0) + 0.5f,
                     static_cast<float>(sourceArea.y0) + 0.5f,
                     static_cast<float>(sourceArea.x1) - 0.5f,
                     static_cast<float>(sourceArea.y1) - 0.5f},
                    {1.0f / static_cast<float>(source.width),
                     1.0f / static_cast<float>(source.height)},
                    offset};
        vkCmdPushConstants(cmd_, r_.blurLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants),
                           &constants);
        vkCmdDraw(cmd_, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd_);
    }

    Renderer::Impl &r_;
    VkCommandBuffer cmd_;
    detail::Frame &frame_;
    RenderTarget::Impl &target_;
    detail::Pipelines &p_;
    std::vector<Dest> dests_;
    bool inPass_ = false;
    VkDescriptorSet boundAux_ = VK_NULL_HANDLE;
    bool externalBound_ = false;  // An external pipeline is bound instead of the UI one.
};

void clearImage(VkCommandBuffer cmd, Image &image) {
    useImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
             VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, image.layers};
    vkCmdClearColorImage(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
}

void makeReadable(VkCommandBuffer cmd, Image &image) {
    useImage(cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
}

}  // namespace

Result<void> Renderer::Impl::prepareTarget(RenderTarget::Impl &target, const FramePlan &plan,
                                           bool &captureBackground) {
    // Background blurs: reuse ones computed from the same background, recompute the rest.
    backgroundSlots.assign(plan.backgroundBlurs.size(), 0);
    backgroundCompute.clear();
    auto &cache = target.backgroundBlurs;
    for (std::size_t i = 0; i < plan.backgroundBlurs.size(); ++i) {
        const float sigma = plan.backgroundBlurs[i].sigma;
        const auto matches = [sigma](const detail::CachedBlur &blur) {
            return std::abs(blur.sigma - sigma) <= 0.01f;
        };
        auto it = std::find_if(cache.begin(), cache.end(), [&](const detail::CachedBlur &blur) {
            return matches(blur) && blur.hash == plan.backgroundHash;
        });
        if (it == cache.end()) {
            it = std::find_if(cache.begin(), cache.end(), matches);
        }
        if (it == cache.end() && cache.size() >= detail::kMaxBackgroundBlurs) {
            it = std::min_element(cache.begin(), cache.end(), [](const auto &a, const auto &b) {
                return a.lastUsed < b.lastUsed;
            });
            if (it->lastUsed == frameNumber + 1) {
                it = cache.end();  // Every entry is in use this frame; add one.
            }
        }
        if (it == cache.end()) {
            auto image = createScratchImage(target, levelSize(target.width, 1),
                                            levelSize(target.height, 1));
            if (!image) {
                return std::unexpected(image.error());
            }
            cache.push_back({sigma, *image, 0, 0});
            it = cache.end() - 1;
        }
        if (it->hash != plan.backgroundHash || !matches(*it) || it->lastUsed == 0) {
            it->sigma = sigma;
            it->hash = plan.backgroundHash;
            backgroundCompute.push_back(i);
        }
        it->lastUsed = frameNumber + 1;  // Never 0, which marks a fresh entry.
        backgroundSlots[i] = static_cast<std::size_t>(it - cache.begin());
    }
    captureBackground = !backgroundCompute.empty() && plan.backgroundOps.has_value();

    std::size_t layersNeeded = plan.maxLayerDepth;
    if (captureBackground) {
        layersNeeded = std::max<std::size_t>(layersNeeded, plan.backgroundLayerDepth + 1);
    }
    while (target.layers.size() < layersNeeded) {
        auto image = createScratchImage(target, target.width, target.height);
        if (!image) {
            return std::unexpected(image.error());
        }
        target.layers.push_back(*image);
    }

    int levelsNeeded = 0;
    for (const auto &op : plan.ops) {
        if (const auto *blur = std::get_if<detail::LiveBlurOp>(&op)) {
            levelsNeeded = std::max(levelsNeeded, blur->setup.levels);
        }
    }
    for (std::size_t i : backgroundCompute) {
        levelsNeeded = std::max(levelsNeeded, plan.backgroundBlurs[i].setup.levels);
    }
    for (int level = 1; level <= levelsNeeded; ++level) {
        if (target.levels[level].image == VK_NULL_HANDLE) {
            auto image = createScratchImage(target, levelSize(target.width, level),
                                            levelSize(target.height, level));
            if (!image) {
                return std::unexpected(image.error());
            }
            target.levels[level] = *image;
        }
    }
    if (unrotatedComposite && target.unrotated.image == VK_NULL_HANDLE) {
        auto image = createScratchImage(target, target.width, target.height);
        if (!image) {
            return std::unexpected(image.error());
        }
        target.unrotated = *image;
    }
    return {};
}

void Renderer::Impl::record(VkCommandBuffer cmd, detail::Frame &frame, RenderTarget::Impl &target,
                            Image &output, Color clearColor, bool captureBackground,
                            const std::vector<ImageCopy> &imageCopies, VkDeviceSize glyphOffset) {
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    if (frame.queries != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(cmd, frame.queries, 0, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, frame.queries, 0);
    }

    // Uploads.
    if (!atlasCleared) {
        clearImage(cmd, alphaAtlas);
        clearImage(cmd, colorAtlas);
        clearImage(cmd, dummy);
        makeReadable(cmd, dummy);
        atlasCleared = true;
    }
    for (const ImageCopy &copy : imageCopies) {
        useImage(cmd, *copy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region{};
        region.bufferOffset = copy.offset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {copy.image->width, copy.image->height, 1};
        vkCmdCopyBufferToImage(cmd, frame.staging.buffer, copy.image->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        makeReadable(cmd, *copy.image);
    }
    acquireExternal(cmd);
    for (int page = 0; page < detail::GlyphAtlas::kAlphaPages; ++page) {
        if (atlas.alphaPageNeedsClear(page)) {
            useImage(cmd, alphaAtlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            const VkClearColorValue zero{};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                                                static_cast<std::uint32_t>(page), 1};
            vkCmdClearColorImage(cmd, alphaAtlas.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &zero, 1, &range);
        }
    }
    if (atlas.colorPageNeedsClear()) {
        clearImage(cmd, colorAtlas);
    }
    std::vector<VkBufferImageCopy> alphaRegions;
    std::vector<VkBufferImageCopy> colorRegions;
    for (const auto &upload : atlas.uploads()) {
        VkBufferImageCopy region{};
        region.bufferOffset = glyphOffset + upload.offset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, upload.color ? 0u : upload.page,
                                   1};
        region.imageOffset = {upload.x, upload.y, 0};
        region.imageExtent = {upload.width, upload.height, 1};
        (upload.color ? colorRegions : alphaRegions).push_back(region);
    }
    for (auto [image, regions] : {std::pair{&alphaAtlas, &alphaRegions},
                                  std::pair{&colorAtlas, &colorRegions}}) {
        if (!regions->empty()) {
            useImage(cmd, *image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            vkCmdCopyBufferToImage(cmd, frame.staging.buffer, image->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   static_cast<std::uint32_t>(regions->size()), regions->data());
        }
        makeReadable(cmd, *image);
    }

    Recorder recorder(*this, cmd, frame, target);
    if (captureBackground) {
        // Draw the background alone into a spare layer and blur it once for later frames.
        Image &capture = target.layers[plan.backgroundLayerDepth];
        recorder.replay(0, *plan.backgroundOps, capture, clearColor);
        for (std::size_t i : backgroundCompute) {
            recorder.blur(capture, plan.backgroundBlurs[i].setup,
                          target.backgroundBlurs[backgroundSlots[i]].image);
        }
    }
    if (unrotatedComposite) {
        recorder.replay(0, plan.ops.size(), target.unrotated, clearColor);
        recorder.composite(target.unrotated, output, target.transform, *unrotatedComposite,
                           clearColor);
    } else {
        recorder.replay(0, plan.ops.size(), output, clearColor, target.transform);
    }
    if (!target.offscreen) {
        useImage(cmd, output, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                 0);
    }

    if (frame.queries != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.queries, 1);
    }
    vkEndCommandBuffer(cmd);
    frame.timing.renderPasses = recorder.passes;
    frame.timing.blurs = recorder.blurs;
}

#ifdef __ANDROID__
void Renderer::Impl::noteSuboptimal(RenderTarget::Impl &target) {
    // Android reports SUBOPTIMAL when the display's transform no longer matches the swapchain's
    // preTransform: the display turned. A half turn keeps the window's size, so the size check
    // doesn't catch it; this does. Only a changed transform recreates, so a swapchain that stays
    // SUBOPTIMAL for another reason isn't remade every frame.
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device->physical, target.surface, &caps) == VK_SUCCESS &&
        detail::swapchainTransform(caps).preTransform != target.preTransform) {
        target.recreateSwapchain = true;
    }
}
#endif

Result<void> Renderer::Impl::render(RenderTarget::Impl &target, const DisplayList &list,
                                    Color clearColor) {
    const auto start = Clock::now();
    detail::Frame &frame = frames[frameNumber % detail::kFramesInFlight];
    finishFrame(frame);
    vkResetCommandPool(device->device, frame.pool, 0);
    const auto waited = Clock::now();
    frame.timing = {};
    frame.timing.frame = frameNumber;
    frame.timing.fenceWaitNs = nanoseconds(waited - start);

    Image *output = nullptr;
    std::uint32_t imageIndex = 0;
    if (target.offscreen) {
        if (target.color.image == VK_NULL_HANDLE) {
            return std::unexpected(Error{0, "The offscreen target has no image"});
        }
        output = &target.color;
    } else {
#ifdef __ANDROID__
        // Android never reports a size change as OUT_OF_DATE, so compare with the window's size.
        if (target.swapchain != VK_NULL_HANDLE &&
            detail::swapchainStale(target.windowWidth, target.windowHeight, ANativeWindow_getWidth(target.window),
                                   ANativeWindow_getHeight(target.window))) {
            target.recreateSwapchain = true;
        }
        VkResult result = VK_ERROR_OUT_OF_DATE_KHR;
        for (int attempt = 0; attempt < 2 && result == VK_ERROR_OUT_OF_DATE_KHR; ++attempt) {
            if (target.recreateSwapchain || target.swapchain == VK_NULL_HANDLE || attempt > 0) {
                if (auto created = createSwapchain(target); !created) {
                    return created;
                }
            }
            if (target.swapchain == VK_NULL_HANDLE) {
                return {};  // The window has no area; nothing to draw.
            }
            result = vkAcquireNextImageKHR(device->device, target.swapchain, UINT64_MAX,
                                           frame.acquired, VK_NULL_HANDLE, &imageIndex);
        }
        // SUBOPTIMAL still presents correctly, the compositor turning the image; keep going.
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            return detail::vkError(result, "vkAcquireNextImageKHR");
        }
        if (result == VK_SUBOPTIMAL_KHR) {
            noteSuboptimal(target);
        }
        output = &target.swapchainImages[imageIndex];
        output->state = {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0};
#else
        return std::unexpected(Error{0, "Surface targets need Android"});
#endif
    }
    const auto acquired = Clock::now();
    frame.timing.acquireNs = nanoseconds(acquired - waited);

    // Images queued from other threads: removals first, so an image added and removed before
    // it was ever uploaded is skipped.
    ImageStore::Pending pending = images.takePending();
    for (ImageId id : pending.removals) {
        if (auto it = residentImages.find(id); it != residentImages.end()) {
            frame.garbage.push_back([this, image = it->second]() mutable { destroyImage(image); });
            residentImages.erase(it);
        } else {
            removedBeforeUpload.insert(id);
        }
    }
    std::vector<ImageCopy> imageCopies;
    std::vector<StagedPixels> uploading;
    std::vector<ImageStore::Upload> deferred;
    VkDeviceSize stagingSize = 0;
    for (ImageStore::Upload &upload : pending.uploads) {
        if (removedBeforeUpload.erase(upload.id) > 0) {
            continue;
        }
        auto existing = residentImages.find(upload.id);
        if (upload.replace && existing == residentImages.end()) {
            continue;  // Removed, or never added.
        }
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(upload.width) * upload.height * 4;
        if (stagingSize > 0 && stagingSize + bytes > detail::kImageUploadBudget) {
            deferred.push_back(std::move(upload));
            continue;
        }
        if (existing != residentImages.end() &&
            existing->second.width == static_cast<std::uint32_t>(upload.width) &&
            existing->second.height == static_cast<std::uint32_t>(upload.height)) {
            // Same size: copy into the existing image. The copy's barrier waits for earlier
            // frames still sampling it, as they were submitted to the same queue before.
            ++existing->second.version;
            imageCopies.push_back({&existing->second, stagingSize});
            uploading.push_back({upload.pixels.data(),
                                 static_cast<std::size_t>(upload.width) * upload.height});
            stagingSize += (bytes + 15) & ~VkDeviceSize{15};
            continue;
        }
        std::uint32_t version = 1;
        if (existing != residentImages.end()) {
            // A new size: the old image goes once this frame, and so every earlier one, is done.
            version = existing->second.version + 1;
            frame.garbage.push_back(
                    [this, image = existing->second]() mutable { destroyImage(image); });
            residentImages.erase(existing);
        }
        auto image = device->createImage(static_cast<std::uint32_t>(upload.width),
                                         static_cast<std::uint32_t>(upload.height), 1,
                                         VK_FORMAT_R8G8B8A8_UNORM,
                                         VK_IMAGE_USAGE_SAMPLED_BIT |
                                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (!image) {
            return std::unexpected(image.error());
        }
        auto set = allocateTextureSet(image->view);
        if (!set) {
            device->destroy(*image);
            return std::unexpected(set.error());
        }
        image->set = *set;
        image->version = version;
        Image &resident = residentImages[upload.id] = *image;
        imageCopies.push_back({&resident, stagingSize});
        uploading.push_back(
                {upload.pixels.data(), static_cast<std::size_t>(upload.width) * upload.height});
        stagingSize += (bytes + 15) & ~VkDeviceSize{15};
    }
    if (!deferred.empty()) {
        images.requeue(std::move(deferred));
    }
    takeExternalImages(frame, imageCopies, uploading, stagingSize);

    builder.build(list, target.width, target.height, clearColor, frameNumber, plan);
    unrotatedComposite.reset();
    if (target.transform != 0 && readsBase(plan)) {
        // Drawn unrotated, then turned onto the target as one layer composite.
        detail::GpuInstance instance{};
        instance.header = static_cast<std::uint32_t>(detail::InstanceKind::layer);
        instance.rect[2] = static_cast<float>(target.width);
        instance.rect[3] = static_cast<float>(target.height);
        instance.transform[0] = 1;
        instance.transform[3] = 1;
        instance.extra[0] = 1;
        unrotatedComposite = static_cast<std::uint32_t>(plan.instances.size());
        plan.instances.push_back(instance);
    }
    bool captureBackground = false;
    if (auto prepared = prepareTarget(target, plan, captureBackground); !prepared) {
        return prepared;
    }

    // Instance, clip and staging data for this frame's slot, which the GPU is done with.
    const VkDeviceSize glyphOffset = stagingSize;
    stagingSize += atlas.uploadData().size();
    bool setStale = false;
    for (auto [buffer, bytes, usage, bound] :
         {std::tuple{&frame.instances, plan.instances.size() * sizeof(detail::GpuInstance),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true},
          std::tuple{&frame.clips, plan.clips.size() * sizeof(detail::GpuClip),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true},
          std::tuple{&frame.staging, static_cast<std::size_t>(stagingSize),
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false}}) {
        auto grown = growBuffer(*buffer, std::max<VkDeviceSize>(bytes, 1), usage);
        if (!grown) {
            return std::unexpected(grown.error());
        }
        setStale = setStale || (*grown && bound);
    }
    if (setStale) {
        updateFrameSet(frame);
    }
    std::memcpy(frame.instances.mapped, plan.instances.data(),
                plan.instances.size() * sizeof(detail::GpuInstance));
    std::memcpy(frame.clips.mapped, plan.clips.data(), plan.clips.size() * sizeof(detail::GpuClip));
    auto *staging = static_cast<std::uint8_t *>(frame.staging.mapped);
    for (std::size_t i = 0; i < uploading.size(); ++i) {
        // Premultiplied, so filtering doesn't darken edges.
        const std::uint8_t *pixels = uploading[i].pixels;
        std::uint8_t *out = staging + imageCopies[i].offset;
        const std::size_t count = uploading[i].texels;
        for (std::size_t p = 0; p < count; ++p) {
            const unsigned a = pixels[p * 4 + 3];
            for (int c = 0; c < 3; ++c) {
                out[p * 4 + c] = static_cast<std::uint8_t>((pixels[p * 4 + c] * a + 127) / 255);
            }
            out[p * 4 + 3] = static_cast<std::uint8_t>(a);
        }
    }
    if (!atlas.uploadData().empty()) {
        std::memcpy(staging + glyphOffset, atlas.uploadData().data(), atlas.uploadData().size());
    }

    record(frame.cmd, frame, target, *output, clearColor, captureBackground, imageCopies,
           glyphOffset);
    atlas.clearUploads();
    frame.timing.drawCalls = plan.drawCalls;
    frame.timing.instances = static_cast<std::uint32_t>(plan.instances.size());

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.cmd;
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (!target.offscreen) {
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &frame.acquired;
        submit.pWaitDstStageMask = &waitStage;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &target.renderDone[imageIndex];
    }
    frame.timing.recordNs = nanoseconds(Clock::now() - acquired);
    FILEPAN_VK_TRY(vkQueueSubmit(device->queue, 1, &submit, frame.fence));
    frame.pending = true;
    ++frameNumber;

#ifdef __ANDROID__
    if (!target.offscreen) {
        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &target.renderDone[imageIndex];
        present.swapchainCount = 1;
        present.pSwapchains = &target.swapchain;
        present.pImageIndices = &imageIndex;
        const VkResult result = vkQueuePresentKHR(device->queue, &present);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            target.recreateSwapchain = true;
        } else if (result == VK_SUBOPTIMAL_KHR) {
            noteSuboptimal(target);
        } else if (result != VK_SUCCESS) {
            return detail::vkError(result, "vkQueuePresentKHR");
        }
    }
#else
    (void)imageIndex;
#endif
    return {};
}

Result<std::vector<std::uint8_t>> Renderer::Impl::readPixels(RenderTarget::Impl &target) {
    if (!target.offscreen || target.color.image == VK_NULL_HANDLE) {
        return std::unexpected(Error{0, "Only offscreen targets can be read back"});
    }
    waitIdle();
    VkDevice vk = device->device;
    const VkDeviceSize size = static_cast<VkDeviceSize>(target.width) * target.height * 4;
    auto buffer = device->createBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    if (!buffer) {
        return std::unexpected(buffer.error());
    }
    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = immediatePool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(vk, &allocateInfo, &cmd);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    useImage(cmd, target.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {target.color.width, target.color.height, 1};
    vkCmdCopyImageToBuffer(cmd, target.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buffer->buffer, 1, &region);
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer->buffer;
    barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                         nullptr, 1, &barrier, 0, nullptr);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VkResult result = vkQueueSubmit(device->queue, 1, &submit, VK_NULL_HANDLE);
    if (result == VK_SUCCESS) {
        result = vkQueueWaitIdle(device->queue);
    }
    vkFreeCommandBuffers(vk, immediatePool, 1, &cmd);
    if (result != VK_SUCCESS) {
        device->destroy(*buffer);
        return detail::vkError(result, "Reading pixels back");
    }
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size));
    const auto *data = static_cast<const std::uint8_t *>(buffer->mapped);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        const unsigned a = data[i + 3];
        for (int c = 0; c < 3; ++c) {
            pixels[i + c] = a == 0 ? 0
                                   : static_cast<std::uint8_t>(
                                             std::min(255u, (data[i + c] * 255 + a / 2) / a));
        }
        pixels[i + 3] = static_cast<std::uint8_t>(a);
    }
    device->destroy(*buffer);
    return pixels;
}

}  // namespace filepan::gfx::vulkan
