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
#include <exception>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

TEST(RendererSmokeTest, DisabledTerrainOcclusionRetainsEveryRange)
{
    EXPECT_TRUE(client::detail::diagnosticTerrainRangeVisible(false, true, VK_SUCCESS, 0U, 1U));
    EXPECT_TRUE(client::detail::diagnosticTerrainRangeVisible(false, false, VK_NOT_READY, 0U, 0U));
}

TEST(RendererSmokeTest, TerrainOcclusionRetainsEveryNonzeroResult)
{
    EXPECT_FALSE(client::detail::diagnosticTerrainRangeVisible(true, true, VK_SUCCESS, 0U, 1U));
    EXPECT_TRUE(client::detail::diagnosticTerrainRangeVisible(true, true, VK_SUCCESS, 1U, 1U));
    EXPECT_TRUE(client::detail::diagnosticTerrainRangeVisible(true, true, VK_SUCCESS, UINT64_MAX, 1U));
}

TEST(RendererSmokeTest, TerrainOcclusionFailsClosedOnStaleUnavailableOrFailedQueries)
{
    EXPECT_TRUE(client::detail::diagnosticTerrainRangeVisible(true, false, VK_SUCCESS, 0U, 1U));
    EXPECT_TRUE(client::detail::diagnosticTerrainRangeVisible(true, true, VK_SUCCESS, 0U, 0U));
    EXPECT_TRUE(client::detail::diagnosticTerrainRangeVisible(true, true, VK_NOT_READY, 0U, 1U));
    EXPECT_TRUE(client::detail::diagnosticTerrainRangeVisible(true, true, VK_ERROR_DEVICE_LOST, 0U, 1U));
}

class InertPresentationBackend final : public core::graphics::vulkan::PresentationBackend {
public:
    core::graphics::vulkan::FenceWaitResult recreation_wait_result = core::graphics::vulkan::FenceWaitResult::Completed;
    bool ready = false;
    uint32_t acquire_count = 0U;
    uint32_t submit_count = 0U;
    std::vector<std::chrono::nanoseconds> recreation_wait_budgets;

    [[nodiscard]] core::graphics::vulkan::FenceWaitResult waitForSlot(
        uint32_t,
        std::chrono::nanoseconds
    ) override
    {
        return core::graphics::vulkan::FenceWaitResult::Completed;
    }

    [[nodiscard]] core::graphics::vulkan::FenceWaitResult waitForRecreation(
        std::chrono::nanoseconds const deadline
    ) override
    {
        recreation_wait_budgets.push_back(deadline);
        return recreation_wait_result;
    }

    [[nodiscard]] core::graphics::vulkan::PresentationAcquireResult acquire(
        uint32_t,
        std::chrono::nanoseconds,
        uint32_t& image
    ) override
    {
        ++acquire_count;
        image = 0U;
        return ready ? core::graphics::vulkan::PresentationAcquireResult::Ready
            : core::graphics::vulkan::PresentationAcquireResult::NotReady;
    }

    void record(VkCommandBuffer, void (*)(VkCommandBuffer, void*), void*) override {}
    void submit(uint32_t, VkCommandBuffer) override { ++submit_count; }

    [[nodiscard]] core::graphics::vulkan::PresentationAcquireResult present(
        uint32_t,
        uint32_t
    ) override
    {
        return ready ? core::graphics::vulkan::PresentationAcquireResult::Ready
            : core::graphics::vulkan::PresentationAcquireResult::NotReady;
    }

    void abandon(uint32_t, uint32_t) noexcept override {}
    void recreate(VkExtent2D) override {}

    [[nodiscard]] std::span<uint8_t const> completedReadback(uint32_t) const noexcept override
    {
        return {};
    }
};

struct PendingGrowthPresentationFixture final {
    InertPresentationBackend* backend = nullptr;
    std::shared_ptr<core::graphics::vulkan::PresentationContext> context;

    PendingGrowthPresentationFixture()
    {
        auto owned_backend = std::make_unique<InertPresentationBackend>();
        backend = owned_backend.get();
        backend->ready = true;
        context = core::graphics::vulkan::PresentationContext::createForTesting(
            { .extent = { .width = 64U, .height = 64U } }, std::move(owned_backend)
        );
    }

