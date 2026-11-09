// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <vulkan/vulkan.h>

#include "filepan/gfx/vulkan/device.hpp"
#include "filepan/gfx/vulkan/external_image_store.hpp"
#include "filepan/gfx/vulkan/frame_plan.hpp"
#include "filepan/gfx/vulkan/glyph_atlas.hpp"
#include "filepan/gfx/vulkan/renderer.hpp"

// Internal: the state behind Renderer and RenderTarget.
namespace filepan::gfx::vulkan {

namespace detail {

constexpr int kFramesInFlight = 2;
constexpr int kMaxBlurLevels = 6;
// Background blurs kept per target, one per distinct radius.
constexpr std::size_t kMaxBackgroundBlurs = 4;
// Image bytes uploaded per frame at most; the rest wait for later frames.
constexpr std::size_t kImageUploadBudget = 32 << 20;

// The UI and external pipelines' push constants: PushConstants in shaders/common.glsl.
struct UiPush {
    float targetSize[2];      // Logical px.
    std::uint32_t transform;  // Quarter turns clockwise, as RenderTarget::Impl::transform.
    std::uint32_t padding;
};
static_assert(sizeof(UiPush) == 16);

// Render passes and pipelines for one target format.
struct Pipelines {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkRenderPass clearPass = VK_NULL_HANDLE;    // Clears the render area first.
    VkRenderPass loadPass = VK_NULL_HANDLE;     // Keeps what's there.
    VkRenderPass discardPass = VK_NULL_HANDLE;  // For passes that overwrite the whole area.
    VkPipeline ui = VK_NULL_HANDLE;
    VkPipeline external = VK_NULL_HANDLE;  // External images sampled without a conversion.
    VkPipeline blurDown = VK_NULL_HANDLE;
    VkPipeline blurUp = VK_NULL_HANDLE;
};

struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    Buffer instances;
    Buffer clips;
    Buffer staging;
    bool pending = false;  // Submitted and not yet waited for.
    FrameTiming timing;
    // Destroys resources the GPU may still use; run once the fence has signaled.
    std::vector<std::function<void()>> garbage;
};

struct CachedBlur {
    float sigma = 0;
    Image image;  // Half the target's size.
    std::uint64_t hash = 0;
    std::uint64_t lastUsed = 0;
};

// A sampler YCbCr conversion for one kind of hardware buffer (an external format, or a
// multi-planar one) and what sampling through it takes: the conversion must be an immutable
// sampler in the descriptor set layout, so it has its own set layout, pipeline layout, pool and
// pipelines (one per target format).
struct YcbcrVariant {
    std::uint64_t externalFormat = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkSamplerYcbcrModelConversion model{};
    VkSamplerYcbcrRange range{};
    VkComponentMapping components{};
    VkChromaLocation xChroma{};
    VkChromaLocation yChroma{};
    VkFilter chromaFilter = VK_FILTER_NEAREST;

    VkSamplerYcbcrConversion conversion = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> pools;
    std::vector<std::pair<VkFormat, VkPipeline>> pipelines;
};

// An external frame's texture: an imported hardware buffer, or an uploaded pixel frame.
struct ExternalTexture {
    std::uint64_t bufferId = 0;  // AHardwareBuffer_getId; 0 for pixel frames.
    Image image;                 // image.set is in the variant's layout, or the texture layout.
    YcbcrVariant *variant = nullptr;
    VkDescriptorPool pool = VK_NULL_HANDLE;  // The variant pool image.set came from.
    std::uint64_t lastUsed = 0;
};

struct ExternalSlot {
    ExternalFrame frame;  // What's on screen.
    bool changed = false;  // Presented since the last render.
    std::vector<ExternalTexture> textures;
    int current = -1;  // Index into textures, or -1 if nothing can be drawn.
    float uv[4] = {0, 0, 1, 1};
    std::uint32_t version = 0;
};

}  // namespace detail

