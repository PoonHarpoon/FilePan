// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Renderer setup and teardown: pipelines, descriptor sets, frames and targets.

#include <algorithm>
#include <cstring>
#include <utility>

#include "filepan/gfx/vulkan/renderer_impl.hpp"

namespace filepan::gfx::vulkan {

namespace {

using detail::Image;

// SPIR-V compiled from shaders/ at build time.
alignas(4) constexpr unsigned char kUiVert[] = {
#embed "ui.vert.spv"
};
alignas(4) constexpr unsigned char kUiFrag[] = {
#embed "ui.frag.spv"
};
alignas(4) constexpr unsigned char kExternalFrag[] = {
#embed "external.frag.spv"
};
alignas(4) constexpr unsigned char kFullscreenVert[] = {
#embed "fullscreen.vert.spv"
};
alignas(4) constexpr unsigned char kBlurDownFrag[] = {
#embed "blur_down.frag.spv"
};
alignas(4) constexpr unsigned char kBlurUpFrag[] = {
#embed "blur_up.frag.spv"
};

constexpr std::uint32_t kTextureSetsPerPool = 256;
constexpr VkImageUsageFlags kScratchUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

Result<VkShaderModule> createShader(VkDevice device, const unsigned char *code, std::size_t size) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = size;
    info.pCode = reinterpret_cast<const std::uint32_t *>(code);
    VkShaderModule module = VK_NULL_HANDLE;
    FILEPAN_VK_TRY(vkCreateShaderModule(device, &info, nullptr, &module));
    return module;
}

Result<VkRenderPass> createRenderPass(VkDevice device, VkFormat format, VkAttachmentLoadOp load) {
    // Layouts stay COLOR_ATTACHMENT_OPTIMAL; useImage() records the transitions around passes.
    VkAttachmentDescription attachment{};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = load;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &attachment;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    VkRenderPass pass = VK_NULL_HANDLE;
    FILEPAN_VK_TRY(vkCreateRenderPass(device, &info, nullptr, &pass));
    return pass;
}

Result<VkPipeline> createPipeline(VkDevice device, VkPipelineCache cache, VkPipelineLayout layout,
                                  VkRenderPass pass, VkShaderModule vertex,
                                  VkShaderModule fragment, bool blend, bool strip) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology =
            strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (blend) {
        // Premultiplied source over.
        attachment.blendEnable = VK_TRUE;
        attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        attachment.colorBlendOp = VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo blendState{};
    blendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blendState.attachmentCount = 1;
    blendState.pAttachments = &attachment;
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;
    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blendState;
    info.pDynamicState = &dynamic;
    info.layout = layout;
    info.renderPass = pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    FILEPAN_VK_TRY(vkCreateGraphicsPipelines(device, cache, 1, &info, nullptr, &pipeline));
    return pipeline;
}

}  // namespace

// ImageStore.

ImageId ImageStore::add(int width, int height, std::vector<std::uint8_t> pixels) {
    if (width <= 0 || height <= 0 ||
        pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4) {
        return 0;
    }
    const ImageId id = nextId_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(mutex_);
    pending_.uploads.push_back({id, width, height, std::move(pixels)});
    return id;
}

bool ImageStore::update(ImageId id, int width, int height, std::vector<std::uint8_t> pixels) {
    if (id == 0 || width <= 0 || height <= 0 ||
        pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4) {
        return false;
    }
    std::lock_guard lock(mutex_);
    // A queued add or update of the same image that the renderer hasn't taken yet only needs
    // the newest pixels. Not past a queued removal, though, which must win.
    if (std::find(pending_.removals.begin(), pending_.removals.end(), id) ==
        pending_.removals.end()) {
        for (auto it = pending_.uploads.rbegin(); it != pending_.uploads.rend(); ++it) {
            if (it->id == id) {
                it->width = width;
                it->height = height;
                it->pixels = std::move(pixels);
                return true;
            }
        }
    }
    pending_.uploads.push_back({id, width, height, std::move(pixels), true});
    return true;
}