    [[nodiscard]] bool present()
    {
        std::optional<core::graphics::vulkan::PresentationContext::Frame> frame;
        if (context->acquire(std::chrono::milliseconds{ 8 }, frame)
                != core::graphics::vulkan::PresentationAcquireResult::Ready || !frame) {
            return false;
        }
        frame->record([](VkCommandBuffer, void*) {}, nullptr);
        context->complete(*frame);
        return true;
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

TEST(RendererSmokeTest, BatchUpsertCoalescesNewlyRetiredRangesWithReusableRanges)
{
    static constexpr uint32_t INITIAL_CAPACITY = 65'536U;
    static constexpr uint32_t MAXIMUM_TILE_FACE_COUNT = shared::HeightTileSurfaceMesh::MAXIMUM_QUAD_COUNT;
    static constexpr uint32_t SMALL_TILE_FACE_COUNT = MAXIMUM_TILE_FACE_COUNT / 2U;
    static constexpr uint32_t FULL_TILE_COUNT =
        (INITIAL_CAPACITY - 2U * SMALL_TILE_FACE_COUNT) / MAXIMUM_TILE_FACE_COUNT;
    static constexpr uint32_t FINAL_TILE_FACE_COUNT = INITIAL_CAPACITY - 2U * SMALL_TILE_FACE_COUNT
        - FULL_TILE_COUNT * MAXIMUM_TILE_FACE_COUNT;
    static constexpr uint32_t HALF_RETIREMENT_DELAY_FRAMES = 4U;
    static constexpr auto TIMEOUT = std::chrono::seconds{ 10 };

    core::platform::glfw::GlfwWindow window{
        { .width = 320U, .height = 240U, .title = "MinecraftClone retired stone-range smoke" },
    };
    glfwHideWindow(window.nativeHandle());
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(window, { .require_validation = true }),
        shader_assets,
        { .require_validation = true },
    };
    auto const deadline = [] { return std::chrono::steady_clock::now() + TIMEOUT; };
    auto mesh_with_faces = [](int32_t const tile_x, uint32_t const face_count) {
        shared::HeightTileSurfaceMesh mesh = denseHeightTileMesh(tile_x);
        mesh.quads.resize(face_count);
        return mesh;
    };

    std::vector<shared::HeightTileSurfaceMesh> resident_meshes;
    resident_meshes.reserve(FULL_TILE_COUNT + 3U);
    resident_meshes.push_back(mesh_with_faces(0, SMALL_TILE_FACE_COUNT));
    resident_meshes.push_back(mesh_with_faces(1, SMALL_TILE_FACE_COUNT));
    for (uint32_t index = 0U; index < FULL_TILE_COUNT; ++index) {
        resident_meshes.push_back(mesh_with_faces(static_cast<int32_t>(index + 2U), MAXIMUM_TILE_FACE_COUNT));
    }
    resident_meshes.push_back(mesh_with_faces(
        static_cast<int32_t>(FULL_TILE_COUNT + 2U), FINAL_TILE_FACE_COUNT
    ));
    for (shared::HeightTileSurfaceMesh const& mesh : resident_meshes) {
        ASSERT_TRUE(renderer.upsertHeightTileMesh(mesh, deadline()));
    }
    ASSERT_EQ(renderer.runtimeInfo().stone_face_capacity, INITIAL_CAPACITY);
    ASSERT_EQ(renderer.runtimeInfo().chunk_face_count, INITIAL_CAPACITY);

    EXPECT_TRUE(renderer.removeHeightTileMesh({ .x = 0, .y = 0 }));
    for (uint32_t frame = 0U; frame < HALF_RETIREMENT_DELAY_FRAMES; ++frame) {
        ASSERT_TRUE(window.nextFrame());
        ASSERT_TRUE(renderer.render({}, deadline()));
    }
    EXPECT_TRUE(renderer.removeHeightTileMesh({ .x = 1, .y = 0 }));
    for (uint32_t frame = 0U; frame < HALF_RETIREMENT_DELAY_FRAMES; ++frame) {
        ASSERT_TRUE(window.nextFrame());
        ASSERT_TRUE(renderer.render({}, deadline()));
    }
    std::array const flush_batch{ resident_meshes[2] };
    ASSERT_TRUE(renderer.upsertHeightTileMeshes(flush_batch, deadline()));

    for (uint32_t frame = 0U; frame < HALF_RETIREMENT_DELAY_FRAMES; ++frame) {
        ASSERT_TRUE(window.nextFrame());
        ASSERT_TRUE(renderer.render({}, deadline()));
    }
    std::array const replacement_batch{
        mesh_with_faces(static_cast<int32_t>(resident_meshes.size()), MAXIMUM_TILE_FACE_COUNT),
    };
    ASSERT_TRUE(renderer.upsertHeightTileMeshes(replacement_batch, deadline()));

    EXPECT_EQ(renderer.runtimeInfo().stone_face_capacity, INITIAL_CAPACITY);
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, INITIAL_CAPACITY);
    EXPECT_EQ(renderer.runtimeInfo().stone_face_growth.growth_attempt_count, 0U);
    EXPECT_EQ(renderer.validationErrorCount(), 0U);
}

