// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// External images: frames from outside the renderer (video), as imported AHardwareBuffers
// sampled through a YCbCr conversion, or as uploaded pixels.
//
// A slot holds the frame on screen. When a new one is presented, the old one is released from
// the garbage of the frame that replaced it, so only after the GPU has finished every frame
// that sampled it (they were all submitted before). Imports are cached by buffer: a decoder
// cycles through a few buffers, and importing costs an allocation and a descriptor set.

#include <algorithm>
#include <cstdio>
#include <memory>
#include <unordered_set>

#include "filepan/gfx/vulkan/renderer_impl.hpp"

#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#include <android/log.h>
#endif

namespace filepan::gfx::vulkan {

namespace {

// Imports not drawn for this many frames are freed.
constexpr std::uint64_t kExternalTextureIdleFrames = 120;
#ifdef __ANDROID__
// Descriptor sets per pool for one YCbCr variant. A multi-planar format may take up to three
// descriptors per set.
constexpr std::uint32_t kYcbcrSetsPerPool = 16;
#endif

void logWarning(const char *message) {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_WARN, "FilePanVulkan", "%s", message);
#else
    std::fprintf(stderr, "vulkan: %s\n", message);
#endif
}

}  // namespace

// --- ExternalFrame ---

ExternalFrame ExternalFrame::fromPixels(int width, int height, std::vector<std::uint8_t> pixels, Rect crop,
                                        std::function<void()> release) {
    ExternalFrame frame;
    if (width <= 0 || height <= 0 ||
        pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4) {
        if (release) {
            release();
        }
        return frame;
    }
    frame.pixels_ = std::make_shared<const std::vector<std::uint8_t>>(std::move(pixels));
    frame.width_ = width;
    frame.height_ = height;
    frame.crop_ = crop;
    frame.release_ = std::move(release);
    return frame;
}

ExternalFrame &ExternalFrame::operator=(ExternalFrame &&other) noexcept {
    if (this != &other) {
        reset();
        buffer_ = std::exchange(other.buffer_, nullptr);
        pixels_ = std::move(other.pixels_);
        width_ = std::exchange(other.width_, 0);
        height_ = std::exchange(other.height_, 0);
        crop_ = other.crop_;
        release_ = std::move(other.release_);
        other.release_ = nullptr;
    }
    return *this;
}

void ExternalFrame::reset() {
    buffer_ = nullptr;
    pixels_.reset();
    width_ = 0;
    height_ = 0;
    if (release_) {
        auto release = std::move(release_);
        release_ = nullptr;
        release();
    }
}

// --- ExternalImageStore ---

ExternalImageId ExternalImageStore::create() {
    std::lock_guard lock(mutex_);
    const ExternalImageId id = nextId_++;
    live_.insert(id);
    return id;
}

void ExternalImageStore::present(ExternalImageId id, ExternalFrame frame) {
    {
        std::lock_guard lock(mutex_);
        if (live_.contains(id)) {
            pending_.push_back({id, std::move(frame)});
            return;
        }
    }
    frame.reset();  // Outside the lock: release may do anything.
}

void ExternalImageStore::destroy(ExternalImageId id) {
    std::lock_guard lock(mutex_);
    if (live_.erase(id) > 0) {
        pending_.push_back({id, std::nullopt});
    }
}

std::vector<ExternalImageStore::Op> ExternalImageStore::takePending() {
    std::lock_guard lock(mutex_);
    return std::exchange(pending_, {});
}

// --- Renderer::Impl ---

