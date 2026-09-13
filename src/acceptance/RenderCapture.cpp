#include <acceptance/RenderCapture.hpp>

#include <client/render/InstalledShaderAssets.hpp>
#include <client/render/VulkanRenderer.hpp>

#include <shared/world/CanonicalWorld.hpp>
#include <shared/world/ChunkMesher.hpp>

#include <core/platform/glfw/GlfwWindow.hpp>

#include <acceptance/FramebufferExtent.hpp>
#include <acceptance/ImageEvidence.hpp>
#include <fmt/format.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <span>

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

[[nodiscard]]
shared::ChunkMesh makeS5ChunkMesh()
{
    shared::ChunkMesher mesher;
    return mesher.update(shared::canonicalWorld().chunk());
}

constexpr client::CameraPose S5_CAMERA{
    .position = { 8.0, -20.0, 18.0 },
    .angles = { .pitch_degrees = -25.0 },
};

[[nodiscard]]
std::expected<void, std::string> validateStoneFrame(
    uint32_t const width,
    uint32_t const height,
    std::span<uint8_t const> const rgba8
)
{
    uint64_t const expected_size = static_cast<uint64_t>(width) * height * 4U;
    if (width == 0U || height == 0U || rgba8.size() != expected_size) {
        return std::unexpected("stone frame capture does not match its declared extent");
    }
    bool has_sky = false;
    bool has_stone = false;
    for (uint64_t offset{ 0 }; offset < rgba8.size(); offset += 4U) {
        uint8_t const red = rgba8[offset];
        uint8_t const green = rgba8[offset + 1U];
        uint8_t const blue = rgba8[offset + 2U];
        has_sky = has_sky || (blue > green + 20U && green > red + 20U);
        uint8_t const red_green_delta = red > green ? red - green : green - red;
        uint8_t const green_blue_delta = green > blue ? green - blue : blue - green;
        has_stone = has_stone || (
            red >= 50U && red <= 160U && red_green_delta <= 10U && green_blue_delta <= 10U
        );
    }
    if (!has_sky || !has_stone) {
        return std::unexpected("stone frame capture is missing sky or visible textured terrain");
    }
    return { };
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
    core::platform::glfw::GlfwWindow const window{
        core::platform::glfw::WindowDescriptor{
            .width = options.width,
            .height = options.height,
            .title = "MinecraftClone renderer capture",
        },
    };
    std::expected<void, std::string> const extent = ensureFramebufferExtent(
        window,
        { .width = options.width, .height = options.height },
        options.max_resize_polls
    );
    if (!extent.has_value()) {
        return std::unexpected(extent.error());
    }
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(
            window,
            { .enable_frame_capture = true }
        ),
        shader_assets,
        {
            .enable_frame_capture = true,
        },
    };
    shared::ChunkMesh const chunk_mesh = makeS5ChunkMesh();
    if (chunk_mesh.faces.empty()) {
        return std::unexpected("renderer capture generated an empty S5 chunk mesh");
    }
    renderer.setCamera(S5_CAMERA);
    renderer.setChunkMesh(chunk_mesh);
    client::RendererRuntimeInfo const initial_runtime = renderer.runtimeInfo();
    if (initial_runtime.chunk_face_count != chunk_mesh.faces.size()
        || initial_runtime.chunk_mesh_upload_count == 0U
    ) {
        return std::unexpected("renderer capture did not upload the S5 chunk mesh");
    }
    if (renderer.captureState() != client::FrameCaptureState::Ready) {
        return std::unexpected("renderer capture is unsupported or unavailable");
    }
    std::span<client::PlayerRenderData const> const players{ };
    renderer.requestFrameCapture();
    auto const started_at = std::chrono::steady_clock::now();
    auto const deadline = started_at + options.deadline;
    uint64_t rendered_frames{ 0 };
    std::optional<uint64_t> first_frame_mesh_upload_count;
    bool observed_stone_draw = false;
    bool mesh_uploads_stayed_stable = true;
    for (uint64_t frame{ 0 }; frame < options.max_frames; ++frame) {
        if (std::chrono::steady_clock::now() - started_at >= options.deadline) {
            return std::unexpected("renderer capture exceeded its monotonic deadline");
        }
        if (!window.nextFrame()) {
            return std::unexpected("renderer capture window was closed");
        }
        if (renderer.render(players, deadline)) {
            ++rendered_frames;
            client::RendererRuntimeInfo const frame_runtime = renderer.runtimeInfo();
            observed_stone_draw = observed_stone_draw
                || (
                    frame_runtime.chunk_face_count == chunk_mesh.faces.size()
                    && frame_runtime.chunk_draw_count > 0U
                );
            if (first_frame_mesh_upload_count.has_value()
                && frame_runtime.chunk_mesh_upload_count != *first_frame_mesh_upload_count
            ) {
                mesh_uploads_stayed_stable = false;
            } else if (!first_frame_mesh_upload_count.has_value()) {
                first_frame_mesh_upload_count = frame_runtime.chunk_mesh_upload_count;
            }
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
    if (!first_frame_mesh_upload_count.has_value() || !observed_stone_draw || !mesh_uploads_stayed_stable) {
        return std::unexpected("renderer capture did not render a frame");
    }
    auto const content = validateStoneFrame(
        capture->width,
        capture->height,
        capture->rgba8
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
    if (runtime.chunk_face_count != chunk_mesh.faces.size()
    ) {
        return std::unexpected("renderer capture did not retain and draw the cached S5 chunk mesh");
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
