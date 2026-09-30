#include <client/Camera.hpp>
#include <client/PlayerPresentation.hpp>
#include <client/render/InstalledShaderAssets.hpp>
#include <client/render/StoneFaceCapacity.hpp>
#include <client/render/VulkanRenderer.hpp>

#include <shared/world/ChunkMesher.hpp>
#include <shared/world/HeightTileSurfaceMesher.hpp>

#include <core/platform/glfw/GlfwWindow.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <gtest/gtest.h>

#include <testsupport/ImageComparison.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

class InertPresentationBackend final : public core::graphics::vulkan::PresentationBackend {
public:
    [[nodiscard]] core::graphics::vulkan::FenceWaitResult waitForSlot(
        uint32_t,
        std::chrono::nanoseconds
    ) override
    {
        return core::graphics::vulkan::FenceWaitResult::Completed;
    }

    [[nodiscard]] core::graphics::vulkan::FenceWaitResult waitForRecreation(
        std::chrono::nanoseconds
    ) override
    {
        return core::graphics::vulkan::FenceWaitResult::Completed;
    }

    [[nodiscard]] core::graphics::vulkan::PresentationAcquireResult acquire(
        uint32_t,
        std::chrono::nanoseconds,
        uint32_t& image
    ) override
    {
        image = 0U;
        return core::graphics::vulkan::PresentationAcquireResult::NotReady;
    }

    void record(VkCommandBuffer, void (*)(VkCommandBuffer, void*), void*) override {}
    void submit(uint32_t, VkCommandBuffer) override {}

    [[nodiscard]] core::graphics::vulkan::PresentationAcquireResult present(
        uint32_t,
        uint32_t
    ) override
    {
        return core::graphics::vulkan::PresentationAcquireResult::NotReady;
    }

    void abandon(uint32_t, uint32_t) noexcept override {}
    void recreate(VkExtent2D) override {}

    [[nodiscard]] std::span<uint8_t const> completedReadback(uint32_t) const noexcept override
    {
        return {};
    }
};

struct CaptureColorClasses final {
    bool has_grid = false;
    bool has_platform = false;
    bool has_platform_side = false;
    bool has_sky = false;
    bool has_red_player = false;
    bool has_green_player = false;
    bool has_debug_hud = false;

    [[nodiscard]] bool complete() const
    {
        return has_grid && has_platform && has_platform_side && has_sky
            && has_red_player && has_green_player && has_debug_hud;
    }

    [[nodiscard]] std::string missingClasses() const
    {
        std::string missing;
        auto appendMissing = [&missing](bool const present, char const* const name) {
            if (!present) {
                if (!missing.empty()) {
                    missing += ", ";
                }
                missing += name;
            }
        };
        appendMissing(has_grid, "grid");
        appendMissing(has_platform, "platform");
        appendMissing(has_platform_side, "platform-side");
        appendMissing(has_sky, "sky");
        appendMissing(has_red_player, "red-player");
        appendMissing(has_green_player, "green-player");
        appendMissing(has_debug_hud, "debug-hud");
        return missing;
    }
};

[[nodiscard]] shared::HeightTileSurfaceMesh denseHeightTileMesh(int32_t const tile_x)
{
    shared::HeightTileSurfaceMesh mesh{ .coordinate = { .x = tile_x, .y = 0 } };
    mesh.quads.reserve(shared::HeightTileSurfaceMesh::MAXIMUM_QUAD_COUNT);
    for (uint32_t index = 0U; index < shared::HeightTileSurfaceMesh::MAXIMUM_QUAD_COUNT; ++index) {
        mesh.quads.push_back({
            .x = tile_x * static_cast<int32_t>(shared::HeightTile::SIDE_LENGTH)
                + static_cast<int32_t>(index % shared::HeightTile::SIDE_LENGTH),
            .y = static_cast<int32_t>(index / shared::HeightTile::SIDE_LENGTH),
            .z = 6,
            .direction = shared::HeightTileSurfaceDirection::PositiveZ,
        });
    }
    return mesh;
}