void ImageStore::remove(ImageId id) {
    if (id == 0) {
        return;
    }
    std::lock_guard lock(mutex_);
    pending_.removals.push_back(id);
}

ImageStore::Pending ImageStore::takePending() {
    std::lock_guard lock(mutex_);
    return std::exchange(pending_, {});
}

void ImageStore::requeue(std::vector<Upload> uploads) {
    std::lock_guard lock(mutex_);
    uploads.insert(uploads.end(), std::make_move_iterator(pending_.uploads.begin()),
                   std::make_move_iterator(pending_.uploads.end()));
    pending_.uploads = std::move(uploads);
}

// Renderer::Impl setup.

Renderer::Impl::Impl(TextSystem &text)
    : text(text),
      builder(
              text, atlas,
              [this](ImageId id) -> std::uint32_t {
                  const auto it = residentImages.find(id);
                  return it == residentImages.end() ? 0 : it->second.version;
              },
              [this](ExternalImageId id) { return externalView(id); }) {}

Result<void> Renderer::Impl::init(const RendererOptions &options) {
    auto created = detail::Device::create(options.validation);
    if (!created) {
        return std::unexpected(created.error());
    }
    device = std::move(*created);
    VkDevice vk = device->device;
    externalImages.setSupported(device->hardwareBuffersSupported);
#ifdef __ANDROID__
    if (device->hardwareBuffersSupported) {
        getHardwareBufferProperties = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
                vkGetDeviceProcAddr(vk, "vkGetAndroidHardwareBufferPropertiesANDROID"));
        externalImages.setSupported(getHardwareBufferProperties != nullptr);
    }
