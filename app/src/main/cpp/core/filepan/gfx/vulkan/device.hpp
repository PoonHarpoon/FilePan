// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

#include "filepan/gfx/vulkan/renderer.hpp"

// Internal: the Vulkan instance and device, and plain buffers and images on top of them.
namespace filepan::gfx::vulkan::detail {

inline std::unexpected<Error> vkError(VkResult result, std::string what) {
    return std::unexpected(Error{static_cast<std::int32_t>(result),
                                 std::move(what) + " failed: VkResult " +
                                         std::to_string(static_cast<int>(result))});
}

// Evaluates a VkResult expression and returns an Error from the enclosing function on failure.
#define FILEPAN_VK_TRY(expr)                                   \
    do {                                                       \
        const VkResult filepan_vk_result_ = (expr);            \
        if (filepan_vk_result_ != VK_SUCCESS) {                \
            return ::filepan::gfx::vulkan::detail::vkError(    \
                    filepan_vk_result_, #expr);                \
        }                                                      \
    } while (false)

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = nullptr;  // Host-visible buffers stay mapped.
    VkDeviceSize size = 0;
};

// An image's last use, so barriers can be derived from the next one.
struct ImageState {
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkAccessFlags access = 0;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;  // Null for swapchain images.
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;  // For images rendered to.
    VkDescriptorSet set = VK_NULL_HANDLE;        // For images sampled, in the texture layout.
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t layers = 1;
    bool owned = true;  // False for swapchain images, which the swapchain destroys.
    std::uint32_t version = 1;  // ImageStore images: bumped when their pixels are replaced.
    ImageState state;
};

// Records a barrier, if one is needed, from the image's last use to the next one.
void useImage(VkCommandBuffer cmd, Image &image, VkImageLayout layout, VkPipelineStageFlags stages,
              VkAccessFlags access);

class Device {
public:
    static Result<std::unique_ptr<Device>> create(bool validation);
    ~Device();
    Device(const Device &) = delete;
    Device &operator=(const Device &) = delete;

    Result<Buffer> createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible);
    void destroy(Buffer &buffer);

    // A 2D image, or a 2D array if layers > 1, with a view covering all of it.
    Result<Image> createImage(std::uint32_t width, std::uint32_t height, std::uint32_t layers,
                              VkFormat format, VkImageUsageFlags usage);
    // Destroys everything the image owns except its descriptor set.
    void destroy(Image &image);

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queueFamily = 0;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    bool swapchainSupported = false;
    bool timestampsSupported = false;
    // VK_ANDROID_external_memory_android_hardware_buffer, VK_EXT_queue_family_foreign and the
    // samplerYcbcrConversion feature are on.
    bool hardwareBuffersSupported = false;
    std::atomic<std::uint32_t> validationErrors{0};

private:
    Device() = default;
    Result<std::uint32_t> memoryType(std::uint32_t typeBits, VkMemoryPropertyFlags required,
                                     VkMemoryPropertyFlags preferred) const;

    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
};

}  // namespace filepan::gfx::vulkan::detail