[[nodiscard]] bool isObservableHudPixel(
    uint8_t const red,
    uint8_t const green,
    uint8_t const blue
)
{
    uint8_t const maximum = std::max(red, std::max(green, blue));
    uint8_t const minimum = std::min(red, std::min(green, blue));
    bool const bright_color = maximum >= 220U && maximum - minimum >= 35U;
    bool const bright_neutral = minimum >= 180U;
    // The presentation background is a blue-dominant sky. Keep its high blue
    // channel from being mistaken for generic colored text, while allowing the
    // brighter cyan HUD row through.
    bool const sky_like = blue > green + 20U && green > red + 20U && green < 200U;
    return bright_neutral || (bright_color && !sky_like);
}

[[nodiscard]] CaptureColorClasses classifyCaptureColors(client::RendererFrameCapture const& capture)
{
    CaptureColorClasses classes;
    uint32_t debug_hud_pixel_count = 0U;
    uint8_t const grid_min = capture.srgb_encoded ? 90U : 32U;
    uint8_t const grid_max = capture.srgb_encoded ? 160U : 64U;
    for (uint64_t offset = 0U; offset < capture.rgba8.size(); offset += 4U) {
        uint32_t const pixel_index = static_cast<uint32_t>(offset / 4U);
        uint32_t const x = pixel_index % capture.width;
        uint32_t const y = pixel_index / capture.width;
        uint8_t const red = capture.rgba8[offset];
        uint8_t const green = capture.rgba8[offset + 1U];
        uint8_t const blue = capture.rgba8[offset + 2U];
        classes.has_grid = classes.has_grid || (
            red > grid_min && red < grid_max && green > grid_min && green < grid_max
                && blue > grid_min && blue < grid_max
        );
        classes.has_sky = classes.has_sky || (
            blue > green + 20U && green > red + 20U
        );
        classes.has_platform = classes.has_platform || (
            red < 130U && blue < 170U && blue > green + 2U && green > red + 2U
        );
        classes.has_platform_side = classes.has_platform_side || (
            y > 0U
            && red < 130U && blue < 170U && blue > green + 2U && green > red + 2U
            && capture.rgba8[offset - static_cast<uint64_t>(capture.width) * 4U] < 130U
            && capture.rgba8[offset - static_cast<uint64_t>(capture.width) * 4U + 2U] < 170U
            && capture.rgba8[offset - static_cast<uint64_t>(capture.width) * 4U + 2U]
                > capture.rgba8[offset - static_cast<uint64_t>(capture.width) * 4U + 1U] + 2U
            && capture.rgba8[offset - static_cast<uint64_t>(capture.width) * 4U + 1U]
                > capture.rgba8[offset - static_cast<uint64_t>(capture.width) * 4U] + 2U
        );
        classes.has_red_player = classes.has_red_player || (
            red > 200U && green < 80U && blue < 80U
        );
        classes.has_green_player = classes.has_green_player || (
            red < 80U && green > 150U && blue < 80U
        );
        if (x < 220U && y < 80U && isObservableHudPixel(red, green, blue)) {
            ++debug_hud_pixel_count;
        }
    }
    classes.has_debug_hud = debug_hud_pixel_count >= 20U;
    return classes;
}