#endif

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0;
    FILEPAN_VK_TRY(vkCreateSampler(vk, &samplerInfo, nullptr, &sampler));

    VkDescriptorSetLayoutBinding frameBindings[4]{};
    for (std::uint32_t i = 0; i < 4; ++i) {
        frameBindings[i].binding = i;
        frameBindings[i].descriptorCount = 1;
        frameBindings[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                                : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        frameBindings[i].stageFlags =
                i == 0 ? VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                       : VK_SHADER_STAGE_FRAGMENT_BIT;
        if (i >= 2) {
            frameBindings[i].pImmutableSamplers = &sampler;
        }
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 4;
    layoutInfo.pBindings = frameBindings;
    FILEPAN_VK_TRY(vkCreateDescriptorSetLayout(vk, &layoutInfo, nullptr, &frameSetLayout));

    VkDescriptorSetLayoutBinding textureBinding{};
    textureBinding.binding = 0;
    textureBinding.descriptorCount = 1;
    textureBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    textureBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    textureBinding.pImmutableSamplers = &sampler;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &textureBinding;
    FILEPAN_VK_TRY(vkCreateDescriptorSetLayout(vk, &layoutInfo, nullptr, &textureSetLayout));

    VkPushConstantRange uiPush{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(detail::UiPush)};
    const VkDescriptorSetLayout uiSets[] = {frameSetLayout, textureSetLayout};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 2;
    pipelineLayoutInfo.pSetLayouts = uiSets;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &uiPush;
    FILEPAN_VK_TRY(vkCreatePipelineLayout(vk, &pipelineLayoutInfo, nullptr, &uiLayout));
    VkPushConstantRange blurPush{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 28};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &textureSetLayout;
    pipelineLayoutInfo.pPushConstantRanges = &blurPush;
    FILEPAN_VK_TRY(vkCreatePipelineLayout(vk, &pipelineLayoutInfo, nullptr, &blurLayout));

    const auto shader = [vk](const unsigned char *code, std::size_t size,
                             VkShaderModule &module) -> Result<void> {
        auto created = createShader(vk, code, size);
        if (!created) {
            return std::unexpected(created.error());
        }
        module = *created;
        return {};
    };
    for (auto result : {shader(kUiVert, sizeof(kUiVert), uiVert),
                        shader(kUiFrag, sizeof(kUiFrag), uiFrag),
                        shader(kExternalFrag, sizeof(kExternalFrag), externalFrag),
                        shader(kFullscreenVert, sizeof(kFullscreenVert), fullscreenVert),
                        shader(kBlurDownFrag, sizeof(kBlurDownFrag), blurDownFrag),
                        shader(kBlurUpFrag, sizeof(kBlurUpFrag), blurUpFrag)}) {
        if (!result) {
            return result;
        }
    }

    VkPipelineCacheCreateInfo cacheInfo{};
    cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    cacheInfo.initialDataSize = options.pipelineCache.size();
    cacheInfo.pInitialData = options.pipelineCache.data();
    if (vkCreatePipelineCache(vk, &cacheInfo, nullptr, &pipelineCache) != VK_SUCCESS) {
        // Stale or foreign cache data: start empty.
        cacheInfo.initialDataSize = 0;
        cacheInfo.pInitialData = nullptr;
        FILEPAN_VK_TRY(vkCreatePipelineCache(vk, &cacheInfo, nullptr, &pipelineCache));
    }
    // Offscreen targets and most Android swapchains use RGBA8; others are created on demand.
    if (auto created = pipelinesFor(VK_FORMAT_R8G8B8A8_UNORM); !created) {
        return std::unexpected(created.error());
    }

    VkDescriptorPoolSize poolSizes[] = {
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * detail::kFramesInFlight},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * detail::kFramesInFlight}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = detail::kFramesInFlight;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    FILEPAN_VK_TRY(vkCreateDescriptorPool(vk, &poolInfo, nullptr, &framePool));

    VkCommandPoolCreateInfo commandPoolInfo{};
    commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    commandPoolInfo.queueFamilyIndex = device->queueFamily;
    FILEPAN_VK_TRY(vkCreateCommandPool(vk, &commandPoolInfo, nullptr, &immediatePool));

    constexpr auto kAtlasUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const auto page = static_cast<std::uint32_t>(detail::GlyphAtlas::kPageSize);
    auto alpha = device->createImage(page, page, detail::GlyphAtlas::kAlphaPages,
                                     VK_FORMAT_R8_UNORM, kAtlasUsage);
    if (!alpha) {
        return std::unexpected(alpha.error());
    }
    alphaAtlas = *alpha;
    auto color = device->createImage(page, page, 1, VK_FORMAT_R8G8B8A8_UNORM, kAtlasUsage);
    if (!color) {
        return std::unexpected(color.error());
    }
    colorAtlas = *color;
    auto blank = device->createImage(1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, kAtlasUsage);
    if (!blank) {
        return std::unexpected(blank.error());
    }
    dummy = *blank;
    auto dummySet = allocateTextureSet(dummy.view);
    if (!dummySet) {
        return std::unexpected(dummySet.error());
    }
    dummy.set = *dummySet;

    for (detail::Frame &frame : frames) {
        if (auto result = createFrame(frame); !result) {
            return result;
        }
    }
    return {};
}

