#include <client/Camera.hpp>
#include <client/PlayerPresentation.hpp>
#include <client/render/InstalledShaderAssets.hpp>
#include <client/render/VulkanRenderer.hpp>

#include <core/platform/glfw/GlfwWindow.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace {

struct CaptureColorClasses final {
    bool has_grid = false;
    bool has_red_player = false;
    bool has_green_player = false;
    bool has_debug_hud = false;

    [[nodiscard]] bool complete() const
    {
        return has_grid && has_red_player && has_green_player && has_debug_hud;
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
        appendMissing(has_red_player, "red-player");
        appendMissing(has_green_player, "green-player");
        appendMissing(has_debug_hud, "debug-hud");
        return missing;
    }
};

[[nodiscard]] CaptureColorClasses classifyCaptureColors(client::RendererFrameCapture const& capture)
{
    CaptureColorClasses classes;
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
        classes.has_red_player = classes.has_red_player || (
            red > 200U && green < 80U && blue < 80U
        );
        classes.has_green_player = classes.has_green_player || (
            red < 80U && green > 200U && blue < 80U
        );
        classes.has_debug_hud = classes.has_debug_hud || (
            x < 220U && y < 80U && red > 180U && green > 180U && blue > 180U
        );
    }
    return classes;
}

TEST(RendererSmokeTest, CompletedCaptureSurvivesRecreateAndReadsBack)
{
    static constexpr uint32_t INITIAL_LOGICAL_WIDTH = 320U;
    static constexpr uint32_t INITIAL_LOGICAL_HEIGHT = 240U;
    static constexpr int32_t RESIZED_WIDTH = 400;
    static constexpr int32_t RESIZED_HEIGHT = 300;
    static constexpr uint32_t MAX_CAPTURE_FRAMES = 12U;
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
    renderer.setDebugHudEnabled(true);
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
    ASSERT_EQ(original_capture->width, initial_framebuffer_width);
    ASSERT_EQ(original_capture->height, initial_framebuffer_height);
    ASSERT_EQ(
        original_capture->rgba8.size(),
        static_cast<uint64_t>(initial_framebuffer_width) * initial_framebuffer_height * 4U
    );
    CaptureColorClasses const original_colors = classifyCaptureColors(*original_capture);
    EXPECT_TRUE(original_colors.complete())
        << "original capture lacks: " << original_colors.missingClasses()
        << "; the 32x32 inset-cell grid may cover every physical framebuffer sample";
    EXPECT_EQ(renderer.runtimeInfo().debug_hud_draw_count, 1U);

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
    EXPECT_EQ(retained_capture->rgba8, original_capture->rgba8);
    CaptureColorClasses const retained_colors = classifyCaptureColors(*retained_capture);
    EXPECT_TRUE(retained_colors.complete())
        << "retained capture lacks: " << retained_colors.missingClasses()
        << "; the 32x32 inset-cell grid may cover every physical framebuffer sample";

    renderer.hotReload();
    renderer.requestFrameCapture();
    ASSERT_TRUE(completeCapture());
    std::optional<client::RendererFrameCapture> const reloaded_capture = renderer.takeFrameCapture();
    ASSERT_TRUE(reloaded_capture.has_value());
    CaptureColorClasses const reloaded_colors = classifyCaptureColors(*reloaded_capture);
    EXPECT_TRUE(reloaded_colors.complete())
        << "reloaded capture lacks: " << reloaded_colors.missingClasses()
        << "; the 32x32 inset-cell grid may cover every physical framebuffer sample";
    EXPECT_GE(rendered_frames, 3U);
    EXPECT_EQ(renderer.runtimeInfo().pipeline_path, client::RendererPipelinePath::Vertex);
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

TEST(RendererSmokeTest, ActiveVertexRendererKeepsUprightGridAndTopLeftHudForRemotePlayers)
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
        .x = 8U,
        .y = 15U,
        .ch = '#',
    };
    core::platform::glfw::GlfwWindow window{
        core::platform::glfw::WindowDescriptor{
            .width = LOGICAL_WIDTH,
            .height = LOGICAL_HEIGHT,
            .title = "MinecraftClone active first-person renderer",
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
    std::array<client::PlayerRenderData, 1U> const remote_players{
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
            .position = client::localPlayerEyePosition(LOCAL),
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
            remote_players,
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
    CaptureColorClasses const classes = classifyCaptureColors(capture);
    EXPECT_TRUE(classes.has_grid);
    EXPECT_TRUE(classes.has_debug_hud);
    EXPECT_TRUE(classes.has_green_player);
    EXPECT_FALSE(classes.has_red_player);
    EXPECT_EQ(renderer.runtimeInfo().pipeline_path, client::RendererPipelinePath::Vertex);
}

} // namespace