TEST(RendererSmokeTest, CompletedCaptureSurvivesRecreateAndReadsBack)
{
    static constexpr uint32_t INITIAL_LOGICAL_WIDTH = 320U;
    static constexpr uint32_t INITIAL_LOGICAL_HEIGHT = 240U;
    static constexpr int32_t RESIZED_WIDTH = 400;
    static constexpr int32_t RESIZED_HEIGHT = 300;
    static constexpr uint32_t MAX_CAPTURE_FRAMES = 12U;
    static constexpr uint32_t MAXIMUM_INITIAL_STONE_FACE_CAPACITY = 65'536U;
    static constexpr auto TIMEOUT = std::chrono::seconds{ 10 };
    core::platform::glfw::GlfwWindow window{
        core::platform::glfw::WindowDescriptor{
            .width = INITIAL_LOGICAL_WIDTH,
            .height = INITIAL_LOGICAL_HEIGHT,
            .title = "MinecraftClone presentation smoke",
        },
    };
    uint32_t initial_framebuffer_width = 0U;
    uint32_t initial_framebuffer_height = 0U;
    window.framebufferSize(initial_framebuffer_width, initial_framebuffer_height);
    ASSERT_GT(initial_framebuffer_width, 0U);
    ASSERT_GT(initial_framebuffer_height, 0U);
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(
            window,
            { .require_validation = true, .enable_frame_capture = true }
        ),
        shader_assets,
        { .require_validation = true, .enable_frame_capture = true },
    };
    renderer.setDebugHudEnabled(false);
    EXPECT_LE(renderer.runtimeInfo().stone_face_capacity, MAXIMUM_INITIAL_STONE_FACE_CAPACITY);
    std::array<client::PlayerRenderData, 2> const players{
        client::PlayerRenderData{ .x = 2U, .y = 3U, .color = { 1.0F, 0.0F, 0.0F, 1.0F } },
        client::PlayerRenderData{ .x = 29U, .y = 28U, .color = { 0.0F, 1.0F, 0.0F, 1.0F } },
    };
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    uint32_t rendered_frames = 0U;
    auto completeCapture = [&] {
        for (uint32_t frame = 0U;
             frame < MAX_CAPTURE_FRAMES && std::chrono::steady_clock::now() < deadline;
             ++frame) {
            if (!window.nextFrame()) {
                return false;
            }
            if (renderer.render(
                    players,
                    client::DebugHudInput{ .player_x = 2.0F, .player_y = 3.0F },
                    1.0F,
                    deadline
                )) {
                ++rendered_frames;
            }
            if (renderer.captureState() == client::FrameCaptureState::Completed) {
                return true;
            }
        }
        return false;
    };

    renderer.requestFrameCapture();
    ASSERT_TRUE(completeCapture());
    ASSERT_EQ(renderer.captureState(), client::FrameCaptureState::Completed);
    std::optional<client::RendererFrameCapture> const original_capture = renderer.takeFrameCapture();
    ASSERT_TRUE(original_capture.has_value());
    EXPECT_EQ(original_capture->width, initial_framebuffer_width);
    EXPECT_EQ(original_capture->height, initial_framebuffer_height);
    EXPECT_EQ(
        original_capture->rgba8.size(),
        static_cast<uint64_t>(initial_framebuffer_width) * initial_framebuffer_height * 4U
    );
    EXPECT_EQ(renderer.runtimeInfo().debug_hud_draw_count, 0U);

    renderer.requestFrameCapture();
    ASSERT_TRUE(completeCapture());
    ASSERT_EQ(renderer.captureState(), client::FrameCaptureState::Completed);

    glfwSetWindowSize(window.nativeHandle(), RESIZED_WIDTH, RESIZED_HEIGHT);
    ASSERT_TRUE(window.nextFrame());
    uint32_t width = 0U;
    uint32_t height = 0U;
    window.framebufferSize(width, height);
    ASSERT_TRUE(width != initial_framebuffer_width || height != initial_framebuffer_height);
    renderer.recreate(width, height);
    ASSERT_EQ(renderer.captureState(), client::FrameCaptureState::Completed);
    std::optional<client::RendererFrameCapture> const retained_capture = renderer.takeFrameCapture();
    ASSERT_TRUE(retained_capture.has_value());
    EXPECT_EQ(retained_capture->width, initial_framebuffer_width);
    EXPECT_EQ(retained_capture->height, initial_framebuffer_height);
    EXPECT_EQ(retained_capture->srgb_encoded, original_capture->srgb_encoded);
    EXPECT_EQ(
        retained_capture->rgba8.size(),
        static_cast<uint64_t>(initial_framebuffer_width) * initial_framebuffer_height * 4U
    );
    EXPECT_EQ(retained_capture->rgba8, original_capture->rgba8);
    CaptureColorClasses const retained_colors = classifyCaptureColors(*retained_capture);
    EXPECT_TRUE(retained_colors.has_grid);
    EXPECT_TRUE(retained_colors.has_platform);
    EXPECT_TRUE(retained_colors.has_platform_side);
    EXPECT_TRUE(retained_colors.has_sky);
    EXPECT_TRUE(retained_colors.has_red_player);
    EXPECT_TRUE(retained_colors.has_green_player);

    renderer.setDebugHudEnabled(true);
    renderer.requestFrameCapture();
    ASSERT_TRUE(completeCapture());
    ASSERT_EQ(renderer.captureState(), client::FrameCaptureState::Completed);
    std::optional<client::RendererFrameCapture> const subsequent_capture = renderer.takeFrameCapture();
    ASSERT_TRUE(subsequent_capture.has_value());
    EXPECT_EQ(subsequent_capture->width, width);
    EXPECT_EQ(subsequent_capture->height, height);
    EXPECT_EQ(
        subsequent_capture->rgba8.size(),
        static_cast<uint64_t>(width) * height * 4U
    );
    CaptureColorClasses const subsequent_colors = classifyCaptureColors(*subsequent_capture);
    EXPECT_TRUE(subsequent_colors.complete())
        << "subsequent capture lacks: " << subsequent_colors.missingClasses()
        << "; the resized platform/grid/sky/HUD scene may cover every physical framebuffer sample";
    EXPECT_EQ(renderer.runtimeInfo().debug_hud_draw_count, 1U);

    renderer.hotReload();
    renderer.requestFrameCapture();
    ASSERT_TRUE(completeCapture());
    std::optional<client::RendererFrameCapture> const reloaded_capture = renderer.takeFrameCapture();
    ASSERT_TRUE(reloaded_capture.has_value());
    CaptureColorClasses const reloaded_colors = classifyCaptureColors(*reloaded_capture);
    EXPECT_TRUE(reloaded_colors.complete())
        << "reloaded capture lacks: " << reloaded_colors.missingClasses()
        << "; the platform/grid/sky scene may cover every physical framebuffer sample";
    EXPECT_GE(rendered_frames, 3U);
    EXPECT_EQ(renderer.runtimeInfo().pipeline_path, client::RendererPipelinePath::Vertex);
}