Result<detail::Pipelines *> Renderer::Impl::pipelinesFor(VkFormat format) {
    for (const auto &existing : pipelines) {
        if (existing->format == format) {
            return existing.get();
        }
    }
    VkDevice vk = device->device;
    auto set = std::make_unique<detail::Pipelines>();
    set->format = format;
    // Pushed first so the destructor cleans up whatever gets created if a step fails.
    detail::Pipelines &p = *pipelines.emplace_back(std::move(set));
    auto clear = createRenderPass(vk, format, VK_ATTACHMENT_LOAD_OP_CLEAR);
    if (!clear) {
        return std::unexpected(clear.error());
    }
    p.clearPass = *clear;
    auto load = createRenderPass(vk, format, VK_ATTACHMENT_LOAD_OP_LOAD);
    if (!load) {
        return std::unexpected(load.error());
    }
    p.loadPass = *load;
    auto discard = createRenderPass(vk, format, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    if (!discard) {
        return std::unexpected(discard.error());
    }
    p.discardPass = *discard;
    auto ui = createPipeline(vk, pipelineCache, uiLayout, p.clearPass, uiVert, uiFrag, true, true);
    if (!ui) {
        return std::unexpected(ui.error());
    }
    p.ui = *ui;
    auto external = createPipeline(vk, pipelineCache, uiLayout, p.clearPass, uiVert, externalFrag,
                                   true, true);
    if (!external) {
        return std::unexpected(external.error());
    }
    p.external = *external;
    auto down = createPipeline(vk, pipelineCache, blurLayout, p.clearPass, fullscreenVert,
                               blurDownFrag, false, false);
    if (!down) {
        return std::unexpected(down.error());
    }
    p.blurDown = *down;
    auto up = createPipeline(vk, pipelineCache, blurLayout, p.clearPass, fullscreenVert,
                             blurUpFrag, false, false);
    if (!up) {
        return std::unexpected(up.error());
    }
    p.blurUp = *up;
    return &p;
}

Result<VkPipeline> Renderer::Impl::ycbcrPipeline(detail::YcbcrVariant &variant,
                                                 const detail::Pipelines &p) {
    for (const auto &[format, pipeline] : variant.pipelines) {
        if (format == p.format) {
            return pipeline;
        }
    }
    auto created = createPipeline(device->device, pipelineCache, variant.layout, p.clearPass, uiVert,
                                  externalFrag, true, true);
    if (!created) {
        return created;
    }
    variant.pipelines.emplace_back(p.format, *created);
    return *created;
}

Result<VkDescriptorSet> Renderer::Impl::allocateTextureSet(VkImageView view) {
    VkDevice vk = device->device;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &textureSetLayout;
    bool allocated = false;
    for (VkDescriptorPool pool : texturePools) {
        info.descriptorPool = pool;
        if (vkAllocateDescriptorSets(vk, &info, &set) == VK_SUCCESS) {
            allocated = true;
            texturePoolOf[set] = pool;
            break;
        }
    }
    if (!allocated) {
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kTextureSetsPerPool};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = kTextureSetsPerPool;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &size;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        FILEPAN_VK_TRY(vkCreateDescriptorPool(vk, &poolInfo, nullptr, &pool));
        texturePools.push_back(pool);
        info.descriptorPool = pool;
        FILEPAN_VK_TRY(vkAllocateDescriptorSets(vk, &info, &set));
        texturePoolOf[set] = pool;
    }
    VkDescriptorImageInfo image{};
    image.imageView = view;
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(vk, 1, &write, 0, nullptr);
    return set;
}

void Renderer::Impl::freeTextureSet(VkDescriptorSet set) {
    if (set == VK_NULL_HANDLE) {
        return;
    }
    if (auto it = texturePoolOf.find(set); it != texturePoolOf.end()) {
        vkFreeDescriptorSets(device->device, it->second, 1, &set);
        texturePoolOf.erase(it);
    }
}

void Renderer::Impl::destroyImage(Image &image) {
    freeTextureSet(image.set);
    device->destroy(image);
}

Result<Image> Renderer::Impl::createScratchImage(RenderTarget::Impl &target, int width,
                                                 int height) {
    auto created = device->createImage(static_cast<std::uint32_t>(width),
                                       static_cast<std::uint32_t>(height), 1, target.format,
                                       kScratchUsage);
    if (!created) {
        return created;
    }
    Image image = *created;
    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = target.pipelines->clearPass;
    info.attachmentCount = 1;
    info.pAttachments = &image.view;
    info.width = image.width;
    info.height = image.height;
    info.layers = 1;
    if (VkResult result = vkCreateFramebuffer(device->device, &info, nullptr, &image.framebuffer);
        result != VK_SUCCESS) {
        device->destroy(image);
        return detail::vkError(result, "vkCreateFramebuffer");
    }
    auto set = allocateTextureSet(image.view);
    if (!set) {
        device->destroy(image);
        return std::unexpected(set.error());
    }
    image.set = *set;
    return image;
}

