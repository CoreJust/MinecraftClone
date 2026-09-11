#include <client/render/InstalledShaderAssets.hpp>
#include <client/render/VulkanRenderer.hpp>

#include <core/platform/glfw/GlfwWindow.hpp>

#include <gtest/gtest.h>

#include <testsupport/ImageComparison.hpp>

#include <array>
#include <chrono>
#include <exception>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>

namespace {

[[nodiscard]]
testsupport::Rgba8Image toImage(client::RendererFrameCapture const& capture)
{
    return {
        .width = capture.width,
        .height = capture.height,
        .srgb_encoded = capture.srgb_encoded,
        .pixels = capture.rgba8,
    };
}

TEST(RendererGoldenTest, S4SceneMatchesApprovedReference)
{
    static constexpr uint32_t LOGICAL_WIDTH = 320U;
    static constexpr uint32_t LOGICAL_HEIGHT = 240U;
    static constexpr uint32_t MAX_CAPTURE_FRAMES = 12U;
    static constexpr uint8_t CHANNEL_TOLERANCE = 2U;
    static constexpr uint64_t ALLOWED_MISMATCHED_PIXELS = 0U;
    static constexpr auto TIMEOUT = std::chrono::seconds{ 10 };
    static constexpr std::array<client::PlayerRenderData, 2> S4_PLAYERS{
        client::PlayerRenderData{ .x = 2U, .y = 3U, .color = { 1.0F, 0.0F, 0.0F, 1.0F } },
        client::PlayerRenderData{ .x = 29U, .y = 28U, .color = { 0.0F, 1.0F, 0.0F, 1.0F } },
    };
    std::optional<client::RendererFrameCapture> capture;
    try {
        core::platform::glfw::GlfwWindow window{
            core::platform::glfw::WindowDescriptor{
                .width = LOGICAL_WIDTH,
                .height = LOGICAL_HEIGHT,
                .title = "MinecraftClone S4 renderer golden",
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
        EXPECT_TRUE(renderer.runtimeInfo().validation_enabled);
        renderer.setDebugHudEnabled(false);
        renderer.requestFrameCapture();
        std::chrono::steady_clock::time_point const deadline = std::chrono::steady_clock::now() + TIMEOUT;
        for (uint32_t frame = 0U;
             frame < MAX_CAPTURE_FRAMES && std::chrono::steady_clock::now() < deadline;
             ++frame) {
            if (!window.nextFrame()) {
                FAIL() << "GLFW window closed before the S4 frame capture completed";
            }
            ASSERT_TRUE(renderer.render(S4_PLAYERS, deadline))
                << "production renderer did not submit the S4 capture frame";
            if (renderer.captureState() == client::FrameCaptureState::Completed) {
                capture = renderer.takeFrameCapture();
                break;
            }
            if (renderer.captureState() == client::FrameCaptureState::UnsupportedSwapchainUsage
                || renderer.captureState() == client::FrameCaptureState::UnsupportedFormat) {
#if MC_RENDERER_GOLDEN_FAIL_UNSUPPORTED
                FAIL() << "renderer golden capture is unsupported by this runtime";
#else
                GTEST_SKIP() << "renderer golden capture is unsupported by this runtime";
#endif
            }
        }
        ASSERT_TRUE(capture.has_value()) << "renderer golden capture did not complete before its deadline";
        EXPECT_EQ(renderer.runtimeInfo().debug_hud_draw_count, 0U);
    } catch (core::platform::glfw::GlfwError const& error) {
#if MC_RENDERER_GOLDEN_FAIL_UNSUPPORTED
        FAIL() << "renderer golden GLFW setup failed: " << error.what();
#else
        GTEST_SKIP() << "renderer golden GLFW setup is unavailable: " << error.what();
#endif
    } catch (std::exception const& error) {
        FAIL() << "renderer golden initialization or render failed: " << error.what();
    }

    testsupport::Rgba8Image const actual = toImage(*capture);
    std::expected<testsupport::Rgba8Image, std::string> const expected = testsupport::readPpm(
        MC_RENDERER_GOLDEN_REFERENCE
    );
    if (!expected.has_value()) {
        std::expected<std::monostate, std::string> const actual_output = testsupport::writePpm(
            actual,
            std::filesystem::path{ MC_RENDERER_GOLDEN_DIAGNOSTIC_DIRECTORY } / "s4_scene-actual.ppm",
            "MC-AI-0052 actual capture; manually inspect before replacing the approved reference"
        );
        ASSERT_TRUE(actual_output.has_value()) << actual_output.error();
        FAIL() << expected.error() << "; wrote a non-approved actual capture to "
               << MC_RENDERER_GOLDEN_DIAGNOSTIC_DIRECTORY;
    }
    testsupport::ImageComparisonPolicy const comparison_policy{
        .channel_tolerance = CHANNEL_TOLERANCE,
        .max_mismatched_pixels = ALLOWED_MISMATCHED_PIXELS,
    };
    testsupport::ImageComparisonResult const result = testsupport::compareRgba8(
        *expected,
        actual,
        comparison_policy
    );
    if (!result.matches) {
        std::expected<std::monostate, std::string> const artifacts = testsupport::writeMismatchArtifacts(
            *expected,
            actual,
            result,
            comparison_policy,
            MC_RENDERER_GOLDEN_DIAGNOSTIC_DIRECTORY,
            "s4_scene"
        );
        ASSERT_TRUE(artifacts.has_value()) << artifacts.error();
    }
    std::string const diagnostic = result.diagnostic + "; diagnostics: "
        + MC_RENDERER_GOLDEN_DIAGNOSTIC_DIRECTORY;
    EXPECT_TRUE(result.matches) << diagnostic;
}

} // namespace