TEST(RendererSmokeTest, GrowsStoneFaceArenaWithoutBreakingPresentation)
{
    static constexpr uint32_t LOGICAL_WIDTH = 320U;
    static constexpr uint32_t LOGICAL_HEIGHT = 240U;
    static constexpr uint32_t TILE_COUNT = 65U;
    static constexpr uint32_t INITIAL_CAPACITY = 65'536U;
    static constexpr double CAMERA_Y = -20.0;
    static constexpr double CAMERA_Z = 18.0;
    static constexpr double CAMERA_X_OFFSET = 8.0;
    static constexpr double CAMERA_PITCH_DEGREES = -25.0;
    static constexpr auto TIMEOUT = std::chrono::seconds{ 10 };
    core::platform::glfw::GlfwWindow window{
        core::platform::glfw::WindowDescriptor{
            .width = LOGICAL_WIDTH,
            .height = LOGICAL_HEIGHT,
            .title = "MinecraftClone stone-face arena smoke",
        },
    };
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(
            window,
            { .require_validation = true }
        ),
        shader_assets,
        { .require_validation = true },
    };
    EXPECT_EQ(renderer.runtimeInfo().stone_face_capacity, INITIAL_CAPACITY);
    std::chrono::steady_clock::time_point const arena_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    for (uint32_t tile_index = 0U; tile_index < TILE_COUNT; ++tile_index) {
        ASSERT_TRUE(renderer.upsertHeightTileMesh(
            denseHeightTileMesh(static_cast<int32_t>(tile_index)),
            arena_deadline
        ));
    }
    EXPECT_GT(renderer.runtimeInfo().stone_face_capacity, INITIAL_CAPACITY);

    for (uint32_t tile_index = 1U; tile_index < TILE_COUNT - 1U; ++tile_index) {
        EXPECT_TRUE(renderer.removeHeightTileMesh({ .x = static_cast<int32_t>(tile_index), .y = 0 }));
    }

    auto const renderVisibleTile = [&renderer, &window](int32_t const tile_x) {
        renderer.setCamera({
            .position = {
                static_cast<double>(tile_x * static_cast<int32_t>(shared::HeightTile::SIDE_LENGTH))
                    + CAMERA_X_OFFSET,
                CAMERA_Y,
                CAMERA_Z,
            },
            .angles = { .pitch_degrees = CAMERA_PITCH_DEGREES },
        });
        ASSERT_TRUE(window.nextFrame());
        EXPECT_TRUE(renderer.render(
            {},
            client::DebugHudInput{},
            1.0F,
            std::chrono::steady_clock::now() + TIMEOUT
        ));
        EXPECT_GT(renderer.runtimeInfo().chunk_draw_count, 0U);
        EXPECT_EQ(renderer.runtimeInfo().chunk_draw_face_count, shared::HeightTileSurfaceMesh::MAXIMUM_QUAD_COUNT);
    };
    renderVisibleTile(0);
    renderVisibleTile(static_cast<int32_t>(TILE_COUNT - 1U));
    EXPECT_EQ(renderer.validationErrorCount(), 0U);
}