void Renderer::Impl::takeExternalImages(detail::Frame &frame, std::vector<ImageCopy> &copies,
                                        std::vector<StagedPixels> &staged, VkDeviceSize &stagingSize) {
    externalAcquires.clear();
    std::vector<ExternalImageStore::Op> ops = externalImages.takePending();
    for (ExternalImageStore::Op &op : ops) {
        if (!op.frame) {
            auto it = externalSlots.find(op.id);
            if (it == externalSlots.end()) {
                continue;
            }
            auto slot = std::make_shared<detail::ExternalSlot>(std::move(it->second));
            externalSlots.erase(it);
            frame.garbage.push_back([this, slot] {
                for (detail::ExternalTexture &texture : slot->textures) {
                    destroyExternalTexture(texture);
                }
                slot->frame.reset();
            });
            continue;
        }
        detail::ExternalSlot &slot = externalSlots[op.id];
        if (slot.changed) {
            slot.frame.reset();  // Replaced before it was ever drawn.
        } else if (!slot.frame.empty()) {
            auto old = std::make_shared<ExternalFrame>(std::move(slot.frame));
            frame.garbage.push_back([old] { old->reset(); });
        }
        slot.frame = std::move(*op.frame);
        slot.changed = true;
    }

    for (auto &[id, slot] : externalSlots) {
        // Imports not drawn for a while go, except the one on screen.
        for (std::size_t i = slot.textures.size(); i-- > 0;) {
            if (static_cast<int>(i) != slot.current &&
                slot.textures[i].lastUsed + kExternalTextureIdleFrames < frameNumber) {
                auto texture = std::make_shared<detail::ExternalTexture>(slot.textures[i]);
                frame.garbage.push_back([this, texture] { destroyExternalTexture(*texture); });
                slot.textures.erase(slot.textures.begin() + static_cast<std::ptrdiff_t>(i));
                if (slot.current > static_cast<int>(i)) {
                    --slot.current;
                }
            }
        }
        if (slot.current >= 0) {
            slot.textures[static_cast<std::size_t>(slot.current)].lastUsed = frameNumber;
        }
        if (!slot.changed) {
            continue;
        }
        slot.changed = false;
        slot.current = -1;
        ++slot.version;
        const ExternalFrame &shown = slot.frame;
        float width = 0;
        float height = 0;
        if (shown.pixels()) {
            // Uploaded into the slot's pixel texture, recreated when the size changes.
            const auto w = static_cast<std::uint32_t>(shown.width());
            const auto h = static_cast<std::uint32_t>(shown.height());
            auto found = std::ranges::find_if(slot.textures,
                                              [](const auto &t) { return t.bufferId == 0; });
            if (found != slot.textures.end() &&
                (found->image.width != w || found->image.height != h)) {
                auto texture = std::make_shared<detail::ExternalTexture>(*found);
                frame.garbage.push_back([this, texture] { destroyExternalTexture(*texture); });
                slot.textures.erase(found);
                found = slot.textures.end();
            }
            if (found == slot.textures.end()) {
                auto image = device->createImage(w, h, 1, VK_FORMAT_R8G8B8A8_UNORM,
                                                 VK_IMAGE_USAGE_SAMPLED_BIT |
                                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                if (!image) {
                    logWarning(image.error().message.c_str());
                    continue;
                }
                auto set = allocateTextureSet(image->view);
                if (!set) {
                    device->destroy(*image);
                    logWarning(set.error().message.c_str());
                    continue;
                }
                image->set = *set;
                slot.textures.push_back({0, *image, nullptr, VK_NULL_HANDLE, frameNumber});
                found = slot.textures.end() - 1;
            }
            const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;
            copies.push_back({&found->image, stagingSize});
            staged.push_back({shown.pixels()->data(), static_cast<std::size_t>(w) * h});
            stagingSize += (bytes + 15) & ~VkDeviceSize{15};
            slot.current = static_cast<int>(found - slot.textures.begin());
            width = static_cast<float>(w);
            height = static_cast<float>(h);
        } else if (shown.buffer() != nullptr) {
#ifdef __ANDROID__
            if (!externalImages.supported()) {
                slot.frame.reset();
                continue;
            }
            const std::uint64_t bufferId = [&] {
                std::uint64_t value = 0;
                return AHardwareBuffer_getId(shown.buffer(), &value) == 0 ? value : 0;
            }();
            auto found = bufferId == 0 ? slot.textures.end()
                                       : std::ranges::find_if(slot.textures, [&](const auto &t) {
                                             return t.bufferId == bufferId;
                                         });
            if (found == slot.textures.end()) {
                auto imported = importHardwareBuffer(shown.buffer());
                if (!imported) {
                    logWarning(imported.error().message.c_str());
                    slot.frame.reset();
                    continue;
                }
                imported->bufferId = bufferId;
                slot.textures.push_back(*imported);
                found = slot.textures.end() - 1;
            }
            // New content in it: take it from the decoder's side.
            found->image.state = {};
            externalAcquires.push_back(&found->image);
            slot.current = static_cast<int>(found - slot.textures.begin());
            width = static_cast<float>(found->image.width);
            height = static_cast<float>(found->image.height);
#else
            slot.frame.reset();  // No hardware buffers here.
            continue;
#endif
        } else {
            continue;
        }
        slot.textures[static_cast<std::size_t>(slot.current)].lastUsed = frameNumber;
        // The crop in texture coordinates. Sides inside the buffer pull in half a texel so
        // filtering never reaches the padding past them (decoders align heights up to 16).
        Rect crop = shown.crop();
        if (crop.empty()) {
            crop = {0, 0, width, height};
        }
        const float x0 = std::clamp(crop.x, 0.0f, width);
        const float y0 = std::clamp(crop.y, 0.0f, height);
        const float x1 = std::clamp(crop.x + crop.width, x0, width);
        const float y1 = std::clamp(crop.y + crop.height, y0, height);
        slot.uv[0] = (x0 > 0 ? x0 + 0.5f : x0) / width;
        slot.uv[1] = (y0 > 0 ? y0 + 0.5f : y0) / height;
        slot.uv[2] = (x1 < width ? x1 - 0.5f : x1) / width;
        slot.uv[3] = (y1 < height ? y1 - 0.5f : y1) / height;
    }
}

std::optional<detail::PlanBuilder::ExternalView> Renderer::Impl::externalView(
        ExternalImageId id) const {
    const auto it = externalSlots.find(id);
    if (it == externalSlots.end() || it->second.current < 0) {
        return std::nullopt;
    }
    detail::PlanBuilder::ExternalView view;
    std::copy_n(it->second.uv, 4, view.uv);
    view.version = it->second.version;
    return view;
}

const detail::ExternalTexture *Renderer::Impl::externalTexture(ExternalImageId id) const {
    const auto it = externalSlots.find(id);
    if (it == externalSlots.end() || it->second.current < 0) {
        return nullptr;
    }
    return &it->second.textures[static_cast<std::size_t>(it->second.current)];
}

void Renderer::Impl::acquireExternal(VkCommandBuffer cmd) {
    if (externalAcquires.empty()) {
        return;
    }
    std::vector<VkImageMemoryBarrier> barriers;
    for (detail::Image *image : externalAcquires) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        // The decoder wrote it outside Vulkan; the content is kept across this transition
        // the way Android's own compositors rely on.
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
#ifdef __ANDROID__
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        barrier.dstQueueFamilyIndex = device->queueFamily;
#else
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
#endif
        barrier.image = image->image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barriers.push_back(barrier);
        image->state = {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                         static_cast<std::uint32_t>(barriers.size()), barriers.data());
    externalAcquires.clear();
}

void Renderer::Impl::destroyExternalTexture(detail::ExternalTexture &texture) {
    VkDevice vk = device->device;
    if (texture.variant != nullptr) {
        if (texture.image.set != VK_NULL_HANDLE && texture.pool != VK_NULL_HANDLE) {
            vkFreeDescriptorSets(vk, texture.pool, 1, &texture.image.set);
        }
        texture.image.set = VK_NULL_HANDLE;
        device->destroy(texture.image);
    } else {
        destroyImage(texture.image);
    }
    texture = {};
}

void Renderer::Impl::destroyYcbcrVariant(detail::YcbcrVariant &variant) {
    VkDevice vk = device->device;
    for (auto &[format, pipeline] : variant.pipelines) {
        vkDestroyPipeline(vk, pipeline, nullptr);
    }
    for (VkDescriptorPool pool : variant.pools) {
        vkDestroyDescriptorPool(vk, pool, nullptr);
    }
    if (variant.layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk, variant.layout, nullptr);
    }
    if (variant.setLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(vk, variant.setLayout, nullptr);
    }
    if (variant.sampler != VK_NULL_HANDLE) {
        vkDestroySampler(vk, variant.sampler, nullptr);
    }
    if (variant.conversion != VK_NULL_HANDLE) {
        vkDestroySamplerYcbcrConversion(vk, variant.conversion, nullptr);
    }
    variant = {};
}

