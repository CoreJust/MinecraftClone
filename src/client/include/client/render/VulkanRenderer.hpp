#pragma once

#include <client/render/ShaderAssets.hpp>

#include <core/common/NonCopyable.hpp>
#include <core/common/NonMovable.hpp>
#include <core/common/Version.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace core::vk {

class SurfaceProvider;

} // namespace core::vk

namespace client {

struct PlayerRenderData final {
    uint32_t x = 0;
    uint32_t y = 0;
    std::array<float, 4> color{ 1.0f, 1.0f, 1.0f, 1.0f };
};

struct VulkanRendererOptions final {
    bool require_validation = false;
    bool prefer_mesh_shaders = true;
    bool enable_frame_capture = false;
    bool require_immediate_present_mode = false;
};

enum class RendererPipelinePath {
    Vertex,
    Mesh,
};

enum class RendererPresentMode {
    Immediate,
    Mailbox,
    FIFO,
    FIFORelaxed,
    Unknown,
};

enum class FrameCaptureState {
    Disabled,
    Ready,
    Completed,
    UnsupportedSwapchainUsage,
    UnsupportedFormat,
    Failed,
};

struct RendererRuntimeInfo final {
    uint32_t width = 0;
    uint32_t height = 0;
    std::string gpu_name;
    core::Version vulkan_api_version;
    bool validation_enabled = false;
    RendererPresentMode present_mode = RendererPresentMode::Unknown;
    RendererPipelinePath pipeline_path = RendererPipelinePath::Vertex;
    std::chrono::nanoseconds cpu_frame_duration{ 0 };
    std::optional<std::chrono::nanoseconds> gpu_frame_duration;
    uint64_t submitted_frame_count{ 0 };
};

struct RendererFrameCapture final {
    uint32_t width = 0;
    uint32_t height = 0;
    bool srgb_encoded = false;
    std::vector<uint8_t> rgba8;
};

class VulkanRenderer final : core::NonCopyable, core::NonMovable {
public:
    explicit VulkanRenderer(
        core::vk::SurfaceProvider const& surface_provider,
        ShaderAssets const& shader_assets,
        VulkanRendererOptions options = {}
    );
    ~VulkanRenderer();

    [[nodiscard]]
    bool render(
        std::span<PlayerRenderData const> players,
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()
    );
    void hotReload();
    void waitIdle();
    [[nodiscard]]
    bool waitForSubmittedFrames(
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()
    );

    void requestFrameCapture();
    [[nodiscard]] FrameCaptureState captureState() const;
    [[nodiscard]] std::optional<RendererFrameCapture> takeFrameCapture();
    [[nodiscard]] RendererRuntimeInfo runtimeInfo() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace client
