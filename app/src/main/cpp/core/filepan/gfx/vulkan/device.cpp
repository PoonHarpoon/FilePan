// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/gfx/vulkan/device.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace filepan::gfx::vulkan::detail {

namespace {

constexpr VkAccessFlags kWriteAccess =
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
        VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT *data,
                                             void *user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        static_cast<Device *>(user)->validationErrors.fetch_add(1, std::memory_order_relaxed);
    }
    if (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_WARN, "FilePanVulkan", "%s", data->pMessage);
#else
        std::fprintf(stderr, "vulkan: %s\n", data->pMessage);
#endif
    }
    return VK_FALSE;
}

bool hasExtension(const std::vector<VkExtensionProperties> &extensions, std::string_view name) {
    for (const auto &extension : extensions) {
        if (name == extension.extensionName) {
            return true;
        }
    }
    return false;
}

bool hasLayer(std::string_view name) {
    std::uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const auto &layer : layers) {
        if (name == layer.layerName) {
            return true;
        }
    }
    return false;
}

int deviceTypeRank(VkPhysicalDeviceType type) {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            return 0;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            return 1;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            return 2;
        default:
            return 3;
    }
}

}  // namespace

void useImage(VkCommandBuffer cmd, Image &image, VkImageLayout layout, VkPipelineStageFlags stages,
              VkAccessFlags access) {
    ImageState &state = image.state;
    if (state.layout == layout && !(state.access & kWriteAccess) && !(access & kWriteAccess)) {
        // Read after read in the same layout: no barrier, but later writes must wait for both.
        state.stages |= stages;
        state.access |= access;
        return;
    }
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = state.access & kWriteAccess;
    barrier.dstAccessMask = access;
    barrier.oldLayout = state.layout;
    barrier.newLayout = layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, image.layers};
    vkCmdPipelineBarrier(cmd, state.stages, stages, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    state = {layout, stages, access};
}