struct RenderTarget::Impl {
    Renderer::Impl *renderer = nullptr;
    bool offscreen = true;
    // The frame's size, which layout and the frame plan use. The output image holds the frame
    // turned transform quarter turns clockwise (a pre-rotated swapchain image), so for 1 and 3
    // it is height x width.
    int width = 0;
    int height = 0;
    int transform = 0;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    detail::Pipelines *pipelines = nullptr;

    // Offscreen.
    detail::Image color;

    // Surface.
    ANativeWindow *window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<detail::Image> swapchainImages;
    std::vector<VkSemaphore> renderDone;  // Per swapchain image.
    bool recreateSwapchain = false;
    // What the swapchain was made with; a display turned since makes it SUBOPTIMAL.
    VkSurfaceTransformFlagBitsKHR preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    // The window's size when the swapchain was made; a different one means it's stale.
    int windowWidth = 0;
    int windowHeight = 0;

    // Scratch, sized to the target and created when first needed.
    std::vector<detail::Image> layers;
    std::array<detail::Image, detail::kMaxBlurLevels + 1> levels;  // [1..6]: 1/2 .. 1/64 size.
    std::vector<detail::CachedBlur> backgroundBlurs;
    // A rotated target's frame, unrotated, when the frame reads its own base (a live blur over
    // it): blits can't turn an image a quarter, so it's drawn here and composited turned.
    detail::Image unrotated;

    ~Impl();
    void destroyScratch();
};

struct Renderer::Impl {
    explicit Impl(TextSystem &text);
    ~Impl();

    Result<void> init(const RendererOptions &options);
    Result<detail::Pipelines *> pipelinesFor(VkFormat format);
    Result<VkDescriptorSet> allocateTextureSet(VkImageView view);
    void freeTextureSet(VkDescriptorSet set);
    // Creates an image the renderer draws into and samples: a layer, blur level or blur result.
    Result<detail::Image> createScratchImage(RenderTarget::Impl &target, int width, int height);
    void destroyImage(detail::Image &image);
    Result<void> createFrame(detail::Frame &frame);
    void destroyFrame(detail::Frame &frame);
    // Returns whether the buffer was replaced (handles of freed buffers can be reused).
    Result<bool> growBuffer(detail::Buffer &buffer, VkDeviceSize needed, VkBufferUsageFlags usage);
    void updateFrameSet(detail::Frame &frame);
    void finishFrame(detail::Frame &frame);
    void waitIdle();

    Result<std::unique_ptr<RenderTarget>> createOffscreen(int width, int height, int transform);
    Result<void> resizeOffscreen(RenderTarget::Impl &target, int width, int height);
#ifdef __ANDROID__
    Result<std::unique_ptr<RenderTarget>> createSurface(ANativeWindow *window);
    Result<void> createSwapchain(RenderTarget::Impl &target);
    // After a SUBOPTIMAL acquire or present: recreates the swapchain if the display turned.
    void noteSuboptimal(RenderTarget::Impl &target);
    void destroySwapchainImages(RenderTarget::Impl &target);
#endif
    void destroyTarget(RenderTarget::Impl &target);

    Result<void> render(RenderTarget::Impl &target, const DisplayList &list, Color clearColor);
    Result<std::vector<std::uint8_t>> readPixels(RenderTarget::Impl &target);

    // Frame recording, in recorder.cpp.
    struct ImageCopy {
        detail::Image *image;
        VkDeviceSize offset;
    };
    // Non-premultiplied pixels to premultiply into the staging buffer at an ImageCopy's offset.
    struct StagedPixels {
        const std::uint8_t *pixels;
        std::size_t texels;
    };

