#pragma once

#include <client/Camera.hpp>
#include <client/render/DebugHud.hpp>
#include <client/render/ShaderAssets.hpp>
#include <client/render/StoneFaceCapacity.hpp>
#include <client/render/TextRenderer.hpp>

#include <core/common/NonCopyable.hpp>
#include <core/common/NonMovable.hpp>
#include <core/common/Version.hpp>

#include <core/graphics/vulkan/Vulkan.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace core::platform::glfw {
class GlfwWindow;
} // namespace core::platform::glfw

struct ANativeWindow;

namespace shared {
struct ChunkMesh;
struct HeightTileCoordinate;
struct HeightTileSurfaceMesh;
} // namespace shared

namespace client {

struct PlayerRenderData final {
    float x = 0.0F;
    float y = 0.0F;
    std::array<float, 4> color{ 1.0f, 1.0f, 1.0f, 1.0f };
    float z = 0.0F;
    bool render_on_top = false;
};

struct VulkanRendererOptions final {
    bool require_validation = false;
    bool prefer_mesh_shaders = true;
    bool enable_frame_capture = false;
    bool require_immediate_present_mode = false;
    bool enable_presentation_history = false;
};

namespace detail {

[[nodiscard]]
inline bool diagnosticTerrainRangeVisible(
    bool const enabled,
    bool const identity_matches,
    VkResult const result,
    uint64_t const passing_samples,
    uint64_t const available
) noexcept
{
    return !enabled || !identity_matches || result != VK_SUCCESS || available == 0U || passing_samples != 0U;
}

} // namespace detail

enum class RendererPipelinePath : uint8_t {
    Vertex,
    Mesh,
};

enum class RendererPresentMode : uint8_t {
    Immediate,
    Mailbox,
    FIFO,
    FIFORelaxed,
    Unknown,
};

enum class FrameCaptureState : uint8_t {
    Disabled,
    Ready,
    Completed,
    UnsupportedSwapchainUsage,
    UnsupportedFormat,
    Failed,
};

struct RendererHeightTileRemovalDiagnostics final {
    std::chrono::nanoseconds prefilter_duration{ 0 };
    std::chrono::nanoseconds slot_retirement_duration{ 0 };
    std::chrono::nanoseconds draw_index_remove_duration{ 0 };
    uint64_t input_coordinate_count = 0U;
    uint64_t matching_slot_count = 0U;
    uint64_t removed_slot_count = 0U;
};

struct RendererRuntimeInfo final {
    uint32_t width = 0;
    uint32_t height = 0;
    std::string gpu_name;
    core::Version vulkan_api_version;
    bool validation_enabled = false;
    RendererPresentMode present_mode = RendererPresentMode::Unknown;
    RendererPipelinePath pipeline_path = RendererPipelinePath::Vertex;
    bool presentation_history_enabled = false;
    uint64_t current_swapchain_generation = 0U;
    std::optional<uint32_t> successful_presentation_id;
    uint64_t successful_presentation_swapchain_generation = 0U;
    std::chrono::nanoseconds cpu_acquire_wait_duration{ 0 };
    core::graphics::vulkan::PresentationAcquireDiagnostics diagnostic_presentation_acquire;
    std::chrono::nanoseconds cpu_presentation_recreation_duration{ 0 };
    bool presentation_recreation_attempted = false;
    bool presentation_recreation_deferred = false;
    std::chrono::nanoseconds cpu_command_record_duration{ 0 };
    std::chrono::nanoseconds cpu_visible_range_collection_duration{ 0 };
    std::chrono::nanoseconds cpu_height_tile_group_and_tile_traversal_duration{ 0 };
    std::chrono::nanoseconds cpu_height_tile_pending_range_order_duration{ 0 };
    std::chrono::nanoseconds cpu_height_tile_visible_range_merge_duration{ 0 };
    std::chrono::nanoseconds cpu_indirect_prepare_duration{ 0 };
    std::chrono::nanoseconds cpu_stone_scene_record_duration{ 0 };
    // Recording outside the three terrain phases includes scene and presentation work.
    std::chrono::nanoseconds cpu_command_record_residual_duration{ 0 };
    std::chrono::nanoseconds cpu_complete_present_wait_duration{ 0 };
    std::chrono::nanoseconds cpu_frame_duration{ 0 };
    std::optional<std::chrono::nanoseconds> gpu_frame_duration;
    uint64_t submitted_frame_count{ 0 };
    uint32_t debug_hud_draw_count{ 0 };
    uint32_t chunk_face_count{ 0 };
    uint32_t chunk_draw_count{ 0 };
    uint32_t chunk_draw_face_count{ 0 };
    uint64_t chunk_mesh_upload_count{ 0 };
    uint32_t height_tile_mesh_count{ 0 };
    RendererHeightTileRemovalDiagnostics diagnostic_height_tile_removal;
    uint32_t stone_face_capacity{ 0 };
    StoneFaceGrowthDiagnostics stone_face_growth;
    uint64_t diagnostic_render_attempt_id{ 0 };
    bool diagnostic_frame_recorded = false;
    bool diagnostic_indexed_stone_quads = false;
    bool diagnostic_stone_indirect = false;
    std::optional<uint32_t> diagnostic_frame_slot;
    uint64_t diagnostic_submitted_stone_quad_count = 0U;
    uint32_t diagnostic_stone_draw_count = 0U;
    std::array<double, 7> diagnostic_camera{};
    bool diagnostic_gpu_timestamps_enabled = false;
    std::string_view diagnostic_gpu_timestamp_reason;
    uint64_t diagnostic_gpu_sample_attempt_id = 0U;
    uint32_t diagnostic_gpu_sample_slot = 0U;
    uint64_t diagnostic_gpu_sample_quad_count = 0U;
    uint32_t diagnostic_gpu_sample_draw_count = 0U;
    std::optional<std::chrono::nanoseconds> diagnostic_gpu_terrain_duration;
    bool diagnostic_terrain_occlusion_enabled = false;
    bool diagnostic_terrain_occlusion_oracle_frame = false;
    bool diagnostic_terrain_occlusion_filtered_frame = false;
    uint64_t diagnostic_terrain_occlusion_removed_quad_count = 0U;
    uint32_t diagnostic_terrain_occlusion_removed_range_count = 0U;
    uint32_t diagnostic_terrain_occlusion_removed_command_count = 0U;
};

struct RendererHeightTileUpsertDiagnostics final {
    std::chrono::nanoseconds free_range_prepare_duration{ 0 };
    std::chrono::nanoseconds mesh_prepare_duration{ 0 };
    std::chrono::nanoseconds capacity_prepare_duration{ 0 };
    std::chrono::nanoseconds commit_upload_duration{ 0 };
};

struct RendererFrameCapture final {
    uint32_t width = 0;
    uint32_t height = 0;
    bool srgb_encoded = false;
    std::vector<uint8_t> rgba8;
};

class VulkanRenderer final : core::NonCopyable, core::NonMovable {
public:
    [[nodiscard]]
    static std::shared_ptr<core::graphics::vulkan::PresentationContext> createPresentationContext(
        core::platform::glfw::GlfwWindow const& window,
        VulkanRendererOptions options = {}
    );
    explicit VulkanRenderer(
        std::shared_ptr<core::graphics::vulkan::PresentationContext> context,
        ShaderAssets const& shader_assets,
        VulkanRendererOptions options = {}
    );
    [[nodiscard]]
    static std::shared_ptr<core::graphics::vulkan::PresentationContext> createPresentationContext(
        ANativeWindow* window,
        uint32_t width,
        uint32_t height,
        VulkanRendererOptions options = {}
    );
    ~VulkanRenderer();