Result<void> Renderer::Impl::createFrame(detail::Frame &frame) {
    VkDevice vk = device->device;
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = device->queueFamily;
    FILEPAN_VK_TRY(vkCreateCommandPool(vk, &poolInfo, nullptr, &frame.pool));
    VkCommandBufferAllocateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    bufferInfo.commandPool = frame.pool;
    bufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    bufferInfo.commandBufferCount = 1;
    FILEPAN_VK_TRY(vkAllocateCommandBuffers(vk, &bufferInfo, &frame.cmd));
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    FILEPAN_VK_TRY(vkCreateFence(vk, &fenceInfo, nullptr, &frame.fence));
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    FILEPAN_VK_TRY(vkCreateSemaphore(vk, &semaphoreInfo, nullptr, &frame.acquired));
    if (device->timestampsSupported) {
        VkQueryPoolCreateInfo queryInfo{};
        queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = 2;
        FILEPAN_VK_TRY(vkCreateQueryPool(vk, &queryInfo, nullptr, &frame.queries));
    }
    VkDescriptorSetAllocateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setInfo.descriptorPool = framePool;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &frameSetLayout;
    FILEPAN_VK_TRY(vkAllocateDescriptorSets(vk, &setInfo, &frame.set));
    constexpr VkBufferUsageFlags kStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    for (auto [buffer, size, usage] :
         {std::tuple{&frame.instances, VkDeviceSize{64 << 10}, kStorage},
          std::tuple{&frame.clips, VkDeviceSize{4 << 10}, kStorage},
          std::tuple{&frame.staging, VkDeviceSize{1 << 20},
                     VkBufferUsageFlags{VK_BUFFER_USAGE_TRANSFER_SRC_BIT}}}) {
        if (auto result = growBuffer(*buffer, size, usage); !result) {
            return std::unexpected(result.error());
        }
    }
    updateFrameSet(frame);
    return {};
}

Result<bool> Renderer::Impl::growBuffer(detail::Buffer &buffer, VkDeviceSize needed,
                                        VkBufferUsageFlags usage) {
    if (buffer.size >= needed && buffer.buffer != VK_NULL_HANDLE) {
        return false;
    }
    // Only called for a frame whose fence has signaled, so the old buffer is idle.
    device->destroy(buffer);
    VkDeviceSize size = 4096;
    while (size < needed) {
        size *= 2;
    }
    auto created = device->createBuffer(size, usage, true);
    if (!created) {
        return std::unexpected(created.error());
    }
    buffer = *created;
    return true;
}