TEST(RendererSmokeTest, TerrainOcclusionOrdinaryCaptureMatchesBaselineAndInvalidatesOnSceneChange)
{
    static constexpr uint32_t MAXIMUM_FRAMES = 24U;
    static constexpr auto TIMEOUT = std::chrono::seconds{ 10 };
    char const* const requested = std::getenv("MC_DIAGNOSTIC_TERRAIN_OCCLUSION");
    if (requested == nullptr || std::string_view{ requested } != "1") {
        GTEST_SKIP() << "set MC_DIAGNOSTIC_TERRAIN_OCCLUSION=1 to exercise the diagnostic oracle";
    }
    core::platform::glfw::GlfwWindow window{
        { .width = 320U, .height = 240U, .title = "MinecraftClone terrain occlusion diagnostic" },
    };
    glfwHideWindow(window.nativeHandle());
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(
            window, { .require_validation = true, .enable_frame_capture = true }
        ),
        shader_assets,
        { .require_validation = true, .enable_frame_capture = true },
    };
    if (!renderer.runtimeInfo().diagnostic_terrain_occlusion_enabled) {
        GTEST_SKIP() << "set MC_DIAGNOSTIC_TERRAIN_OCCLUSION=1 to exercise the diagnostic oracle";
    }
    renderer.setDebugHudEnabled(false);
    renderer.setCamera({ .position = { 8.0, -20.0, 25.0 }, .angles = { .pitch_degrees = -30.0 } });
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    shared::HeightTileSurfaceMesh mesh = denseHeightTileMesh(0);
    ASSERT_TRUE(renderer.upsertHeightTileMesh(mesh, deadline));
    ASSERT_TRUE(window.nextFrame());
    renderer.requestFrameCapture();
    ASSERT_TRUE(renderer.render({}, deadline));
    ASSERT_FALSE(renderer.runtimeInfo().diagnostic_terrain_occlusion_oracle_frame);
    ASSERT_FALSE(renderer.runtimeInfo().diagnostic_terrain_occlusion_filtered_frame);
    auto const baseline = renderer.takeFrameCapture();
    ASSERT_TRUE(baseline.has_value());

    bool saw_oracle = false;
    bool saw_filtered = false;
    renderer.requestFrameCapture();
    for (uint32_t frame = 0U; frame < MAXIMUM_FRAMES && std::chrono::steady_clock::now() < deadline; ++frame) {
        ASSERT_TRUE(window.nextFrame());
        ASSERT_TRUE(renderer.render({}, deadline));
        auto const info = renderer.runtimeInfo();
        if (info.diagnostic_terrain_occlusion_oracle_frame) {
            saw_oracle = true;
            EXPECT_FALSE(renderer.takeFrameCapture().has_value());
            EXPECT_FALSE(info.diagnostic_gpu_terrain_duration.has_value());
        }
        if (info.diagnostic_terrain_occlusion_filtered_frame) {
            saw_filtered = true;
            auto const filtered = renderer.takeFrameCapture();
            ASSERT_TRUE(filtered.has_value());
            EXPECT_EQ(filtered->rgba8, baseline->rgba8);
            EXPECT_EQ(filtered->width, baseline->width);
            EXPECT_EQ(filtered->height, baseline->height);
            break;
        }
    }
    ASSERT_TRUE(saw_oracle);
    ASSERT_TRUE(saw_filtered);

    renderer.setCamera({ .position = { 9.0, -20.0, 25.0 }, .angles = { .pitch_degrees = -30.0 } });
    ASSERT_TRUE(window.nextFrame());
    ASSERT_TRUE(renderer.render({}, deadline));
    EXPECT_FALSE(renderer.runtimeInfo().diagnostic_terrain_occlusion_filtered_frame);
    EXPECT_EQ(renderer.runtimeInfo().diagnostic_terrain_occlusion_removed_quad_count, 0U);
    mesh.quads.pop_back();
    ASSERT_TRUE(renderer.upsertHeightTileMesh(mesh, deadline));
    ASSERT_TRUE(window.nextFrame());
    ASSERT_TRUE(renderer.render({}, deadline));
    EXPECT_FALSE(renderer.runtimeInfo().diagnostic_terrain_occlusion_filtered_frame);
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
    glfwHideWindow(window.nativeHandle());
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(
            window, { .require_validation = true, .enable_frame_capture = true }
        ),
        shader_assets,
        { .require_validation = true, .enable_frame_capture = true },
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
    EXPECT_EQ(renderer.runtimeInfo().stone_face_growth.growth_attempt_count, 0U);
    EXPECT_EQ(renderer.runtimeInfo().stone_face_growth.before_recreate_count, 0U);
    EXPECT_EQ(renderer.runtimeInfo().stone_face_growth.actual_capacity, INITIAL_CAPACITY);
    EXPECT_EQ(renderer.runtimeInfo().stone_face_growth.actual_capacity_high_water, INITIAL_CAPACITY);
    renderer.setDebugHudEnabled(false);
    renderer.setCamera({ .position = { 1'024.0, -20.0, 18.0 }, .angles = { .pitch_degrees = -25.0 } });
    auto const capture_installed = [&] {
        renderer.requestFrameCapture();
        auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
        for (uint32_t frame = 0U; frame < 32U && std::chrono::steady_clock::now() < deadline; ++frame) {
            if (!window.nextFrame()) {
                break;
            }
            static_cast<void>(renderer.render({}, deadline));
            if (renderer.captureState() == client::FrameCaptureState::Completed) {
                break;
            }
        }
        return renderer.takeFrameCapture();
    };
    auto const original_capture = capture_installed();
    ASSERT_TRUE(original_capture.has_value());
    uint64_t const original_draw_faces = renderer.runtimeInfo().chunk_draw_face_count;

    EXPECT_FALSE(renderer.upsertHeightTileMeshes(replacements, std::chrono::steady_clock::now()));
    EXPECT_EQ(renderer.runtimeInfo().stone_face_capacity, INITIAL_CAPACITY);
    EXPECT_EQ(renderer.runtimeInfo().height_tile_mesh_count, FILLER_TILE_COUNT + BATCH_TILE_COUNT);
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, original_face_count);
    EXPECT_EQ(renderer.runtimeInfo().stone_face_growth.pre_growth_deadline_rejection_count, 1U);
    EXPECT_EQ(renderer.runtimeInfo().stone_face_growth.growth_attempt_count, 0U);
    EXPECT_EQ(renderer.runtimeInfo().stone_face_growth.publication_retry_count, 1U);
    EXPECT_GT(renderer.runtimeInfo().stone_face_growth.last_required_capacity, INITIAL_CAPACITY);
    auto const retained_capture = capture_installed();
    ASSERT_TRUE(retained_capture.has_value());
    EXPECT_EQ(retained_capture->rgba8, original_capture->rgba8);
    EXPECT_EQ(renderer.runtimeInfo().chunk_draw_face_count, original_draw_faces);

    ASSERT_TRUE(renderer.upsertHeightTileMeshes(replacements, std::chrono::steady_clock::now() + TIMEOUT));
    EXPECT_GT(renderer.runtimeInfo().stone_face_capacity, INITIAL_CAPACITY);
    auto const growth = renderer.runtimeInfo().stone_face_growth;
    EXPECT_EQ(growth.pre_growth_deadline_rejection_count, 1U);
    EXPECT_EQ(growth.growth_attempt_count, 1U);
    EXPECT_EQ(growth.before_recreate_count, 1U);
    EXPECT_EQ(growth.growth_success_count, 1U);
    EXPECT_EQ(growth.deferred_recreation_count, 0U);
    EXPECT_EQ(growth.recreation_exception_count, 0U);
    EXPECT_EQ(growth.rollback_count, 0U);
    EXPECT_EQ(growth.publication_retry_count, 1U);
    EXPECT_EQ(growth.last_target_capacity, renderer.runtimeInfo().stone_face_capacity);
    EXPECT_EQ(growth.actual_capacity, renderer.runtimeInfo().stone_face_capacity);
    EXPECT_EQ(growth.actual_capacity_high_water, renderer.runtimeInfo().stone_face_capacity);
    EXPECT_GE(growth.growth_work_duration, std::chrono::nanoseconds::zero());
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

TEST(RendererSmokeTest, DeferredArenaRecreationHasSeparateAttributionAndKeepsRollbackDeadline)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t REQUIRED_CAPACITY = 9U;
    static constexpr uint32_t MAXIMUM_CAPACITY = 16U;
    static constexpr auto FRAME_DEADLINE = std::chrono::steady_clock::time_point{
        std::chrono::steady_clock::duration{ 123'456 }
    };

    uint32_t capacity = INITIAL_CAPACITY;
    client::StoneFaceGrowthDiagnostics diagnostics;
    std::vector<uint32_t> recreated_capacities;
    auto const recreate = [&](std::chrono::steady_clock::time_point const deadline) {
        EXPECT_EQ(deadline, FRAME_DEADLINE);
        recreated_capacities.push_back(capacity);
        if (capacity != INITIAL_CAPACITY) {
            throw client::detail::StoneFaceRecreationDeferred{};
        }
    };
    EXPECT_FALSE(client::detail::tryGrowStoneFaceCapacity(
        capacity, REQUIRED_CAPACITY, MAXIMUM_CAPACITY, FRAME_DEADLINE, recreate, &diagnostics
    ));

    EXPECT_EQ(capacity, INITIAL_CAPACITY);
    EXPECT_EQ(recreated_capacities, (std::vector<uint32_t>{ MAXIMUM_CAPACITY, INITIAL_CAPACITY }));
    EXPECT_EQ(diagnostics.growth_attempt_count, 1U);
    EXPECT_EQ(diagnostics.before_recreate_count, 0U);
    EXPECT_EQ(diagnostics.deferred_recreation_count, 1U);
    EXPECT_EQ(diagnostics.recreation_exception_count, 0U);
    EXPECT_EQ(diagnostics.recreation_unknown_exception_count, 0U);
    EXPECT_EQ(std::string_view{ diagnostics.first_recreation_exception_reason.data() }, "");
    EXPECT_EQ(std::string_view{ diagnostics.last_recreation_exception_reason.data() }, "");
    EXPECT_EQ(diagnostics.recreation_exception_before_hook_count, 0U);
    EXPECT_EQ(diagnostics.growth_success_count, 0U);
    EXPECT_EQ(diagnostics.rollback_count, 1U);
    EXPECT_EQ(diagnostics.last_required_capacity, REQUIRED_CAPACITY);
    EXPECT_EQ(diagnostics.last_target_capacity, MAXIMUM_CAPACITY);
    EXPECT_GE(diagnostics.growth_work_duration, std::chrono::nanoseconds::zero());
}

TEST(RendererSmokeTest, ArenaRecreationExceptionBeforeHookDoesNotMasqueradeAsDeferralAndCanRetry)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t REQUIRED_CAPACITY = 9U;
    static constexpr uint32_t MAXIMUM_CAPACITY = 16U;
    static constexpr auto FRAME_DEADLINE = std::chrono::steady_clock::time_point{
        std::chrono::steady_clock::duration{ 123'456 }
    };

    uint32_t capacity = INITIAL_CAPACITY;
    client::StoneFaceGrowthDiagnostics diagnostics;
    bool fail_growth = true;
    auto const recreate = [&](std::chrono::steady_clock::time_point const deadline) {
        EXPECT_EQ(deadline, FRAME_DEADLINE);
        if (fail_growth && capacity != INITIAL_CAPACITY) {
            throw std::runtime_error("simulated native drain failure");
        }
    };
    EXPECT_FALSE(client::detail::tryGrowStoneFaceCapacity(
        capacity, REQUIRED_CAPACITY, MAXIMUM_CAPACITY, FRAME_DEADLINE, recreate, &diagnostics
    ));
    EXPECT_EQ(capacity, INITIAL_CAPACITY);
    EXPECT_EQ(diagnostics.deferred_recreation_count, 0U);
    EXPECT_EQ(diagnostics.recreation_exception_count, 1U);
    EXPECT_EQ(diagnostics.recreation_exception_before_hook_count, 1U);
    EXPECT_EQ(diagnostics.rollback_count, 1U);

    fail_growth = false;
    EXPECT_TRUE(client::detail::tryGrowStoneFaceCapacity(
        capacity, REQUIRED_CAPACITY, MAXIMUM_CAPACITY, FRAME_DEADLINE, recreate, &diagnostics
    ));
    EXPECT_EQ(capacity, MAXIMUM_CAPACITY);
    EXPECT_EQ(diagnostics.growth_attempt_count, 2U);
    EXPECT_EQ(diagnostics.growth_success_count, 1U);
    EXPECT_EQ(diagnostics.rollback_count, 1U);
    EXPECT_EQ(diagnostics.required_capacity_high_water, REQUIRED_CAPACITY);
}

TEST(RendererSmokeTest, ArenaRecreationExceptionAfterHookKeepsItsStageAndRollbackFailurePropagates)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t REQUIRED_CAPACITY = 9U;
    static constexpr uint32_t MAXIMUM_CAPACITY = 16U;
    static constexpr auto FRAME_DEADLINE = std::chrono::steady_clock::time_point{
        std::chrono::steady_clock::duration{ 123'456 }
    };

    uint32_t capacity = INITIAL_CAPACITY;
    client::StoneFaceGrowthDiagnostics diagnostics;
    auto const recreate = [&](std::chrono::steady_clock::time_point const deadline) {
        EXPECT_EQ(deadline, FRAME_DEADLINE);
        if (capacity != INITIAL_CAPACITY) {
            ++diagnostics.before_recreate_count;
            throw std::runtime_error("simulated native creation failure");
        }
        throw std::invalid_argument("simulated rollback failure");
    };
    try {
        static_cast<void>(client::detail::tryGrowStoneFaceCapacity(
            capacity, REQUIRED_CAPACITY, MAXIMUM_CAPACITY, FRAME_DEADLINE, recreate, &diagnostics
        ));
        FAIL() << "rollback failure must propagate";
    } catch (std::invalid_argument const& error) {
        EXPECT_EQ(std::string{ error.what() }, "simulated rollback failure");
    }
    EXPECT_EQ(capacity, INITIAL_CAPACITY);
    EXPECT_EQ(diagnostics.growth_attempt_count, 1U);
    EXPECT_EQ(diagnostics.before_recreate_count, 1U);
    EXPECT_EQ(diagnostics.recreation_exception_count, 1U);
    EXPECT_EQ(diagnostics.recreation_exception_before_hook_count, 0U);
    EXPECT_EQ(diagnostics.deferred_recreation_count, 0U);
    EXPECT_EQ(diagnostics.growth_success_count, 0U);
    EXPECT_EQ(diagnostics.rollback_count, 1U);
    EXPECT_EQ(diagnostics.rollback_exception_count, 1U);
    EXPECT_GE(diagnostics.growth_work_duration, std::chrono::nanoseconds::zero());
}

TEST(RendererSmokeTest, FreshArenaDiagnosticsDoNotCountCapacitySatisfiedWithoutRecreationAsGrowth)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t REQUIRED_CAPACITY = 4U;
    static constexpr uint32_t MAXIMUM_CAPACITY = 16U;

    uint32_t capacity = INITIAL_CAPACITY;
    client::StoneFaceGrowthDiagnostics diagnostics;
    auto const recreate = [](std::chrono::steady_clock::time_point) {
        FAIL() << "an already sufficient arena must not be recreated";
    };
    EXPECT_TRUE(client::detail::tryGrowStoneFaceCapacity(
        capacity, REQUIRED_CAPACITY, MAXIMUM_CAPACITY, {}, recreate, &diagnostics
    ));
    EXPECT_EQ(capacity, INITIAL_CAPACITY);
    EXPECT_EQ(diagnostics.growth_attempt_count, 0U);
    EXPECT_EQ(diagnostics.growth_success_count, 0U);
    EXPECT_EQ(diagnostics.rollback_count, 0U);
    EXPECT_EQ(diagnostics.last_required_capacity, REQUIRED_CAPACITY);
    EXPECT_EQ(diagnostics.required_capacity_high_water, REQUIRED_CAPACITY);
    EXPECT_EQ(diagnostics.last_target_capacity, 0U);
    EXPECT_EQ(diagnostics.growth_work_duration, std::chrono::nanoseconds::zero());
}

TEST(RendererSmokeTest, ArenaNativeExceptionReasonsRetainFirstAndFollowOnlyChangedLastReason)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t REQUIRED_CAPACITY = 9U;
    static constexpr uint32_t MAXIMUM_CAPACITY = 16U;
    static constexpr std::array<std::string_view, 3U> REASONS{ "submitted fence", "submitted fence", "acquire fence" };

    uint32_t capacity = INITIAL_CAPACITY;
    client::StoneFaceGrowthDiagnostics diagnostics;
    for (std::string_view const reason : REASONS) {
        auto const recreate = [&](std::chrono::steady_clock::time_point) {
            if (capacity != INITIAL_CAPACITY) {
                throw std::runtime_error(std::string{ reason });
            }
        };
        EXPECT_FALSE(client::detail::tryGrowStoneFaceCapacity(
            capacity, REQUIRED_CAPACITY, MAXIMUM_CAPACITY, {}, recreate, &diagnostics
        ));
        EXPECT_EQ(capacity, INITIAL_CAPACITY);
        EXPECT_EQ(std::string_view{ diagnostics.first_recreation_exception_reason.data() }, REASONS.front());
        EXPECT_EQ(std::string_view{ diagnostics.last_recreation_exception_reason.data() }, reason);
    }
    EXPECT_EQ(diagnostics.recreation_exception_count, REASONS.size());
    EXPECT_EQ(diagnostics.recreation_unknown_exception_count, 0U);
    EXPECT_EQ(diagnostics.deferred_recreation_count, 0U);
    EXPECT_EQ(diagnostics.rollback_count, REASONS.size());
}