Result<std::unique_ptr<Device>> Device::create(bool validation) {
    std::unique_ptr<Device> self(new Device());

    const char *environment = std::getenv("FILEPAN_VK_VALIDATION");
    if (environment != nullptr && std::strcmp(environment, "1") == 0) {
        validation = true;
    } else if (environment != nullptr && std::strcmp(environment, "0") == 0) {
        validation = false;
    }
    constexpr const char *kValidationLayer = "VK_LAYER_KHRONOS_validation";
    if (validation && !hasLayer(kValidationLayer)) {
        std::fprintf(stderr, "vulkan: validation requested but %s isn't installed\n",
                     kValidationLayer);
        validation = false;
    }

    std::uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> instanceExtensions(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, instanceExtensions.data());
    std::vector<const char *> extensions;
    bool surfaceSupported = hasExtension(instanceExtensions, VK_KHR_SURFACE_EXTENSION_NAME);
#ifdef __ANDROID__
    surfaceSupported = surfaceSupported &&
                       hasExtension(instanceExtensions, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
    if (surfaceSupported) {
        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        extensions.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
    }
#else
    // Host builds render offscreen only.
    surfaceSupported = false;
#endif
    if (validation && hasExtension(instanceExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    } else {
        validation = false;
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "FilePan";
    app.pEngineName = "FilePan";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    instanceInfo.ppEnabledExtensionNames = extensions.data();
    if (validation) {
        instanceInfo.enabledLayerCount = 1;
        instanceInfo.ppEnabledLayerNames = &kValidationLayer;
    }
    FILEPAN_VK_TRY(vkCreateInstance(&instanceInfo, nullptr, &self->instance));

    if (validation) {
        VkDebugUtilsMessengerCreateInfoEXT messengerInfo{};
        messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messengerInfo.pfnUserCallback = debugCallback;
        messengerInfo.pUserData = self.get();
        auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(self->instance, "vkCreateDebugUtilsMessengerEXT"));
        if (createMessenger != nullptr) {
            createMessenger(self->instance, &messengerInfo, nullptr, &self->messenger_);
        }
    }

    vkEnumeratePhysicalDevices(self->instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physicalDevices(count);
    vkEnumeratePhysicalDevices(self->instance, &count, physicalDevices.data());
    int bestRank = 1 << 30;
    for (VkPhysicalDevice physical : physicalDevices) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(physical, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_1) {
            continue;
        }
        std::uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
        for (std::uint32_t family = 0; family < familyCount; ++family) {
            if (!(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                continue;
            }
            const int rank = deviceTypeRank(properties.deviceType);
            if (rank < bestRank) {
                bestRank = rank;
                self->physical = physical;
                self->queueFamily = family;
                self->properties = properties;
                self->timestampsSupported = families[family].timestampValidBits > 0 &&
                                            properties.limits.timestampPeriod > 0;
            }
            break;
        }
    }
    if (self->physical == VK_NULL_HANDLE) {
        return std::unexpected(Error{VK_ERROR_INITIALIZATION_FAILED,
                                     "No Vulkan 1.1 device with a graphics queue"});
    }
    vkGetPhysicalDeviceMemoryProperties(self->physical, &self->memoryProperties);

    vkEnumerateDeviceExtensionProperties(self->physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> deviceExtensions(count);
    vkEnumerateDeviceExtensionProperties(self->physical, nullptr, &count, deviceExtensions.data());
    std::vector<const char *> enabledDeviceExtensions;
    if (surfaceSupported && hasExtension(deviceExtensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        enabledDeviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        self->swapchainSupported = true;
    }

    // Video frames: imported AHardwareBuffers, sampled through a YCbCr conversion (core in 1.1,
    // but a feature to turn on). Without all of it, the viewer falls back to other apps.
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcrFeatures{};
    ycbcrFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
#ifdef __ANDROID__
    {
        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &ycbcrFeatures;
        vkGetPhysicalDeviceFeatures2(self->physical, &features);
        if (ycbcrFeatures.samplerYcbcrConversion == VK_TRUE &&
            hasExtension(deviceExtensions,
                         VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME) &&
            hasExtension(deviceExtensions, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)) {
            enabledDeviceExtensions.push_back(
                    VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
            enabledDeviceExtensions.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
            self->hardwareBuffersSupported = true;
        }
    }
#endif
    ycbcrFeatures.samplerYcbcrConversion = self->hardwareBuffersSupported ? VK_TRUE : VK_FALSE;

    const float priority = 1;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = self->queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    if (self->hardwareBuffersSupported) {
        deviceInfo.pNext = &ycbcrFeatures;
    }
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = static_cast<std::uint32_t>(enabledDeviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = enabledDeviceExtensions.data();
    FILEPAN_VK_TRY(vkCreateDevice(self->physical, &deviceInfo, nullptr, &self->device));
    vkGetDeviceQueue(self->device, self->queueFamily, 0, &self->queue);
    return self;
}

Device::~Device() {
    if (device != VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
    }
    if (messenger_ != VK_NULL_HANDLE) {
        auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyMessenger != nullptr) {
            destroyMessenger(instance, messenger_, nullptr);
        }
    }
    if (instance != VK_NULL_HANDLE) {
        vkDestroyInstance(instance, nullptr);
    }
}

Result<std::uint32_t> Device::memoryType(std::uint32_t typeBits, VkMemoryPropertyFlags required,
                                         VkMemoryPropertyFlags preferred) const {
    int fallback = -1;
    for (std::uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = memoryProperties.memoryTypes[i].propertyFlags;
        if (!(typeBits & (1u << i)) || (flags & required) != required) {
            continue;
        }
        if ((flags & preferred) == preferred) {
            return i;
        }
        if (fallback < 0) {
            fallback = static_cast<int>(i);
        }
    }
    if (fallback < 0) {
        return std::unexpected(Error{VK_ERROR_OUT_OF_DEVICE_MEMORY, "No suitable memory type"});
    }
    return static_cast<std::uint32_t>(fallback);
}

Result<Buffer> Device::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible) {
    Buffer buffer;
    buffer.size = size;
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    FILEPAN_VK_TRY(vkCreateBuffer(device, &info, nullptr, &buffer.buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer.buffer, &requirements);
    const auto type =
            hostVisible
                    ? memoryType(requirements.memoryTypeBits,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 0)
                    : memoryType(requirements.memoryTypeBits, 0,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!type) {
        vkDestroyBuffer(device, buffer.buffer, nullptr);
        return std::unexpected(type.error());
    }
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = *type;
    if (VkResult result = vkAllocateMemory(device, &allocation, nullptr, &buffer.memory);
        result != VK_SUCCESS) {
        vkDestroyBuffer(device, buffer.buffer, nullptr);
        return vkError(result, "vkAllocateMemory");
    }
    vkBindBufferMemory(device, buffer.buffer, buffer.memory, 0);
    if (hostVisible) {
        if (VkResult result = vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped);
            result != VK_SUCCESS) {
            destroy(buffer);
            return vkError(result, "vkMapMemory");
        }
    }
    return buffer;
}

void Device::destroy(Buffer &buffer) {
    if (buffer.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, buffer.buffer, nullptr);
    }
    if (buffer.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, buffer.memory, nullptr);
    }
    buffer = {};
}

Result<Image> Device::createImage(std::uint32_t width, std::uint32_t height, std::uint32_t layers,
                                  VkFormat format, VkImageUsageFlags usage) {
    Image image;
    image.format = format;
    image.width = width;
    image.height = height;
    image.layers = layers;
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    FILEPAN_VK_TRY(vkCreateImage(device, &info, nullptr, &image.image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image.image, &requirements);
    const auto type =
            memoryType(requirements.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!type) {
        destroy(image);
        return std::unexpected(type.error());
    }
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = *type;
    if (VkResult result = vkAllocateMemory(device, &allocation, nullptr, &image.memory);
        result != VK_SUCCESS) {
        destroy(image);
        return vkError(result, "vkAllocateMemory");
    }
    vkBindImageMemory(device, image.image, image.memory, 0);
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image.image;
    viewInfo.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    if (VkResult result = vkCreateImageView(device, &viewInfo, nullptr, &image.view);
        result != VK_SUCCESS) {
        destroy(image);
        return vkError(result, "vkCreateImageView");
    }
    return image;
}

void Device::destroy(Image &image) {
    if (image.framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, image.framebuffer, nullptr);
    }
    if (image.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, image.view, nullptr);
    }
    if (image.owned && image.image != VK_NULL_HANDLE) {
        vkDestroyImage(device, image.image, nullptr);
    }
    if (image.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, image.memory, nullptr);
    }
    image = {};
}

}  // namespace filepan::gfx::vulkan::detail