void Renderer::Impl::updateFrameSet(detail::Frame &frame) {
    VkDescriptorBufferInfo buffers[2]{};
    buffers[0] = {frame.instances.buffer, 0, VK_WHOLE_SIZE};
    buffers[1] = {frame.clips.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo images[2]{};
    images[0] = {VK_NULL_HANDLE, alphaAtlas.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    images[1] = {VK_NULL_HANDLE, colorAtlas.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[4]{};
    for (std::uint32_t i = 0; i < 4; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = frame.set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        if (i < 2) {
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &buffers[i];
        } else {
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &images[i - 2];
        }
    }
    vkUpdateDescriptorSets(device->device, 4, writes, 0, nullptr);
}

void Renderer::Impl::finishFrame(detail::Frame &frame) {
    if (!frame.pending) {
        return;
    }
    VkDevice vk = device->device;
    vkWaitForFences(vk, 1, &frame.fence, VK_TRUE, UINT64_MAX);
    vkResetFences(vk, 1, &frame.fence);
    frame.pending = false;
    if (frame.queries != VK_NULL_HANDLE) {
        std::uint64_t stamps[2] = {};
        if (vkGetQueryPoolResults(vk, frame.queries, 0, 2, sizeof(stamps), stamps,
                                  sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            frame.timing.gpuNs = static_cast<std::int64_t>(
                    static_cast<double>(stamps[1] - stamps[0]) *
                    device->properties.limits.timestampPeriod);
        }
    }
    for (auto &destroy : frame.garbage) {
        destroy();
    }
    frame.garbage.clear();
    if (timingCallback) {
        timingCallback(frame.timing);
    }
}

void Renderer::Impl::destroyFrame(detail::Frame &frame) {
    VkDevice vk = device->device;
    finishFrame(frame);
    device->destroy(frame.instances);
    device->destroy(frame.clips);
    device->destroy(frame.staging);
    if (frame.queries != VK_NULL_HANDLE) {
        vkDestroyQueryPool(vk, frame.queries, nullptr);
    }
    if (frame.acquired != VK_NULL_HANDLE) {
        vkDestroySemaphore(vk, frame.acquired, nullptr);
    }
    if (frame.fence != VK_NULL_HANDLE) {
        vkDestroyFence(vk, frame.fence, nullptr);
    }
    if (frame.pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(vk, frame.pool, nullptr);
    }
    frame = {};
}

void Renderer::Impl::waitIdle() {
    if (device == nullptr) {
        return;
    }
    for (detail::Frame &frame : frames) {
        finishFrame(frame);
    }
    vkDeviceWaitIdle(device->device);
}

Renderer::Impl::~Impl() {
    if (device == nullptr) {
        return;
    }
    waitIdle();
    VkDevice vk = device->device;
    for (detail::Frame &frame : frames) {
        destroyFrame(frame);
    }
    for (auto &[id, image] : residentImages) {
        device->destroy(image);
    }
    for (auto &[id, slot] : externalSlots) {
        for (detail::ExternalTexture &texture : slot.textures) {
            destroyExternalTexture(texture);
        }
        slot.frame.reset();
    }
    externalSlots.clear();
    for (auto &variant : ycbcrVariants) {
        destroyYcbcrVariant(*variant);
    }
    ycbcrVariants.clear();
    device->destroy(alphaAtlas);
    device->destroy(colorAtlas);
    device->destroy(dummy);
    for (VkDescriptorPool pool : texturePools) {
        vkDestroyDescriptorPool(vk, pool, nullptr);
    }
    if (framePool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(vk, framePool, nullptr);
    }
    if (immediatePool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(vk, immediatePool, nullptr);
    }
    for (const auto &p : pipelines) {
        for (VkPipeline pipeline : {p->ui, p->external, p->blurDown, p->blurUp}) {
            if (pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(vk, pipeline, nullptr);
            }
        }
        for (VkRenderPass pass : {p->clearPass, p->loadPass, p->discardPass}) {
            if (pass != VK_NULL_HANDLE) {
                vkDestroyRenderPass(vk, pass, nullptr);
            }
        }
    }
    for (VkShaderModule module :
         {uiVert, uiFrag, externalFrag, fullscreenVert, blurDownFrag, blurUpFrag}) {
        if (module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(vk, module, nullptr);
        }
    }
    if (pipelineCache != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(vk, pipelineCache, nullptr);
    }
    for (VkPipelineLayout layout : {uiLayout, blurLayout}) {
        if (layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(vk, layout, nullptr);
        }
    }
    for (VkDescriptorSetLayout layout : {frameSetLayout, textureSetLayout}) {
        if (layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(vk, layout, nullptr);
        }
    }
    if (sampler != VK_NULL_HANDLE) {
        vkDestroySampler(vk, sampler, nullptr);
    }
}

// Targets.

void RenderTarget::Impl::destroyScratch() {
    for (Image &layer : layers) {
        renderer->destroyImage(layer);
    }
    layers.clear();
    for (Image &level : levels) {
        renderer->destroyImage(level);
    }
    for (detail::CachedBlur &blur : backgroundBlurs) {
        renderer->destroyImage(blur.image);
    }
    backgroundBlurs.clear();
    renderer->destroyImage(unrotated);
}

RenderTarget::Impl::~Impl() {
    if (renderer != nullptr) {
        renderer->destroyTarget(*this);
    }
}

void Renderer::Impl::destroyTarget(RenderTarget::Impl &target) {
    waitIdle();
    target.destroyScratch();
    destroyImage(target.color);
#ifdef __ANDROID__
    destroySwapchainImages(target);
    if (target.swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device->device, target.swapchain, nullptr);
    }
    if (target.surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(device->instance, target.surface, nullptr);
    }
#endif
}

Result<std::unique_ptr<RenderTarget>> Renderer::Impl::createOffscreen(int width, int height,
                                                                     int transform) {
    auto impl = std::make_unique<RenderTarget::Impl>();
    impl->renderer = this;
    impl->offscreen = true;
    impl->transform = transform & 3;
    impl->format = VK_FORMAT_R8G8B8A8_UNORM;
    auto pipelineSet = pipelinesFor(impl->format);
    if (!pipelineSet) {
        return std::unexpected(pipelineSet.error());
    }
    impl->pipelines = *pipelineSet;
    if (auto result = resizeOffscreen(*impl, width, height); !result) {
        return std::unexpected(result.error());
    }
    return std::unique_ptr<RenderTarget>(new RenderTarget(std::move(impl)));
}

Result<void> Renderer::Impl::resizeOffscreen(RenderTarget::Impl &target, int width, int height) {
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        return std::unexpected(Error{0, "Invalid target size"});
    }
    if (target.color.image != VK_NULL_HANDLE) {
        waitIdle();
        target.destroyScratch();
        destroyImage(target.color);
    }
    target.width = width;
    target.height = height;
    const bool turned = target.transform % 2 != 0;
    auto color = createScratchImage(target, turned ? height : width, turned ? width : height);
    if (!color) {
        return std::unexpected(color.error());
    }
    target.color = *color;
    return {};
}

// Public API.

RenderTarget::RenderTarget(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

RenderTarget::~RenderTarget() = default;

Size RenderTarget::size() const {
    return {static_cast<float>(impl_->width), static_cast<float>(impl_->height)};
}

void RenderTarget::resize(int width, int height) {
    if (impl_->offscreen) {
        if (width != impl_->width || height != impl_->height) {
            // On failure the target is left empty and render() reports it.
            (void)impl_->renderer->resizeOffscreen(*impl_, width, height);
        }
    } else {
        impl_->recreateSwapchain = true;
    }
}

Renderer::Renderer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Renderer::~Renderer() = default;

Result<std::unique_ptr<Renderer>> Renderer::create(TextSystem &text,
                                                   const RendererOptions &options) {
    auto impl = std::make_unique<Impl>(text);
    if (auto result = impl->init(options); !result) {
        return std::unexpected(result.error());
    }
    return std::unique_ptr<Renderer>(new Renderer(std::move(impl)));
}

ImageStore &Renderer::images() { return impl_->images; }

ExternalImageStore &Renderer::externalImages() { return impl_->externalImages; }

Result<std::unique_ptr<RenderTarget>> Renderer::createOffscreenTarget(int width, int height,
                                                                     int quarterTurns) {
    return impl_->createOffscreen(width, height, quarterTurns);
}

#ifdef __ANDROID__
Result<std::unique_ptr<RenderTarget>> Renderer::createSurfaceTarget(ANativeWindow *window) {
    return impl_->createSurface(window);
}
#endif

Result<void> Renderer::render(RenderTarget &target, const DisplayList &list, Color clearColor) {
    return impl_->render(*target.impl_, list, clearColor);
}

Result<std::vector<std::uint8_t>> Renderer::readPixels(RenderTarget &target) {
    return impl_->readPixels(*target.impl_);
}

void Renderer::setFrameTimingCallback(std::function<void(const FrameTiming &)> callback) {
    impl_->timingCallback = std::move(callback);
}

std::vector<std::uint8_t> Renderer::pipelineCacheData() const {
    std::size_t size = 0;
    VkDevice vk = impl_->device->device;
    if (vkGetPipelineCacheData(vk, impl_->pipelineCache, &size, nullptr) != VK_SUCCESS) {
        return {};
    }
    std::vector<std::uint8_t> data(size);
    if (vkGetPipelineCacheData(vk, impl_->pipelineCache, &size, data.data()) != VK_SUCCESS) {
        return {};
    }
    data.resize(size);
    return data;
}

void Renderer::waitIdle() { impl_->waitIdle(); }

std::uint32_t Renderer::validationErrorCount() const {
    return impl_->device->validationErrors.load(std::memory_order_relaxed);
}

std::string Renderer::deviceName() const { return impl_->device->properties.deviceName; }

}  // namespace filepan::gfx::vulkan