TEST(RendererSmokeTest, HeightTileBatchKeepsBothOldMeshesWhenDeadlinePreventsArenaGrowth)
{
    static constexpr uint32_t LOGICAL_WIDTH = 320U;
    static constexpr uint32_t LOGICAL_HEIGHT = 240U;
    static constexpr uint32_t FILLER_TILE_COUNT = 63U;
    static constexpr uint32_t INITIAL_CAPACITY = 65'536U;
    static constexpr uint32_t BATCH_TILE_COUNT = 2U;
    static constexpr auto TIMEOUT = std::chrono::seconds{ 10 };

    core::platform::glfw::GlfwWindow window{
        core::platform::glfw::WindowDescriptor{
            .width = LOGICAL_WIDTH,
            .height = LOGICAL_HEIGHT,
            .title = "MinecraftClone height tile batch smoke",
        },
    };
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(window, { .require_validation = true }),
        shader_assets,
        { .require_validation = true },
    };
    std::chrono::steady_clock::time_point const setup_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    for (uint32_t index = 0U; index < FILLER_TILE_COUNT; ++index) {
        ASSERT_TRUE(renderer.upsertHeightTileMesh(denseHeightTileMesh(static_cast<int32_t>(index)), setup_deadline));
    }

    std::array<shared::HeightTileSurfaceMesh, BATCH_TILE_COUNT> replacements;
    for (uint32_t index = 0U; index < BATCH_TILE_COUNT; ++index) {
        int32_t const tile_x = static_cast<int32_t>(FILLER_TILE_COUNT + index);
        shared::HeightTileSurfaceMesh original{ .coordinate = { .x = tile_x, .y = 0 } };
        original.quads.push_back({
            .x = tile_x * static_cast<int32_t>(shared::HeightTile::SIDE_LENGTH),
            .y = 0,
            .z = 6,
            .direction = shared::HeightTileSurfaceDirection::PositiveZ,
        });
        ASSERT_TRUE(renderer.upsertHeightTileMesh(original, setup_deadline));
        replacements[index] = denseHeightTileMesh(tile_x);
    }

    uint32_t const original_face_count = FILLER_TILE_COUNT * shared::HeightTileSurfaceMesh::MAXIMUM_QUAD_COUNT
        + BATCH_TILE_COUNT;
    ASSERT_EQ(renderer.runtimeInfo().stone_face_capacity, INITIAL_CAPACITY);
    ASSERT_EQ(renderer.runtimeInfo().height_tile_mesh_count, FILLER_TILE_COUNT + BATCH_TILE_COUNT);
    ASSERT_EQ(renderer.runtimeInfo().chunk_face_count, original_face_count);

    EXPECT_FALSE(renderer.upsertHeightTileMeshes(replacements, std::chrono::steady_clock::now()));
    EXPECT_EQ(renderer.runtimeInfo().stone_face_capacity, INITIAL_CAPACITY);
    EXPECT_EQ(renderer.runtimeInfo().height_tile_mesh_count, FILLER_TILE_COUNT + BATCH_TILE_COUNT);
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, original_face_count);

    ASSERT_TRUE(renderer.upsertHeightTileMeshes(replacements, std::chrono::steady_clock::now() + TIMEOUT));
    EXPECT_GT(renderer.runtimeInfo().stone_face_capacity, INITIAL_CAPACITY);
    EXPECT_EQ(renderer.runtimeInfo().height_tile_mesh_count, FILLER_TILE_COUNT + BATCH_TILE_COUNT);
    EXPECT_EQ(
        renderer.runtimeInfo().chunk_face_count,
        (FILLER_TILE_COUNT + BATCH_TILE_COUNT) * shared::HeightTileSurfaceMesh::MAXIMUM_QUAD_COUNT
    );
    EXPECT_EQ(renderer.validationErrorCount(), 0U);
}