TEST(RendererSmokeTest, ArenaNativeExceptionReasonsTruncateAndTerminateAfterShorterReplacements)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t REQUIRED_CAPACITY = 9U;
    static constexpr uint32_t MAXIMUM_CAPACITY = 16U;
    static constexpr uint32_t MAXIMUM_REASON_BYTES = 256U;

    uint32_t capacity = INITIAL_CAPACITY;
    client::StoneFaceGrowthDiagnostics diagnostics;
    std::string reason(MAXIMUM_REASON_BYTES + 64U, 'x');
    auto const recreate = [&](std::chrono::steady_clock::time_point) {
        if (capacity != INITIAL_CAPACITY) {
            throw std::runtime_error(reason);
        }
    };
    EXPECT_FALSE(client::detail::tryGrowStoneFaceCapacity(
        capacity, REQUIRED_CAPACITY, MAXIMUM_CAPACITY, {}, recreate, &diagnostics
    ));
    EXPECT_EQ(capacity, INITIAL_CAPACITY);
    EXPECT_EQ(
        std::string_view{ diagnostics.first_recreation_exception_reason.data() },
        reason.substr(0U, MAXIMUM_REASON_BYTES)
    );
    EXPECT_EQ(
        std::string_view{ diagnostics.last_recreation_exception_reason.data() },
        reason.substr(0U, MAXIMUM_REASON_BYTES)
    );
    EXPECT_EQ(diagnostics.first_recreation_exception_reason.back(), '\0');
    EXPECT_EQ(diagnostics.last_recreation_exception_reason.back(), '\0');
    EXPECT_EQ(diagnostics.recreation_exception_count, 1U);

    reason = "x";
    EXPECT_FALSE(client::detail::tryGrowStoneFaceCapacity(
        capacity, REQUIRED_CAPACITY, MAXIMUM_CAPACITY, {}, recreate, &diagnostics
    ));
    EXPECT_EQ(std::string_view{ diagnostics.last_recreation_exception_reason.data() }, reason);
    EXPECT_EQ(diagnostics.last_recreation_exception_reason[1U], '\0');

    reason.clear();
    EXPECT_FALSE(client::detail::tryGrowStoneFaceCapacity(
        capacity, REQUIRED_CAPACITY, MAXIMUM_CAPACITY, {}, recreate, &diagnostics
    ));
    EXPECT_EQ(std::string_view{ diagnostics.last_recreation_exception_reason.data() }, reason);
    EXPECT_EQ(diagnostics.last_recreation_exception_reason.front(), '\0');
    EXPECT_EQ(std::string_view{ diagnostics.first_recreation_exception_reason.data() },
        std::string(MAXIMUM_REASON_BYTES, 'x'));
    EXPECT_EQ(diagnostics.recreation_exception_count, 3U);
}

