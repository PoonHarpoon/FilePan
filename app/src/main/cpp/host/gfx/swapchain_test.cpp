// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Swapchain sizing on rotation, with surface capabilities as Android reports them: currentExtent
// still has the old orientation's size right after the window turned.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "filepan/gfx/vulkan/swapchain.hpp"
#include "test_util.hpp"

using filepan::gfx::vulkan::detail::swapchainExtent;
using filepan::gfx::vulkan::detail::swapchainStale;
using filepan::gfx::vulkan::detail::swapchainTransform;

namespace {

// What Android's loader reports: currentExtent from the window's cached default size, a minimum
// of 1x1 and a maximum of 4096x4096 (or currentExtent, if bigger).
VkSurfaceCapabilitiesKHR androidCaps(std::uint32_t width, std::uint32_t height) {
    VkSurfaceCapabilitiesKHR caps{};
    caps.currentExtent = {width, height};
    caps.minImageExtent = {1, 1};
    caps.maxImageExtent = {std::max(4096u, width), std::max(4096u, height)};
    caps.currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps.supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR |
                               VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
    return caps;
}

bool equal(VkExtent2D a, VkExtent2D b) { return a.width == b.width && a.height == b.height; }

void testRotation() {
    // Portrait, settled: everything agrees.
    EXPECT(equal(swapchainExtent(androidCaps(1080, 2400), 1080, 2400), {1080, 2400}));
    // Just turned: the window is landscape, currentExtent still portrait. The window wins.
    EXPECT(equal(swapchainExtent(androidCaps(1080, 2400), 2400, 1080), {2400, 1080}));
    // And back.
    EXPECT(equal(swapchainExtent(androidCaps(2400, 1080), 1080, 2400), {1080, 2400}));
    // A resize that keeps the orientation (split screen, a foldable opening).
    EXPECT(equal(swapchainExtent(androidCaps(1080, 2400), 1080, 1180), {1080, 1180}));
}

void testPreRotation() {
    // Turned to landscape, drawn pre-rotated: the window is 2400x1080 as the app sees it, the
    // images stay in the display's natural portrait.
    VkSurfaceCapabilitiesKHR turned = androidCaps(1080, 2400);
    for (auto [transform, turns] : {std::pair{VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR, 1},
                                    std::pair{VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR, 3}}) {
        turned.currentTransform = transform;
        const auto chosen = swapchainTransform(turned);
        EXPECT(chosen.preTransform == transform && chosen.quarterTurns == turns);
        EXPECT(equal(swapchainExtent(turned, 2400, 1080, turns), {1080, 2400}));
    }
    // Upside down: no swap.
    turned.currentTransform = VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR;
    EXPECT(swapchainTransform(turned).quarterTurns == 2);
    EXPECT(equal(swapchainExtent(turned, 1080, 2400, 2), {1080, 2400}));
    // The limits apply to the image, in the natural orientation.
    VkSurfaceCapabilitiesKHR narrow = androidCaps(100, 100);
    narrow.maxImageExtent = {1080, 4096};
    EXPECT(equal(swapchainExtent(narrow, 2400, 1080, 1), {1080, 2400}));
    // A transform the surface can't take, or a mirrored one, falls back to IDENTITY.
    VkSurfaceCapabilitiesKHR identityOnly = androidCaps(1080, 2400);
    identityOnly.supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    identityOnly.currentTransform = VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR;
    EXPECT(swapchainTransform(identityOnly).preTransform == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR &&
           swapchainTransform(identityOnly).quarterTurns == 0);
    VkSurfaceCapabilitiesKHR mirrored = androidCaps(1080, 2400);
    mirrored.currentTransform = VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_BIT_KHR;
    EXPECT(swapchainTransform(mirrored).quarterTurns == 0);
}

void testFallbacks() {
    // No window size: currentExtent.
    EXPECT(equal(swapchainExtent(androidCaps(1080, 2400), 0, 0), {1080, 2400}));
    // Neither: no area (minimized, not laid out), so no swapchain.
    EXPECT(equal(swapchainExtent(androidCaps(0, 0), 0, 0), {0, 0}));
    VkSurfaceCapabilitiesKHR undefined = androidCaps(0, 0);
    undefined.currentExtent = {UINT32_MAX, UINT32_MAX};
    EXPECT(equal(swapchainExtent(undefined, 0, 0), {0, 0}));
    EXPECT(equal(swapchainExtent(undefined, 800, 600), {800, 600}));
    // Clamped to what the surface allows.
    VkSurfaceCapabilitiesKHR small = androidCaps(100, 100);
    small.maxImageExtent = {512, 512};
    EXPECT(equal(swapchainExtent(small, 2400, 1080), {512, 512}));
}

void testStale() {
    // Made in portrait, the window now landscape: stale, though Android says nothing.
    EXPECT(swapchainStale(1080, 2400, 2400, 1080));
    EXPECT(!swapchainStale(1080, 2400, 1080, 2400));
    // A window that reports no size keeps the swapchain it has.
    EXPECT(!swapchainStale(1080, 2400, 0, 0));
    // One made while the window had no size is stale once it has one.
    EXPECT(swapchainStale(0, 0, 1080, 2400));
}

}  // namespace

int main() {
    testRotation();
    testPreRotation();
    testFallbacks();
    testStale();
    return 0;
}