TEST(RendererSmokeTest, ReportsExactResidentFaceCountsAcrossPublicationRemovalAndLegacyReset)
{
    static constexpr auto TIMEOUT = std::chrono::seconds{ 10 };
    static constexpr uint32_t FIRST_FACE_COUNT = 3U;
    static constexpr uint32_t SECOND_FACE_COUNT = 5U;
    static constexpr uint32_t REPLACEMENT_FACE_COUNT = 2U;
    core::platform::glfw::GlfwWindow window{
        { .width = 320U, .height = 240U, .title = "MinecraftClone resident face-count smoke" },
    };
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(window, { .require_validation = true }),
        shader_assets,
        { .require_validation = true },
    };
    auto const deadline = [] { return std::chrono::steady_clock::now() + TIMEOUT; };
    shared::HeightTileSurfaceMesh first = denseHeightTileMesh(0);
    shared::HeightTileSurfaceMesh second = denseHeightTileMesh(1);
    shared::HeightTileSurfaceMesh third = denseHeightTileMesh(2);
    first.quads.resize(FIRST_FACE_COUNT);
    second.quads.resize(SECOND_FACE_COUNT);
    third.quads.resize(REPLACEMENT_FACE_COUNT);
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, 0U);
    ASSERT_TRUE(renderer.upsertHeightTileMesh(first, deadline()));
    ASSERT_TRUE(renderer.upsertHeightTileMesh(first, deadline()));
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, FIRST_FACE_COUNT);
    ASSERT_TRUE(renderer.upsertHeightTileMesh(second, deadline()));
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, FIRST_FACE_COUNT + SECOND_FACE_COUNT);
    first.quads.resize(REPLACEMENT_FACE_COUNT);
    ASSERT_TRUE(renderer.upsertHeightTileMesh(first, deadline()));
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, REPLACEMENT_FACE_COUNT + SECOND_FACE_COUNT);

    first.quads.clear();
    second.quads.resize(FIRST_FACE_COUNT);
    std::array const batch{ first, second, third };
    ASSERT_TRUE(renderer.upsertHeightTileMeshes(batch, deadline()));
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, FIRST_FACE_COUNT + REPLACEMENT_FACE_COUNT);
    EXPECT_EQ(renderer.runtimeInfo().height_tile_mesh_count, 2U);
    second.quads.clear();
    ASSERT_TRUE(renderer.upsertHeightTileMesh(second, deadline()));
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, REPLACEMENT_FACE_COUNT);
    EXPECT_FALSE(renderer.removeHeightTileMesh(second.coordinate));
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, REPLACEMENT_FACE_COUNT);
    ASSERT_TRUE(renderer.removeHeightTileMesh(third.coordinate));
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, 0U);
    ASSERT_TRUE(renderer.upsertHeightTileMesh(third, deadline()));
    renderer.hotReload();
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, REPLACEMENT_FACE_COUNT);

    shared::ChunkMesher mesher;
    shared::ChunkMesh const legacy = mesher.update(shared::Chunk::makeStoneFixture({}));
    ASSERT_FALSE(legacy.faces.empty());
    renderer.setChunkMesh(legacy);
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, legacy.faces.size());
    EXPECT_EQ(renderer.runtimeInfo().height_tile_mesh_count, 0U);
    ASSERT_TRUE(renderer.upsertHeightTileMesh(third, deadline()));
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, REPLACEMENT_FACE_COUNT);
    std::array const legacy_batch{ legacy, legacy };
    renderer.setChunkMeshes(legacy_batch);
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, legacy.faces.size() * legacy_batch.size());
    EXPECT_EQ(renderer.runtimeInfo().height_tile_mesh_count, 0U);
    renderer.setChunkMeshes({});
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, 0U);
    ASSERT_TRUE(renderer.upsertHeightTileMesh(third, deadline()));
    renderer.setChunkMesh({});
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, 0U);
    EXPECT_EQ(renderer.runtimeInfo().height_tile_mesh_count, 0U);
    EXPECT_EQ(renderer.validationErrorCount(), 0U);
}

TEST(RendererSmokeTest, RejectedDuplicateBatchDoesNotChangeResidentFaceCounts)
{
    static constexpr auto TIMEOUT = std::chrono::seconds{ 10 };
    static constexpr uint32_t ORIGINAL_FACE_COUNT = 3U;
    core::platform::glfw::GlfwWindow window{
        { .width = 320U, .height = 240U, .title = "MinecraftClone rejected face-count smoke" },
    };
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(window, { .require_validation = true }),
        shader_assets,
        { .require_validation = true },
    };
    shared::HeightTileSurfaceMesh original = denseHeightTileMesh(0);
    original.quads.resize(ORIGINAL_FACE_COUNT);
    ASSERT_TRUE(renderer.upsertHeightTileMesh(original, std::chrono::steady_clock::now() + TIMEOUT));
    std::array const duplicate_batch{ denseHeightTileMesh(0), denseHeightTileMesh(0) };
    EXPECT_THROW(
        static_cast<void>(renderer.upsertHeightTileMeshes(duplicate_batch, std::chrono::steady_clock::now() + TIMEOUT)),
        std::invalid_argument
    );
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, ORIGINAL_FACE_COUNT);
    EXPECT_EQ(renderer.runtimeInfo().height_tile_mesh_count, 1U);
    EXPECT_EQ(renderer.validationErrorCount(), 0U);
}