TEST(RendererSmokeTest, UnknownArenaExceptionsStayCountedAndDoNotReplaceKnownEmptyFirstReason)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t REQUIRED_CAPACITY = 9U;
    static constexpr uint32_t MAXIMUM_CAPACITY = 16U;
    static constexpr std::array<std::string_view, 2U> KNOWN_REASONS{ "", "later fence" };

    uint32_t capacity = INITIAL_CAPACITY;
    client::StoneFaceGrowthDiagnostics diagnostics;
    auto const unknown_recreate = [&](std::chrono::steady_clock::time_point) {
        if (capacity != INITIAL_CAPACITY) {
            throw uint32_t{ 1U };
        }
    };
    EXPECT_FALSE(client::detail::tryGrowStoneFaceCapacity(
        capacity, REQUIRED_CAPACITY, MAXIMUM_CAPACITY, {}, unknown_recreate, &diagnostics
    ));
    EXPECT_EQ(diagnostics.recreation_exception_count, 1U);
    EXPECT_EQ(diagnostics.recreation_unknown_exception_count, 1U);
    for (std::string_view const reason : KNOWN_REASONS) {
        auto const recreate = [&](std::chrono::steady_clock::time_point) {
            if (capacity != INITIAL_CAPACITY) {
                throw std::runtime_error(std::string{ reason });
            }
        };
        EXPECT_FALSE(client::detail::tryGrowStoneFaceCapacity(
            capacity, REQUIRED_CAPACITY, MAXIMUM_CAPACITY, {}, recreate, &diagnostics
        ));
    }
    EXPECT_FALSE(client::detail::tryGrowStoneFaceCapacity(
        capacity, REQUIRED_CAPACITY, MAXIMUM_CAPACITY, {}, unknown_recreate, &diagnostics
    ));
    EXPECT_EQ(capacity, INITIAL_CAPACITY);
    EXPECT_EQ(diagnostics.recreation_exception_count, 4U);
    EXPECT_EQ(diagnostics.recreation_unknown_exception_count, 2U);
    EXPECT_EQ(diagnostics.recreation_exception_before_hook_count, 4U);
    EXPECT_EQ(std::string_view{ diagnostics.first_recreation_exception_reason.data() }, "");
    EXPECT_EQ(std::string_view{ diagnostics.last_recreation_exception_reason.data() }, KNOWN_REASONS.back());
    EXPECT_EQ(diagnostics.rollback_count, 4U);
}