#ifdef __ANDROID__

Result<detail::YcbcrVariant *> Renderer::Impl::ycbcrVariantFor(
        const VkAndroidHardwareBufferFormatPropertiesANDROID &format) {
    const VkFormat vkFormat = format.format;
    const std::uint64_t externalFormat = vkFormat == VK_FORMAT_UNDEFINED ? format.externalFormat : 0;
    const VkFilter chromaFilter =
            (format.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT)
                    ? VK_FILTER_LINEAR
                    : VK_FILTER_NEAREST;
    const VkComponentMapping &c = format.samplerYcbcrConversionComponents;
    for (const auto &existing : ycbcrVariants) {
        const VkComponentMapping &e = existing->components;
        if (existing->externalFormat == externalFormat && existing->format == vkFormat &&
            existing->model == format.suggestedYcbcrModel &&
            existing->range == format.suggestedYcbcrRange && e.r == c.r && e.g == c.g &&
            e.b == c.b && e.a == c.a && existing->xChroma == format.suggestedXChromaOffset &&
            existing->yChroma == format.suggestedYChromaOffset &&
            existing->chromaFilter == chromaFilter) {
            return existing.get();
        }
    }
    VkDevice vk = device->device;
    auto variant = std::make_unique<detail::YcbcrVariant>();
    variant->externalFormat = externalFormat;
    variant->format = vkFormat;
    variant->model = format.suggestedYcbcrModel;
    variant->range = format.suggestedYcbcrRange;
    variant->components = c;
    variant->xChroma = format.suggestedXChromaOffset;
    variant->yChroma = format.suggestedYChromaOffset;
    variant->chromaFilter = chromaFilter;
    // Cleans up whatever was created if a step fails.
    struct Guard {
        Renderer::Impl &r;
        std::unique_ptr<detail::YcbcrVariant> &v;
        ~Guard() {
            if (v) {
                r.destroyYcbcrVariant(*v);
            }
        }
    } guard{*this, variant};

    VkExternalFormatANDROID external{};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
    external.externalFormat = externalFormat;
    VkSamplerYcbcrConversionCreateInfo conversionInfo{};
    conversionInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
    conversionInfo.pNext = externalFormat != 0 ? &external : nullptr;
    conversionInfo.format = vkFormat;
    conversionInfo.ycbcrModel = variant->model;
    conversionInfo.ycbcrRange = variant->range;
    // External formats take an identity swizzle; the suggested one applies otherwise.
    conversionInfo.components = externalFormat != 0 ? VkComponentMapping{} : c;
    conversionInfo.xChromaOffset = variant->xChroma;
    conversionInfo.yChromaOffset = variant->yChroma;
    conversionInfo.chromaFilter = chromaFilter;
    conversionInfo.forceExplicitReconstruction = VK_FALSE;
    FILEPAN_VK_TRY(vkCreateSamplerYcbcrConversion(vk, &conversionInfo, nullptr, &variant->conversion));

    VkSamplerYcbcrConversionInfo samplerConversion{};
    samplerConversion.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    samplerConversion.conversion = variant->conversion;
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.pNext = &samplerConversion;
    // A conversion's sampler filters as its chroma does, and clamps to the edge.
    samplerInfo.magFilter = chromaFilter;
    samplerInfo.minFilter = chromaFilter;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0;
    FILEPAN_VK_TRY(vkCreateSampler(vk, &samplerInfo, nullptr, &variant->sampler));

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorCount = 1;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binding.pImmutableSamplers = &variant->sampler;
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    FILEPAN_VK_TRY(vkCreateDescriptorSetLayout(vk, &layoutInfo, nullptr, &variant->setLayout));

    // Set 0 and the push constants match uiLayout, so the frame's set stays bound across.
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                             sizeof(detail::UiPush)};
    const VkDescriptorSetLayout sets[] = {frameSetLayout, variant->setLayout};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 2;
    pipelineLayoutInfo.pSetLayouts = sets;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &push;
    FILEPAN_VK_TRY(vkCreatePipelineLayout(vk, &pipelineLayoutInfo, nullptr, &variant->layout));

    ycbcrVariants.push_back(std::move(variant));
    return ycbcrVariants.back().get();
}

