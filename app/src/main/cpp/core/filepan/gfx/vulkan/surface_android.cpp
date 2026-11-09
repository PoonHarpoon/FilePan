// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Window surfaces and swapchains. Android only. Drawing pre-rotated (the display's transform as
// the swapchain's preTransform) is untested on a device so far.

#ifdef __ANDROID__

#include <algorithm>
#include <vector>

#include <android/native_window.h>

// With VK_USE_PLATFORM_ANDROID_KHR, which the build defines, vulkan.h declares the Android
// surface API too.
#include "filepan/gfx/vulkan/renderer_impl.hpp"
#include "filepan/gfx/vulkan/swapchain.hpp"

namespace filepan::gfx::vulkan {

Result<std::unique_ptr<RenderTarget>> Renderer::Impl::createSurface(ANativeWindow *window) {
    if (window == nullptr || !device->swapchainSupported) {
        return std::unexpected(Error{0, "No window, or the device can't present"});
    }
    auto impl = std::make_unique<RenderTarget::Impl>();
    impl->renderer = this;
    impl->offscreen = false;
    impl->window = window;
    VkAndroidSurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    info.window = window;
    FILEPAN_VK_TRY(vkCreateAndroidSurfaceKHR(device->instance, &info, nullptr, &impl->surface));
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(device->physical, device->queueFamily, impl->surface,
                                         &supported);
    if (!supported) {
        return std::unexpected(Error{0, "The graphics queue can't present to this window"});
    }
    if (auto result = createSwapchain(*impl); !result) {
        return std::unexpected(result.error());
    }
    return std::unique_ptr<RenderTarget>(new RenderTarget(std::move(impl)));
}

void Renderer::Impl::destroySwapchainImages(RenderTarget::Impl &target) {
    for (detail::Image &image : target.swapchainImages) {
        device->destroy(image);
    }
    target.swapchainImages.clear();
    for (VkSemaphore semaphore : target.renderDone) {
        vkDestroySemaphore(device->device, semaphore, nullptr);
    }
    target.renderDone.clear();
}

Result<void> Renderer::Impl::createSwapchain(RenderTarget::Impl &target) {
    VkDevice vk = device->device;
    VkPhysicalDevice physical = device->physical;
    // Rare (resizes and rotations), so simply wait for everything using the old images.
    waitIdle();
    target.recreateSwapchain = false;

    VkSurfaceCapabilitiesKHR caps{};
    FILEPAN_VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, target.surface, &caps));
    // Not caps.currentExtent alone: after a rotation it still has the old size (see swapchain.hpp).
    target.windowWidth = ANativeWindow_getWidth(target.window);
    target.windowHeight = ANativeWindow_getHeight(target.window);
    // Drawn as the display is turned, so the compositor needn't turn every frame.
    const detail::SwapchainTransform transform = detail::swapchainTransform(caps);
    const VkExtent2D extent =
            detail::swapchainExtent(caps, target.windowWidth, target.windowHeight, transform.quarterTurns);
    // The frame's size, as the app lays it out: the image's, turned back.
    const bool turned = transform.quarterTurns % 2 != 0;
    const int frameWidth = static_cast<int>(turned ? extent.height : extent.width);
    const int frameHeight = static_cast<int>(turned ? extent.width : extent.height);

    std::uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, target.surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, target.surface, &count, formats.data());
    // UNORM, not SRGB: the renderer writes sRGB values and blends in sRGB space.
    VkSurfaceFormatKHR format{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (VkFormat wanted : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM}) {
        for (const VkSurfaceFormatKHR &candidate : formats) {
            if (format.format == VK_FORMAT_UNDEFINED && candidate.format == wanted &&
                candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                format = candidate;
            }
        }
    }
    if (format.format == VK_FORMAT_UNDEFINED) {
        return std::unexpected(Error{0, "The window offers no RGBA8 or BGRA8 UNORM format"});
    }
    // Live backdrop blurs blit from the swapchain image (unturned ones; see RenderTarget::Impl's
    // unrotated image).
    constexpr VkImageUsageFlags kUsage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if ((caps.supportedUsageFlags & kUsage) != kUsage) {
        return std::unexpected(Error{0, "The swapchain can't be a transfer source"});
    }

    // Scratch images are the frame's size, whichever way the display is turned.
    if (format.format != target.format || frameWidth != target.width || frameHeight != target.height) {
        target.destroyScratch();
    }
    auto pipelineSet = pipelinesFor(format.format);
    if (!pipelineSet) {
        return std::unexpected(pipelineSet.error());
    }
    target.pipelines = *pipelineSet;
    target.format = format.format;

    destroySwapchainImages(target);
    if (extent.width == 0 || extent.height == 0) {
        // Minimized or not laid out yet; render() skips frames until it has an area.
        if (target.swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(vk, target.swapchain, nullptr);
            target.swapchain = VK_NULL_HANDLE;
        }
        target.width = 0;
        target.height = 0;
        target.transform = 0;
        return {};
    }

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = target.surface;
    info.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount > 0) {
        info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
    }
    info.imageFormat = format.format;
    info.imageColorSpace = format.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = kUsage;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = transform.preTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    for (VkCompositeAlphaFlagBitsKHR alpha :
         {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR}) {
        if (caps.supportedCompositeAlpha & alpha) {
            info.compositeAlpha = alpha;
            break;
        }
    }
    // FIFO: always available, and the shell paces frames with AChoreographer.
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    info.oldSwapchain = target.swapchain;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    const VkResult result = vkCreateSwapchainKHR(vk, &info, nullptr, &swapchain);
    if (target.swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(vk, target.swapchain, nullptr);
        target.swapchain = VK_NULL_HANDLE;
    }
    if (result != VK_SUCCESS) {
        return detail::vkError(result, "vkCreateSwapchainKHR");
    }
    target.swapchain = swapchain;
    target.width = frameWidth;
    target.height = frameHeight;
    target.transform = transform.quarterTurns;
    target.preTransform = transform.preTransform;

    vkGetSwapchainImagesKHR(vk, swapchain, &count, nullptr);
    std::vector<VkImage> images(count);
    vkGetSwapchainImagesKHR(vk, swapchain, &count, images.data());
    for (VkImage handle : images) {
        detail::Image image;
        image.image = handle;
        image.owned = false;
        image.format = format.format;
        image.width = extent.width;
        image.height = extent.height;
        target.swapchainImages.push_back(image);
        detail::Image &stored = target.swapchainImages.back();
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = handle;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        FILEPAN_VK_TRY(vkCreateImageView(vk, &viewInfo, nullptr, &stored.view));
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = target.pipelines->clearPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &stored.view;
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;
        FILEPAN_VK_TRY(vkCreateFramebuffer(vk, &framebufferInfo, nullptr, &stored.framebuffer));
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkSemaphore semaphore = VK_NULL_HANDLE;
        FILEPAN_VK_TRY(vkCreateSemaphore(vk, &semaphoreInfo, nullptr, &semaphore));
        target.renderDone.push_back(semaphore);
    }
    return {};
}

}  // namespace filepan::gfx::vulkan

#endif  // __ANDROID__