TEST(RendererSmokeTest, PendingArenaDrainGuardsAcquireAndSubmitUntilNativeRecreationCompletesWithoutNewUploads)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t TARGET_CAPACITY = 16U;
    static constexpr uint32_t LARGER_TARGET_CAPACITY = 32U;
    static constexpr auto FRAME_BUDGET = std::chrono::milliseconds{ 8 };
    static constexpr auto STARTED_AT = std::chrono::steady_clock::time_point{};
    static constexpr auto FRAME_DEADLINE = STARTED_AT + FRAME_BUDGET;

    PendingGrowthPresentationFixture fixture;
    client::detail::PendingStoneFaceGrowth pending;
    uint32_t actual_capacity = INITIAL_CAPACITY;
    fixture.backend->recreation_wait_result = core::graphics::vulkan::FenceWaitResult::TimedOut;
    auto const grow = [&](uint32_t const target, std::chrono::steady_clock::time_point const deadline) {
        EXPECT_EQ(deadline, FRAME_DEADLINE);
        try {
            fixture.context->recreate(fixture.context->info().extent, {}, FRAME_BUDGET);
            actual_capacity = target;
        } catch (...) {
            EXPECT_EQ(pending.recordFailure(std::current_exception(), target, false, STARTED_AT),
                client::detail::StoneFaceRecreationFailure::DrainPending);
        }
        return actual_capacity;
    };
    EXPECT_EQ(grow(TARGET_CAPACITY, FRAME_DEADLINE), INITIAL_CAPACITY);
    pending.coalesce(LARGER_TARGET_CAPACITY);
    pending.coalesce(TARGET_CAPACITY);
    EXPECT_EQ(pending.targetCapacity(), LARGER_TARGET_CAPACITY);

    EXPECT_FALSE(pending.render(actual_capacity, FRAME_DEADLINE, grow, STARTED_AT, [&] { return fixture.present(); }));
    EXPECT_EQ(fixture.backend->acquire_count, 0U);
    EXPECT_EQ(fixture.backend->submit_count, 0U);
    EXPECT_EQ(actual_capacity, INITIAL_CAPACITY);

    fixture.backend->recreation_wait_result = core::graphics::vulkan::FenceWaitResult::Completed;
    EXPECT_TRUE(pending.render(actual_capacity, FRAME_DEADLINE, grow, STARTED_AT, [&] { return fixture.present(); }));
    EXPECT_EQ(actual_capacity, LARGER_TARGET_CAPACITY);
    EXPECT_EQ(pending.targetCapacity(), 0U);
    EXPECT_EQ(fixture.backend->acquire_count, 1U);
    EXPECT_EQ(fixture.backend->submit_count, 1U);
    EXPECT_EQ(fixture.backend->recreation_wait_budgets, (std::vector<std::chrono::nanoseconds>(3U, FRAME_BUDGET)));
}

TEST(RendererSmokeTest, PendingArenaDrainRequiresExactCoreFenceErrorAndOldResourcesBeforeTheHook)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t TARGET_CAPACITY = 16U;
    static constexpr auto STARTED_AT = std::chrono::steady_clock::time_point{};
    static constexpr std::string_view RECREATION_TIMEOUT_REASON = "Vulkan presentation recreation fence wait timed out";
    static constexpr std::string_view PRESENT_TIMEOUT_REASON = "Vulkan presentation present fence wait timed out";

    std::array<std::exception_ptr, 2U> const timeouts{
        std::make_exception_ptr(core::graphics::vulkan::VulkanError{ std::string{ RECREATION_TIMEOUT_REASON } }),
        std::make_exception_ptr(core::graphics::vulkan::VulkanError{ std::string{ PRESENT_TIMEOUT_REASON } }),
    };
    std::array<std::exception_ptr, 4U> const nonmatching_failures{
        std::make_exception_ptr(std::runtime_error{ std::string{ PRESENT_TIMEOUT_REASON } }),
        std::make_exception_ptr(core::graphics::vulkan::VulkanError{
            "Vulkan presentation acquire fence wait timed out"
        }),
        std::make_exception_ptr(core::graphics::vulkan::VulkanError{
            "Vulkan presentation present fence wait timed out with extra detail"
        }),
        std::make_exception_ptr(uint32_t{ 1U }),
    };
    for (std::exception_ptr const& timeout : timeouts) {
        client::detail::PendingStoneFaceGrowth pending;
        EXPECT_EQ(pending.recordFailure(timeout, TARGET_CAPACITY, false, STARTED_AT),
            client::detail::StoneFaceRecreationFailure::DrainPending);
        EXPECT_EQ(pending.recordFailure(timeout, TARGET_CAPACITY, true, STARTED_AT),
            client::detail::StoneFaceRecreationFailure::Failed);
        EXPECT_EQ(pending.targetCapacity(), 0U);
    }
    for (std::exception_ptr const& failure : nonmatching_failures) {
        PendingGrowthPresentationFixture fixture;
        client::detail::PendingStoneFaceGrowth pending;
        EXPECT_EQ(pending.recordFailure(timeouts.front(), TARGET_CAPACITY, false, STARTED_AT),
            client::detail::StoneFaceRecreationFailure::DrainPending);
        EXPECT_EQ(pending.recordFailure(failure, TARGET_CAPACITY, false, STARTED_AT),
            client::detail::StoneFaceRecreationFailure::Failed);
        EXPECT_TRUE(pending.render(INITIAL_CAPACITY, STARTED_AT,
            [](uint32_t, std::chrono::steady_clock::time_point) {
                ADD_FAILURE() << "failure must clear latch";
                return 0U;
            },
            STARTED_AT, [&] { return fixture.present(); }));
        EXPECT_EQ(fixture.backend->acquire_count, 1U);
        EXPECT_EQ(fixture.backend->submit_count, 1U);
    }
}

