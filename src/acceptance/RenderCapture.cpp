#include <acceptance/RenderCapture.hpp>

#include <client/render/InstalledShaderAssets.hpp>
#include <client/render/VulkanRenderer.hpp>

#include <core/vulkan/GlfwSurfaceProvider.hpp>
#include <core/window/Window.hpp>

#include <acceptance/ImageEvidence.hpp>
#include <fmt/format.h>

#include <chrono>
#include <limits>
#include <vector>

namespace acceptance {

namespace {

std::string presentModeName(client::RendererPresentMode const mode)
{
    switch (mode) {
        case client::RendererPresentMode::Immediate: return "immediate";
        case client::RendererPresentMode::Mailbox: return "mailbox";
        case client::RendererPresentMode::FIFO: return "fifo";
        case client::RendererPresentMode::FIFORelaxed: return "fifo-relaxed";
        case client::RendererPresentMode::Unknown: return "unknown";
    }
    return "unknown";
}

std::string pipelinePathName(client::RendererPipelinePath const path)
{
    return path == client::RendererPipelinePath::Mesh ? "mesh" : "vertex";
}

} // namespace

std::expected<RuntimeEvidence, std::string> captureRendererFrame(
    std::filesystem::path const& image_path,
    RenderCaptureOptions const& options
)
{
    if (options.width == 0U || options.height == 0U || options.max_resize_polls == 0U
        || options.max_frames == 0U || options.deadline <= std::chrono::seconds::zero()
    ) {
        return std::unexpected("renderer capture has invalid bounded limits");
    }
    if (options.width > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())
        || options.height > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())
    ) {
        return std::unexpected("renderer capture extent is not supported by the window API");
    }
    core::Window const window{
        "MinecraftClone renderer capture",
        static_cast<int32_t>(options.width),
        static_cast<int32_t>(options.height),
    };
    if (!window.resizeFramebuffer(options.width, options.height, options.max_resize_polls)) {
        return std::unexpected("renderer capture could not establish its requested framebuffer extent");
    }
    core::vk::GlfwSurfaceProvider const surface_provider{ window };
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        surface_provider,
        shader_assets,
        {
            .enable_frame_capture = true,
        },
    };
    if (renderer.captureState() != client::FrameCaptureState::Ready) {
        return std::unexpected("renderer capture is unsupported or unavailable");
    }
    std::vector<client::PlayerRenderData> const players{
        client::PlayerRenderData{ .x = 2U, .y = 3U, .color = { 1.0F, 0.0F, 0.0F, 1.0F } },
        client::PlayerRenderData{ .x = 29U, .y = 28U, .color = { 0.0F, 1.0F, 0.0F, 1.0F } },
    };
    renderer.requestFrameCapture();
    auto const started_at = std::chrono::steady_clock::now();
    auto const deadline = started_at + options.deadline;
    uint64_t rendered_frames{ 0 };
    for (uint64_t frame{ 0 }; frame < options.max_frames; ++frame) {
        if (std::chrono::steady_clock::now() - started_at >= options.deadline) {
            return std::unexpected("renderer capture exceeded its monotonic deadline");
        }
        if (!window.nextFrame()) {
            return std::unexpected("renderer capture window was closed");
        }
        if (renderer.render(players, deadline)) {
            ++rendered_frames;
        }
        if (renderer.captureState() == client::FrameCaptureState::Completed) {
            break;
        }
    }
    if (!renderer.waitForSubmittedFrames(deadline)) {
        return std::unexpected("renderer capture exceeded its monotonic deadline while draining frames");
    }
    auto const capture = renderer.takeFrameCapture();
    if (!capture.has_value()) {
        return std::unexpected("renderer capture did not complete within its frame limit");
    }
    auto const content = validateGameplayFrameCapture(
        capture->width,
        capture->height,
        capture->rgba8,
        capture->srgb_encoded
    );
    if (!content.has_value()) {
        return std::unexpected(content.error());
    }
    auto const written = writeRgba8Ppm(image_path, capture->width, capture->height, capture->rgba8);
    if (!written.has_value()) {
        return std::unexpected(written.error());
    }
    client::RendererRuntimeInfo const runtime = renderer.runtimeInfo();
    if (runtime.width != options.width || runtime.height != options.height) {
        return std::unexpected("renderer capture did not retain its requested framebuffer extent");
    }
    return RuntimeEvidence{
        .mode = "capture-render",
        .ticks = rendered_frames,
        .expectations_passed = 1U,
        .elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at
        ),
        .deadline = std::chrono::duration_cast<std::chrono::milliseconds>(options.deadline),
        .passed = true,
        .capture = RendererCaptureEvidence{
            .image_path = image_path.string(),
            .gpu = runtime.gpu_name,
            .vulkan_api_version = fmt::format("{}", runtime.vulkan_api_version),
            .present_mode = presentModeName(runtime.present_mode),
            .pipeline_path = pipelinePathName(runtime.pipeline_path),
            .requested_width = options.width,
            .requested_height = options.height,
            .actual_width = runtime.width,
            .actual_height = runtime.height,
            .validation_enabled = runtime.validation_enabled,
            .content_verified = true,
        },
    };
}

} // namespace acceptance
