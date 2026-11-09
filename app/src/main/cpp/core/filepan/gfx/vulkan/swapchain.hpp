// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>

#include <vulkan/vulkan.h>

// Swapchain sizing, kept free of the window so it builds and is tested on a host.
namespace filepan::gfx::vulkan::detail {

// The transform a new swapchain's images are drawn with, and the quarter turns clockwise that
// means for the frame.
struct SwapchainTransform {
    VkSurfaceTransformFlagBitsKHR preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    int quarterTurns = 0;
};

// The display's current rotation, so the frame is drawn pre-rotated and the compositor doesn't
// have to turn every frame. Mirrored transforms aren't drawn: IDENTITY instead where it's
// supported, and the compositor turns the image.
inline SwapchainTransform swapchainTransform(const VkSurfaceCapabilitiesKHR &caps) {
    constexpr struct {
        VkSurfaceTransformFlagBitsKHR transform;
        int quarterTurns;
    } kTurns[] = {{VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, 0},
                  {VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR, 1},
                  {VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR, 2},
                  {VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR, 3}};
    for (const auto &turn : kTurns) {
        if (caps.currentTransform == turn.transform && (caps.supportedTransforms & turn.transform) != 0) {
            return {turn.transform, turn.quarterTurns};
        }
    }
    if ((caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0) {
        return {};
    }
    // Only the current (mirrored) transform: present with it, as drawn.
    return {caps.currentTransform, 0};
}

// The extent for a new swapchain on a window that is windowWidth x windowHeight px now, its
// images holding the frame turned quarterTurns clockwise.
//
// Android's currentExtent is not the window's size now: the platform's Surface caches it from
// its last connect or queued buffer, so right after a resize or a rotation it still reports the
// old size, and a swapchain made from it stays at the old size (Android never reports a size
// change as OUT_OF_DATE). The window's own size (ANativeWindow_getWidth/Height, the same as
// surfaceChanged) is current, so it wins; currentExtent is only the fallback.
//
// The window reports its size as the app sees it. Pre-rotated images are in the display's
// natural orientation instead, so a quarter turn either way swaps the window's width and height;
// currentExtent and the image limits are already in the natural orientation.
inline VkExtent2D swapchainExtent(const VkSurfaceCapabilitiesKHR &caps, int windowWidth, int windowHeight,
                                  int quarterTurns = 0) {
    VkExtent2D extent{0, 0};
    if (windowWidth > 0 && windowHeight > 0) {
        extent = {static_cast<std::uint32_t>(windowWidth), static_cast<std::uint32_t>(windowHeight)};
        if (quarterTurns % 2 != 0) {
            std::swap(extent.width, extent.height);
        }
    } else if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    }
    if (extent.width == 0 || extent.height == 0) {
        return {0, 0};  // Minimized or not laid out yet.
    }
    const auto clamp = [](std::uint32_t value, std::uint32_t low, std::uint32_t high) {
        return high >= low && high > 0 ? std::clamp(value, low, high) : value;
    };
    return {clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width),
            clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height)};
}

// Whether a swapchain made when the window was madeWidth x madeHeight px is stale now that it
// is windowWidth x windowHeight. Checked every frame, so a size change that surfaceChanged
// didn't report (or reported before the window had it) still recreates it.
inline bool swapchainStale(int madeWidth, int madeHeight, int windowWidth, int windowHeight) {
    if (windowWidth <= 0 || windowHeight <= 0) {
        return false;  // Unknown; keep what we have until the shell says otherwise.
    }
    return windowWidth != madeWidth || windowHeight != madeHeight;
}

}  // namespace filepan::gfx::vulkan::detail