TEST(RendererSmokeTest, FrameBoundedPresentationRecreationDefersOnlyDrainTimeouts)
{
    static constexpr auto FRAME_DEADLINE = std::chrono::steady_clock::time_point{
        std::chrono::steady_clock::duration{ 123'456 }
    };
    static constexpr std::array<std::string_view, 2U> RETRYABLE_REASONS{
        "Vulkan presentation recreation fence wait timed out",
        "Vulkan presentation present fence wait timed out",
    };

    for (std::string_view const reason : RETRYABLE_REASONS) {
        std::chrono::steady_clock::time_point observed_deadline{};
        auto const recreate = [&](std::chrono::steady_clock::time_point const deadline) {
            observed_deadline = deadline;
            throw core::graphics::vulkan::VulkanError{ std::string{ reason } };
        };
        EXPECT_FALSE(client::detail::tryRecreatePresentationWithinDeadline(FRAME_DEADLINE, recreate));
        EXPECT_EQ(observed_deadline, FRAME_DEADLINE);
    }

    auto const failed_recreation = [](std::chrono::steady_clock::time_point const) {
        throw core::graphics::vulkan::VulkanError{ "Vulkan presentation recreation lost the device" };
    };
    EXPECT_THROW(
        static_cast<void>(client::detail::tryRecreatePresentationWithinDeadline(FRAME_DEADLINE, failed_recreation)),
        core::graphics::vulkan::VulkanError
    );
}

TEST(RendererSmokeTest, NormalArenaDeferralRetainsTargetUntilTheNextFrame)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t TARGET_CAPACITY = 16U;
    static constexpr auto STARTED_AT = std::chrono::steady_clock::time_point{};

    PendingGrowthPresentationFixture fixture;
    client::detail::PendingStoneFaceGrowth pending;
    EXPECT_EQ(pending.recordFailure(std::make_exception_ptr(core::graphics::vulkan::VulkanError{
        "Vulkan presentation present fence wait timed out" }), TARGET_CAPACITY, false, STARTED_AT),
        client::detail::StoneFaceRecreationFailure::DrainPending);
    EXPECT_FALSE(pending.render(INITIAL_CAPACITY, STARTED_AT,
        [&](uint32_t, std::chrono::steady_clock::time_point) {
            EXPECT_EQ(pending.recordFailure(std::make_exception_ptr(client::detail::StoneFaceRecreationDeferred{}),
                TARGET_CAPACITY, false, STARTED_AT), client::detail::StoneFaceRecreationFailure::Deferred);
            return INITIAL_CAPACITY;
        }, STARTED_AT, [&] { return fixture.present(); }));
    EXPECT_EQ(pending.targetCapacity(), TARGET_CAPACITY);
    EXPECT_EQ(fixture.backend->acquire_count, 0U);
    EXPECT_EQ(fixture.backend->submit_count, 0U);
    EXPECT_TRUE(pending.render(INITIAL_CAPACITY, STARTED_AT + std::chrono::milliseconds{ 8 },
        [](uint32_t const target, std::chrono::steady_clock::time_point const deadline) {
            EXPECT_EQ(target, TARGET_CAPACITY);
            EXPECT_EQ(deadline, STARTED_AT + std::chrono::milliseconds{ 8 });
            return target;
        }, STARTED_AT, [&] { return fixture.present(); }));
    EXPECT_EQ(pending.targetCapacity(), 0U);
}

TEST(RendererSmokeTest, ExhaustedUploadDeadlineRetainsGrowthForAFreshFrameBudget)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t TARGET_CAPACITY = 16U;
    static constexpr auto FRAME_BUDGET = std::chrono::milliseconds{ 8 };
    static constexpr auto STARTED_AT = std::chrono::steady_clock::time_point{};

    PendingGrowthPresentationFixture fixture;
    client::detail::PendingStoneFaceGrowth pending;
    pending.coalesce(TARGET_CAPACITY, STARTED_AT);
    EXPECT_EQ(pending.targetCapacity(), TARGET_CAPACITY);
    auto const next_frame = STARTED_AT + FRAME_BUDGET;
    bool grew = false;
    EXPECT_TRUE(pending.render(INITIAL_CAPACITY, next_frame + FRAME_BUDGET,
        [&](uint32_t const target, std::chrono::steady_clock::time_point const deadline) {
            EXPECT_EQ(target, TARGET_CAPACITY);
            EXPECT_EQ(deadline, next_frame + FRAME_BUDGET);
            grew = true;
            return target;
        }, next_frame, [&] { return fixture.present(); }));
    EXPECT_TRUE(grew);
    EXPECT_EQ(pending.targetCapacity(), 0U);
}

TEST(RendererSmokeTest, PendingArenaRollbackFailurePropagatesBeforeAnyNativeAcquire)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t TARGET_CAPACITY = 16U;
    static constexpr auto STARTED_AT = std::chrono::steady_clock::time_point{};

    PendingGrowthPresentationFixture fixture;
    client::detail::PendingStoneFaceGrowth pending;
    EXPECT_EQ(pending.recordFailure(std::make_exception_ptr(core::graphics::vulkan::VulkanError{
        "Vulkan presentation recreation fence wait timed out" }), TARGET_CAPACITY, false, STARTED_AT),
        client::detail::StoneFaceRecreationFailure::DrainPending);
    try {
        static_cast<void>(pending.render(INITIAL_CAPACITY, STARTED_AT,
            [](uint32_t, std::chrono::steady_clock::time_point) -> uint32_t {
                throw std::invalid_argument("rollback failed");
            }, STARTED_AT, [&] { return fixture.present(); }));
        FAIL() << "rollback failure must propagate";
    } catch (std::invalid_argument const& error) {
        EXPECT_EQ(std::string_view{ error.what() }, "rollback failed");
    }
    EXPECT_EQ(fixture.backend->acquire_count, 0U);
    EXPECT_EQ(fixture.backend->submit_count, 0U);
}