TEST(RendererSmokeTest, FailedStoneFaceArenaGrowthRetriesWithOriginalDeadlineAndRestoresCapacity)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t REQUIRED_CAPACITY = 9U;
    static constexpr uint32_t MAXIMUM_CAPACITY = 16U;
    static constexpr auto FRAME_DEADLINE = std::chrono::steady_clock::time_point{
        std::chrono::steady_clock::duration{ 123'456 }
    };
    static constexpr std::array<uint32_t, 2> EXPECTED_CAPACITIES{ 16U, INITIAL_CAPACITY };

    uint32_t capacity = INITIAL_CAPACITY;
    std::vector<uint32_t> recreated_capacities;
    std::vector<std::chrono::steady_clock::time_point> recreation_deadlines;
    recreated_capacities.reserve(EXPECTED_CAPACITIES.size());
    recreation_deadlines.reserve(EXPECTED_CAPACITIES.size());
    auto recreate = [&](std::chrono::steady_clock::time_point const deadline) {
        recreated_capacities.push_back(capacity);
        recreation_deadlines.push_back(deadline);
        if (recreated_capacities.size() == 1U) {
            throw std::runtime_error("simulated capacity growth failure");
        }
    };

    EXPECT_FALSE(client::detail::tryGrowStoneFaceCapacity(
        capacity,
        REQUIRED_CAPACITY,
        MAXIMUM_CAPACITY,
        FRAME_DEADLINE,
        recreate
    ));

    EXPECT_EQ(capacity, INITIAL_CAPACITY);
    EXPECT_EQ(recreated_capacities, (std::vector<uint32_t>{ 16U, INITIAL_CAPACITY }));
    EXPECT_EQ(
        recreation_deadlines,
        (std::vector<std::chrono::steady_clock::time_point>{ FRAME_DEADLINE, FRAME_DEADLINE })
    );
}

TEST(RendererSmokeTest, RejectsASelectedTransformThatRequiresClientCompensation)
{
    auto context = core::graphics::vulkan::PresentationContext::createForTesting(
        {
            .extent = { .width = 1280U, .height = 720U },
            .surface_transform = {
                .pre_transform = VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR,
                .requires_client_orientation_compensation = true,
            },
        },
        std::make_unique<InertPresentationBackend>()
    );
    client::InstalledShaderAssets const shader_assets;

    EXPECT_THROW(
        static_cast<void>(client::VulkanRenderer{ std::move(context), shader_assets }),
        std::runtime_error
    );
}

TEST(RendererSmokeTest, GlfwInputAdapterPreservesPressAndRelease)
{
    core::platform::glfw::GlfwWindow window{
        core::platform::glfw::WindowDescriptor{
            .width = 64U,
            .height = 64U,
            .title = "MinecraftClone input adapter",
        },
    };
    EXPECT_FALSE(window.keyPressed(core::platform::glfw::WindowKey::W));
    window.injectKeyForTesting(core::platform::glfw::WindowKey::W, true);
    EXPECT_TRUE(window.keyPressed(core::platform::glfw::WindowKey::W));
    window.injectKeyForTesting(core::platform::glfw::WindowKey::W, false);
    EXPECT_FALSE(window.keyPressed(core::platform::glfw::WindowKey::W));
}

