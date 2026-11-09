// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "filepan/gfx/display_list.hpp"
#include "filepan/gfx/geometry.hpp"
#include "filepan/gfx/text.hpp"
#include "filepan/gfx/vulkan/external_image_store.hpp"
#include "filepan/gfx/vulkan/image_store.hpp"

struct ANativeWindow;

// The Vulkan renderer: plays gfx::DisplayLists back into a window surface or an offscreen image.
// Everything except the image stores belongs to the render thread, and the Renderer must outlive
// its targets.
namespace filepan::gfx::vulkan {

struct Error {
    std::int32_t result = 0;  // The VkResult, or 0 if the error isn't Vulkan's.
    std::string message;
};

template <typename T>
using Result = std::expected<T, Error>;

// Reported through Renderer::setFrameTimingCallback once a frame's GPU work has finished, which
// is when the frame's slot comes around again (two frames later) or at waitIdle().
struct FrameTiming {
    std::uint64_t frame = 0;
    std::int64_t fenceWaitNs = 0;  // render() blocked waiting for a frame slot.
    std::int64_t acquireNs = 0;    // Blocked acquiring a swapchain image.
    std::int64_t recordNs = 0;     // Display list playback, uploads and command recording.
    std::int64_t gpuNs = -1;       // Between the frame's first and last GPU command; -1 if unknown.
    std::uint32_t drawCalls = 0;
    std::uint32_t instances = 0;
    std::uint32_t renderPasses = 0;
    std::uint32_t blurs = 0;
};

struct RendererOptions {
    // Enables the Khronos validation layer. FILEPAN_VK_VALIDATION=1 in the environment turns it
    // on regardless, and FILEPAN_VK_VALIDATION=0 off.
    bool validation = false;
    // Data from an earlier pipelineCacheData(), to speed up pipeline creation.
    std::vector<std::uint8_t> pipelineCache;
};

class Renderer;

// Where frames go: a window surface (with its swapchain) or an offscreen image.
class RenderTarget {
public:
    ~RenderTarget();
    RenderTarget(const RenderTarget &) = delete;
    RenderTarget &operator=(const RenderTarget &) = delete;

    [[nodiscard]] Size size() const;
    // Offscreen targets: changes the size. Surface targets: recreates the swapchain at the
    // window's current size before the next frame; call it when the window is resized.
    void resize(int width, int height);

    struct Impl;

private:
    friend class Renderer;
    explicit RenderTarget(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

class Renderer {
public:
    // Creates the Vulkan instance and device and all pipelines. The text system rasterizes glyphs
    // on the render thread and must outlive the renderer.
    static Result<std::unique_ptr<Renderer>> create(TextSystem &text,
                                                    const RendererOptions &options = {});
    ~Renderer();
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    // Thread-safe.
    ImageStore &images();
    // Thread-safe. Video frames and other images produced outside the renderer.
    ExternalImageStore &externalImages();

    // A width x height frame. With quarterTurns, the image holds it turned that many quarter turns
    // clockwise, as a pre-rotated swapchain image does (for testing that path); readPixels() then
    // returns the image as it is, height x width for 1 and 3.
    Result<std::unique_ptr<RenderTarget>> createOffscreenTarget(int width, int height,
                                                                int quarterTurns = 0);
#ifdef __ANDROID__
    // The window must outlive the target.
    Result<std::unique_ptr<RenderTarget>> createSurfaceTarget(ANativeWindow *window);
#endif

    // Plays list back into target over clearColor and submits it; for a surface, also presents.
    // Blocks only while both frames in flight are still on the GPU, and, with FIFO presentation,
    // while acquiring the next swapchain image.
    Result<void> render(RenderTarget &target, const DisplayList &list, Color clearColor);

    // Offscreen targets only: waits for the last frame and returns its pixels as non-premultiplied
    // RGBA8, row by row from the top.
    Result<std::vector<std::uint8_t>> readPixels(RenderTarget &target);

    // Called on the render thread from render() and waitIdle().
    void setFrameTimingCallback(std::function<void(const FrameTiming &)> callback);

    // For RendererOptions::pipelineCache on the next launch.
    [[nodiscard]] std::vector<std::uint8_t> pipelineCacheData() const;

    // Waits for all submitted work, e.g. before destroying the window.
    void waitIdle();

    // Errors the validation layer reported, when it's enabled.
    [[nodiscard]] std::uint32_t validationErrorCount() const;
    // The physical device's name.
    [[nodiscard]] std::string deviceName() const;

    struct Impl;

private:
    explicit Renderer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace filepan::gfx::vulkan