TEST(RendererSmokeTest, PendingArenaAgeIsFiniteAndNotRenewedByCoalescingOrTimeoutRetries)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t TARGET_CAPACITY = 16U;
    static constexpr uint32_t LARGER_TARGET_CAPACITY = 32U;
    static constexpr auto STARTED_AT = std::chrono::steady_clock::time_point{};
    static constexpr auto FRAME_DEADLINE = STARTED_AT + std::chrono::milliseconds{ 8 };
    static constexpr auto EXPIRES_AT = STARTED_AT + std::chrono::seconds{ 5 };

    PendingGrowthPresentationFixture fixture;
    client::detail::PendingStoneFaceGrowth pending;
    std::exception_ptr const recreation_timeout = std::make_exception_ptr(core::graphics::vulkan::VulkanError{
        "Vulkan presentation recreation fence wait timed out" });
    std::exception_ptr const present_timeout = std::make_exception_ptr(core::graphics::vulkan::VulkanError{
        "Vulkan presentation present fence wait timed out" });
    EXPECT_EQ(pending.recordFailure(recreation_timeout, TARGET_CAPACITY, false, STARTED_AT),
        client::detail::StoneFaceRecreationFailure::DrainPending);
    pending.coalesce(LARGER_TARGET_CAPACITY);
    EXPECT_FALSE(pending.render(INITIAL_CAPACITY, FRAME_DEADLINE,
        [&](uint32_t const target, std::chrono::steady_clock::time_point const deadline) {
            EXPECT_EQ(deadline, FRAME_DEADLINE);
            EXPECT_EQ(pending.recordFailure(present_timeout, target, false, EXPIRES_AT - std::chrono::nanoseconds{ 1 }),
                client::detail::StoneFaceRecreationFailure::DrainPending);
            return INITIAL_CAPACITY;
        }, EXPIRES_AT - std::chrono::nanoseconds{ 1 }, [&] { return fixture.present(); }));
    try {
        static_cast<void>(pending.render(INITIAL_CAPACITY, FRAME_DEADLINE,
            [](uint32_t, std::chrono::steady_clock::time_point) {
                ADD_FAILURE() << "expired drain must not retry";
                return 0U;
            },
            EXPIRES_AT, [&] { return fixture.present(); }));
        FAIL() << "pending drain must surface its finite horizon";
    } catch (std::runtime_error const& error) {
        EXPECT_EQ(std::string_view{ error.what() },
            "stone face arena growth exceeded its pending drain horizon: actual=8, target=32, "
            "reason=Vulkan presentation present fence wait timed out");
    }
    EXPECT_EQ(fixture.backend->acquire_count, 0U);
    EXPECT_EQ(fixture.backend->submit_count, 0U);
}

TEST(RendererSmokeTest, ResetOrAlreadyInstalledActualCapacityCancelsPendingArenaGrowth)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t TARGET_CAPACITY = 16U;
    static constexpr auto STARTED_AT = std::chrono::steady_clock::time_point{};

    PendingGrowthPresentationFixture fixture;
    client::detail::PendingStoneFaceGrowth pending;
    std::exception_ptr const timeout = std::make_exception_ptr(core::graphics::vulkan::VulkanError{
        "Vulkan presentation recreation fence wait timed out" });
    auto const unexpected_growth = [](uint32_t, std::chrono::steady_clock::time_point) {
        ADD_FAILURE() << "cancelled or installed growth must not retry";
        return 0U;
    };
    EXPECT_EQ(pending.recordFailure(timeout, TARGET_CAPACITY, false, STARTED_AT),
        client::detail::StoneFaceRecreationFailure::DrainPending);
    pending.clear();
    EXPECT_TRUE(pending.render(INITIAL_CAPACITY, STARTED_AT, unexpected_growth, STARTED_AT,
        [&] { return fixture.present(); }));
    EXPECT_EQ(pending.recordFailure(timeout, TARGET_CAPACITY, false, STARTED_AT),
        client::detail::StoneFaceRecreationFailure::DrainPending);
    EXPECT_TRUE(pending.render(TARGET_CAPACITY, STARTED_AT, unexpected_growth, STARTED_AT,
        [&] { return fixture.present(); }));
    EXPECT_EQ(pending.targetCapacity(), 0U);
    EXPECT_EQ(fixture.backend->acquire_count, 2U);
    EXPECT_EQ(fixture.backend->submit_count, 2U);
}

TEST(RendererSmokeTest, SmallerExternalArenaCapacityDoesNotClearTheLargerPendingTarget)
{
    static constexpr uint32_t SMALLER_CAPACITY = 16U;
    static constexpr uint32_t TARGET_CAPACITY = 32U;
    static constexpr auto STARTED_AT = std::chrono::steady_clock::time_point{};

    PendingGrowthPresentationFixture fixture;
    client::detail::PendingStoneFaceGrowth pending;
    EXPECT_EQ(pending.recordFailure(std::make_exception_ptr(core::graphics::vulkan::VulkanError{
        "Vulkan presentation recreation fence wait timed out" }), TARGET_CAPACITY, false, STARTED_AT),
        client::detail::StoneFaceRecreationFailure::DrainPending);
    EXPECT_FALSE(pending.render(SMALLER_CAPACITY, STARTED_AT,
        [](uint32_t const target, std::chrono::steady_clock::time_point) {
            EXPECT_EQ(target, TARGET_CAPACITY);
            return SMALLER_CAPACITY;
        }, STARTED_AT, [&] { return fixture.present(); }));
    EXPECT_EQ(pending.targetCapacity(), TARGET_CAPACITY);
    EXPECT_EQ(fixture.backend->acquire_count, 0U);
    EXPECT_EQ(fixture.backend->submit_count, 0U);
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