TEST(RendererSmokeTest, GlfwCursorCaptureSupportsContinuousCameraLook)
{
    core::platform::glfw::GlfwWindow window{
        core::platform::glfw::WindowDescriptor{
            .width = 64U,
            .height = 64U,
            .title = "MinecraftClone cursor capture",
        },
    };
    glfwSetInputMode(window.nativeHandle(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    EXPECT_EQ(glfwGetInputMode(window.nativeHandle(), GLFW_CURSOR), GLFW_CURSOR_DISABLED);
    glfwSetInputMode(window.nativeHandle(), GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    EXPECT_EQ(glfwGetInputMode(window.nativeHandle(), GLFW_CURSOR), GLFW_CURSOR_NORMAL);
}

TEST(RendererSmokeTest, ActiveThirdPersonRendererShowsPlatformPlayersAndTopLeftHud)
{
    static constexpr uint32_t LOGICAL_WIDTH = 320U;
    static constexpr uint32_t LOGICAL_HEIGHT = 240U;
    static constexpr uint32_t MAX_CAPTURE_FRAMES = 12U;
    static constexpr auto TIMEOUT = std::chrono::seconds{ 10 };
    static constexpr shared::Player LOCAL{
        .id = 1U,
        .x = 8U,
        .y = 8U,
        .ch = '@',
    };
    static constexpr shared::Player REMOTE{
        .id = 2U,
        .x = 12U,
        .y = 15U,
        .ch = '#',
    };
    core::platform::glfw::GlfwWindow window{
        core::platform::glfw::WindowDescriptor{
            .width = LOGICAL_WIDTH,
            .height = LOGICAL_HEIGHT,
            .title = "MinecraftClone active third-person renderer",
        },
    };
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(
            window,
            { .require_validation = true, .enable_frame_capture = true }
        ),
        shader_assets,
        { .require_validation = true, .enable_frame_capture = true },
    };
    renderer.setDebugHudEnabled(true);
    std::array<client::PlayerRenderData, 2U> const players{
        client::PlayerRenderData{
            .x = LOCAL.x,
            .y = LOCAL.y,
            .color = { 1.0F, 0.0F, 0.0F, 1.0F },
        },
        client::PlayerRenderData{
            .x = REMOTE.x,
            .y = REMOTE.y,
            .color = { 0.0F, 1.0F, 0.0F, 1.0F },
        },
    };
    EXPECT_FALSE(client::shouldRenderRemotePlayer(LOCAL, LOCAL.ch));
    EXPECT_TRUE(client::shouldRenderRemotePlayer(REMOTE, LOCAL.ch));
    client::Camera const camera{
        {
            .position = client::localPlayerThirdPersonPose(
                LOCAL,
                { .pitch_degrees = -10.0 }
            ).position,
            .angles = { .pitch_degrees = -10.0 },
        },
    };
    renderer.setCamera(camera.pose());
    renderer.requestFrameCapture();

    client::RendererFrameCapture capture;
    bool captured = false;
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    for (uint32_t frame = 0U;
         frame < MAX_CAPTURE_FRAMES && std::chrono::steady_clock::now() < deadline;
         ++frame) {
        ASSERT_TRUE(window.nextFrame());
        static_cast<void>(renderer.render(
            players,
            client::DebugHudInput{ .player_x = static_cast<float>(LOCAL.x), .player_y = static_cast<float>(LOCAL.y) },
            1.0F,
            deadline
        ));
        if (std::optional<client::RendererFrameCapture> const completed = renderer.takeFrameCapture(); completed.has_value()) {
            capture = std::move(*completed);
            captured = true;
            break;
        }
    }

    ASSERT_TRUE(captured);
    if (char const* const capture_path = std::getenv("MC_RENDERER_SMOKE_CAPTURE_PATH");
        capture_path != nullptr && capture_path[0] != '\0') {
        auto const written = testsupport::writePpm(
            {
                .width = capture.width,
                .height = capture.height,
                .srgb_encoded = capture.srgb_encoded,
                .pixels = capture.rgba8,
            },
            std::filesystem::path{ capture_path },
            "MC-AI-0118 normal close third-person HUD smoke capture"
        );
        ASSERT_TRUE(written.has_value()) << written.error();
    }
    CaptureColorClasses const classes = classifyCaptureColors(capture);
    EXPECT_TRUE(classes.has_grid);
    EXPECT_TRUE(classes.has_platform);
    EXPECT_TRUE(classes.has_platform_side);
    EXPECT_TRUE(classes.has_sky);
    EXPECT_TRUE(classes.has_debug_hud);
    EXPECT_TRUE(classes.has_red_player);
    EXPECT_TRUE(classes.has_green_player);
    EXPECT_EQ(renderer.runtimeInfo().pipeline_path, client::RendererPipelinePath::Vertex);
}

} // namespace