    // External images, in external_images.cpp. Applies the store's queued work, then makes each
    // changed slot's frame drawable: imports its hardware buffer, or queues its pixels.
    void takeExternalImages(detail::Frame &frame, std::vector<ImageCopy> &copies,
                            std::vector<StagedPixels> &staged, VkDeviceSize &stagingSize);
    void destroyExternalTexture(detail::ExternalTexture &texture);
    void destroyYcbcrVariant(detail::YcbcrVariant &variant);
    [[nodiscard]] std::optional<detail::PlanBuilder::ExternalView> externalView(
            ExternalImageId id) const;
    // The texture to draw for id, or null.
    [[nodiscard]] const detail::ExternalTexture *externalTexture(ExternalImageId id) const;
    // The pipeline that samples through variant into targets of p's format.
    Result<VkPipeline> ycbcrPipeline(detail::YcbcrVariant &variant, const detail::Pipelines &p);
    // Records the queue family transfers that make this frame's newly imported content visible.
    void acquireExternal(VkCommandBuffer cmd);
#ifdef __ANDROID__
    Result<detail::ExternalTexture> importHardwareBuffer(AHardwareBuffer *buffer);
    Result<detail::YcbcrVariant *> ycbcrVariantFor(
            const VkAndroidHardwareBufferFormatPropertiesANDROID &format);
#endif
    Result<void> prepareTarget(RenderTarget::Impl &target, const detail::FramePlan &plan,
                               bool &captureBackground);
    void record(VkCommandBuffer cmd, detail::Frame &frame, RenderTarget::Impl &target,
                detail::Image &output, Color clearColor, bool captureBackground,
                const std::vector<ImageCopy> &imageCopies, VkDeviceSize glyphOffset);

    TextSystem &text;
    std::unique_ptr<detail::Device> device;
    ImageStore images;
    ExternalImageStore externalImages;
    detail::GlyphAtlas atlas;
    detail::PlanBuilder builder;
    detail::FramePlan plan;

    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout frameSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout textureSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout uiLayout = VK_NULL_HANDLE;
    VkPipelineLayout blurLayout = VK_NULL_HANDLE;
    VkShaderModule uiVert = VK_NULL_HANDLE;
    VkShaderModule uiFrag = VK_NULL_HANDLE;
    VkShaderModule fullscreenVert = VK_NULL_HANDLE;
    VkShaderModule blurDownFrag = VK_NULL_HANDLE;
    VkShaderModule blurUpFrag = VK_NULL_HANDLE;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    std::vector<std::unique_ptr<detail::Pipelines>> pipelines;
    VkDescriptorPool framePool = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> texturePools;
    std::unordered_map<VkDescriptorSet, VkDescriptorPool> texturePoolOf;
    VkCommandPool immediatePool = VK_NULL_HANDLE;

    detail::Image alphaAtlas;
    detail::Image colorAtlas;
    detail::Image dummy;  // Bound when a draw samples no aux texture.
    bool atlasCleared = false;

    // Per frame: the target's cached blur for each of plan.backgroundBlurs, and the ones that
    // must be recomputed.
    std::vector<std::size_t> backgroundSlots;
    std::vector<std::size_t> backgroundCompute;
    // The instance that composites target.unrotated onto a rotated target this frame, if any.
    std::optional<std::uint32_t> unrotatedComposite;

    std::unordered_map<ImageId, detail::Image> residentImages;
    std::unordered_set<ImageId> removedBeforeUpload;

    VkShaderModule externalFrag = VK_NULL_HANDLE;
    std::unordered_map<ExternalImageId, detail::ExternalSlot> externalSlots;
    std::vector<std::unique_ptr<detail::YcbcrVariant>> ycbcrVariants;
    // Imported images whose content changed: taken from the foreign queue family this frame.
    std::vector<detail::Image *> externalAcquires;
#ifdef __ANDROID__
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID getHardwareBufferProperties = nullptr;
#endif

    std::array<detail::Frame, detail::kFramesInFlight> frames;
    std::uint64_t frameNumber = 0;
    std::function<void(const FrameTiming &)> timingCallback;
};

}  // namespace filepan::gfx::vulkan