Result<detail::ExternalTexture> Renderer::Impl::importHardwareBuffer(AHardwareBuffer *buffer) {
    VkDevice vk = device->device;
    VkAndroidHardwareBufferFormatPropertiesANDROID format{};
    format.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
    VkAndroidHardwareBufferPropertiesANDROID properties{};
    properties.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    properties.pNext = &format;
    FILEPAN_VK_TRY(getHardwareBufferProperties(vk, buffer, &properties));
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(buffer, &desc);
    if (desc.width == 0 || desc.height == 0) {
        return std::unexpected(Error{0, "Empty hardware buffer"});
    }

    // Video formats (an external format, or a YCbCr one) go through a conversion; others are
    // sampled like any image.
    const bool ycbcr = format.format == VK_FORMAT_UNDEFINED ||
                       (format.format >= VK_FORMAT_G8B8G8R8_422_UNORM &&
                        format.format <= VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM);
    detail::YcbcrVariant *variant = nullptr;
    if (ycbcr) {
        auto found = ycbcrVariantFor(format);
        if (!found) {
            return std::unexpected(found.error());
        }
        variant = *found;
    }

    detail::ExternalTexture texture;
    texture.variant = variant;
    detail::Image &image = texture.image;
    image.format = format.format;
    image.width = desc.width;
    image.height = desc.height;
    struct Guard {
        Renderer::Impl &r;
        detail::ExternalTexture *t;
        ~Guard() {
            if (t != nullptr) {
                r.destroyExternalTexture(*t);
            }
        }
    } guard{*this, &texture};

    VkExternalFormatANDROID external{};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
    external.externalFormat = format.format == VK_FORMAT_UNDEFINED ? format.externalFormat : 0;
    VkExternalMemoryImageCreateInfo externalMemory{};
    externalMemory.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    externalMemory.pNext = &external;
    externalMemory.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.pNext = &externalMemory;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format.format;
    imageInfo.extent = {desc.width, desc.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    FILEPAN_VK_TRY(vkCreateImage(vk, &imageInfo, nullptr, &image.image));

    // The import takes its own reference to the buffer, so the decoder can recycle its AImage
    // and the cached import stays valid.
    VkImportAndroidHardwareBufferInfoANDROID import{};
    import.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    import.buffer = buffer;
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.pNext = &import;
    dedicated.image = image.image;
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.pNext = &dedicated;
    allocation.allocationSize = properties.allocationSize;
    if (properties.memoryTypeBits == 0) {
        return std::unexpected(Error{0, "No memory type for the hardware buffer"});
    }
    allocation.memoryTypeIndex = static_cast<std::uint32_t>(__builtin_ctz(properties.memoryTypeBits));
    FILEPAN_VK_TRY(vkAllocateMemory(vk, &allocation, nullptr, &image.memory));
    FILEPAN_VK_TRY(vkBindImageMemory(vk, image.image, image.memory, 0));

    VkSamplerYcbcrConversionInfo conversion{};
    conversion.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    conversion.conversion = variant != nullptr ? variant->conversion : VK_NULL_HANDLE;
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.pNext = variant != nullptr ? &conversion : nullptr;
    viewInfo.image = image.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format.format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    FILEPAN_VK_TRY(vkCreateImageView(vk, &viewInfo, nullptr, &image.view));

    if (variant == nullptr) {
        auto set = allocateTextureSet(image.view);
        if (!set) {
            return std::unexpected(set.error());
        }
        image.set = *set;
    } else {
        VkDescriptorSetAllocateInfo setInfo{};
        setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setInfo.descriptorSetCount = 1;
        setInfo.pSetLayouts = &variant->setLayout;
        for (VkDescriptorPool pool : variant->pools) {
            setInfo.descriptorPool = pool;
            if (vkAllocateDescriptorSets(vk, &setInfo, &image.set) == VK_SUCCESS) {
                texture.pool = pool;
                break;
            }
            image.set = VK_NULL_HANDLE;
        }
        if (image.set == VK_NULL_HANDLE) {
            VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kYcbcrSetsPerPool * 3};
            VkDescriptorPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
            poolInfo.maxSets = kYcbcrSetsPerPool;
            poolInfo.poolSizeCount = 1;
            poolInfo.pPoolSizes = &size;
            VkDescriptorPool pool = VK_NULL_HANDLE;
            FILEPAN_VK_TRY(vkCreateDescriptorPool(vk, &poolInfo, nullptr, &pool));
            variant->pools.push_back(pool);
            setInfo.descriptorPool = pool;
            FILEPAN_VK_TRY(vkAllocateDescriptorSets(vk, &setInfo, &image.set));
            texture.pool = pool;
        }
        // The sampler is immutable, in the layout.
        VkDescriptorImageInfo imageDescriptor{};
        imageDescriptor.imageView = image.view;
        imageDescriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = image.set;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageDescriptor;
        vkUpdateDescriptorSets(vk, 1, &write, 0, nullptr);
    }
    guard.t = nullptr;
    return texture;
}

#endif

}  // namespace filepan::gfx::vulkan