    [[nodiscard]]
    bool render(
        std::span<PlayerRenderData const> players,
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()
    );
    [[nodiscard]]
    bool render(
        std::span<PlayerRenderData const> players,
        DebugHudInput debug_hud_input,
        float debug_hud_dpi_scale,
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()
    );
    void setDebugHudEnabled(bool enabled) noexcept;
    void toggleDebugHud() noexcept;
    void setGuiText(std::string_view text, TextColor color = {});
    void addGuiText(std::string_view text, TextPlacement placement, TextColor color = {});
    void clearGuiText() noexcept;
    void setWorldText(std::string_view text, TextPlacement placement, TextColor color = {});
    void addWorldText(std::string_view text, TextPlacement placement, TextColor color = {});
    void clearWorldText() noexcept;
    void setCamera(CameraPose pose) noexcept;
    void setCamera(CameraPose pose, CameraProjection projection) noexcept;
    void setChunkMesh(shared::ChunkMesh const& mesh);
    void setChunkMeshes(std::span<shared::ChunkMesh const> meshes);
    [[nodiscard]]
    bool upsertHeightTileMesh(
        shared::HeightTileSurfaceMesh const& mesh,
        std::chrono::steady_clock::time_point deadline
    );
    [[nodiscard]]
    bool upsertHeightTileMeshes(
        std::span<shared::HeightTileSurfaceMesh const> meshes,
        std::chrono::steady_clock::time_point deadline,
        RendererHeightTileUpsertDiagnostics* diagnostics = nullptr
    );
    [[nodiscard]] bool removeHeightTileMesh(shared::HeightTileCoordinate coordinate);
    [[nodiscard]]
    uint32_t removeHeightTileMeshes(
        std::span<shared::HeightTileCoordinate const> coordinates,
        bool capture_diagnostics = false
    );
    void hotReload();
    void recreate(uint32_t width, uint32_t height);
    [[nodiscard]]
    bool waitForSubmittedFrames(
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()
    );

    void requestFrameCapture();
    [[nodiscard]] FrameCaptureState captureState() const;
    [[nodiscard]] std::optional<RendererFrameCapture> takeFrameCapture();
    [[nodiscard]] RendererRuntimeInfo runtimeInfo() const;
    [[nodiscard]] core::graphics::vulkan::PresentationHistoryResult queryPresentationHistory(
        std::span<core::graphics::vulkan::PresentationTiming> records
    );
    [[nodiscard]] uint32_t validationErrorCount() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class VulkanOffscreenRenderer final : core::NonCopyable, core::NonMovable {
public:
    explicit VulkanOffscreenRenderer(
        ShaderAssets const& shader_assets,
        bool require_validation = false
    );
    ~VulkanOffscreenRenderer();

    [[nodiscard]] RendererFrameCapture render(
        std::span<PlayerRenderData const> players,
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()
    );
    void setCamera(CameraPose pose) noexcept;
    void setChunkMesh(shared::ChunkMesh const& mesh);
    void setDebugHudEnabled(bool enabled) noexcept;
    [[nodiscard]] bool validationEnabled() const noexcept;
    [[nodiscard]] uint32_t validationErrorCount() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace client
