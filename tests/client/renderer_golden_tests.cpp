#include <client/render/InstalledShaderAssets.hpp>
#include <client/render/VulkanRenderer.hpp>

#include <core/graphics/vulkan/Vulkan.hpp>

#include <gtest/gtest.h>

#include <testsupport/ImageComparison.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <span>
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

[[nodiscard]] bool isRed(std::span<uint8_t const> const pixels, size_t const offset)
{
    return pixels[offset] > 200U && pixels[offset + 1U] < 80U && pixels[offset + 2U] < 80U;
}

[[nodiscard]] bool isGreen(std::span<uint8_t const> const pixels, size_t const offset)
{
    return pixels[offset] < 80U && pixels[offset + 1U] > 200U && pixels[offset + 2U] < 80U;
}

[[nodiscard]] bool isSky(std::span<uint8_t const> const pixels, size_t const offset)
{
    return pixels[offset + 2U] > pixels[offset + 1U] + 20U
        && pixels[offset + 1U] > pixels[offset] + 20U;
}

[[nodiscard]] bool isPlatform(std::span<uint8_t const> const pixels, size_t const offset)
{
    return pixels[offset] < 100U
        && pixels[offset + 2U] < 170U
        && pixels[offset + 2U] > pixels[offset + 1U] + 8U
        && pixels[offset + 1U] > pixels[offset] + 8U;
}

TEST(RendererGoldenTest, ThirdPersonPlatformV3MatchesApprovedReferenceWithoutWindow)
{
    static constexpr uint32_t WIDTH = 640U;
    static constexpr uint32_t HEIGHT = 480U;
    static constexpr uint8_t CHANNEL_TOLERANCE = 2U;
    static constexpr uint64_t ALLOWED_MISMATCHED_PIXELS = 0U;
    static constexpr auto TIMEOUT = std::chrono::seconds{ 10 };
    static constexpr std::array<client::PlayerRenderData, 2> S4_PLAYERS{
        client::PlayerRenderData{ .x = 2U, .y = 3U, .color = { 1.0F, 0.0F, 0.0F, 1.0F } },
        client::PlayerRenderData{ .x = 29U, .y = 28U, .color = { 0.0F, 1.0F, 0.0F, 1.0F } },
    };
    try {
        client::InstalledShaderAssets const shader_assets;
        client::VulkanOffscreenRenderer renderer{ shader_assets, true };
        ASSERT_TRUE(renderer.validationEnabled());
        client::RendererFrameCapture const capture = renderer.render(
            S4_PLAYERS,
            std::chrono::steady_clock::now() + TIMEOUT
        );
        EXPECT_EQ(capture.width, WIDTH);
        EXPECT_EQ(capture.height, HEIGHT);
        EXPECT_FALSE(capture.srgb_encoded);
        EXPECT_EQ(capture.rgba8.size(), static_cast<size_t>(WIDTH) * HEIGHT * 4U);
        EXPECT_EQ(renderer.validationErrorCount(), 0U);

        bool saw_sky = false;
        bool saw_platform = false;
        bool saw_platform_side = false;
        bool saw_red_player = false;
        bool saw_green_player = false;
        for (size_t offset = 0U; offset < capture.rgba8.size(); offset += 4U) {
            saw_sky = saw_sky || isSky(capture.rgba8, offset);
            saw_platform = saw_platform || isPlatform(capture.rgba8, offset);
            saw_platform_side = saw_platform_side || (
                offset >= static_cast<size_t>(WIDTH) * 4U
                && isPlatform(capture.rgba8, offset)
                && isPlatform(capture.rgba8, offset - static_cast<size_t>(WIDTH) * 4U)
            );
            saw_red_player = saw_red_player || isRed(capture.rgba8, offset);
            saw_green_player = saw_green_player || isGreen(capture.rgba8, offset);
        }
        EXPECT_TRUE(saw_sky) << "third-person scene lacks sky-like clear color";
        EXPECT_TRUE(saw_platform) << "third-person scene lacks solid platform side color";
        EXPECT_TRUE(saw_platform_side) << "third-person scene lacks visible platform thickness";
        EXPECT_TRUE(saw_red_player) << "third-person scene lacks local player cube";
        EXPECT_TRUE(saw_green_player) << "third-person scene lacks remote player cube";

        testsupport::Rgba8Image const actual = toImage(capture);
        std::expected<testsupport::Rgba8Image, std::string> const expected = testsupport::readPpm(
            MC_RENDERER_GOLDEN_REFERENCE
        );
        if (!expected.has_value()) {
            std::expected<std::monostate, std::string> const actual_output = testsupport::writePpm(
                actual,
                std::filesystem::path{ MC_RENDERER_GOLDEN_DIAGNOSTIC_DIRECTORY } / "s4_scene-actual.ppm",
                "MC-AI-0003 true offscreen actual; manually inspect before replacing the approved reference"
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
        EXPECT_TRUE(result.matches)
            << result.diagnostic << "; diagnostics: " << MC_RENDERER_GOLDEN_DIAGNOSTIC_DIRECTORY;
    } catch (core::graphics::vulkan::VulkanError const& error) {
#if MC_RENDERER_GOLDEN_FAIL_UNSUPPORTED
        FAIL() << "true offscreen renderer is unavailable: " << error.what();
#else
        GTEST_SKIP() << "true offscreen renderer is unavailable: " << error.what();
#endif
    } catch (std::exception const& error) {
        FAIL() << "true offscreen renderer failed: " << error.what();
    }
}

TEST(RendererGoldenTest, FixedExtentAndDepthOrderingDoNotRequirePresentation)
{
    static constexpr uint32_t WIDTH = 640U;
    static constexpr uint32_t HEIGHT = 480U;
    static constexpr std::array FAR_ONLY{
        client::PlayerRenderData{ .x = 16U, .y = 13U, .color = { 0.0F, 1.0F, 0.0F, 1.0F } },
    };
    static constexpr std::array NEAR_THEN_FAR{
        client::PlayerRenderData{ .x = 16U, .y = 12U, .color = { 1.0F, 0.0F, 0.0F, 1.0F } },
        FAR_ONLY[0],
    };
    try {
        client::InstalledShaderAssets const shader_assets;
        client::VulkanOffscreenRenderer renderer{ shader_assets, true };
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 10 };
        client::RendererFrameCapture const far = renderer.render(FAR_ONLY, deadline);
        client::RendererFrameCapture const combined = renderer.render(NEAR_THEN_FAR, deadline);
        EXPECT_EQ(far.width, WIDTH);
        EXPECT_EQ(far.height, HEIGHT);
        EXPECT_EQ(combined.width, WIDTH);
        EXPECT_EQ(combined.height, HEIGHT);
        ASSERT_EQ(far.rgba8.size(), combined.rgba8.size());

        bool saw_occluded_far_pixel = false;
        for (size_t offset = 0U; offset < far.rgba8.size(); offset += 4U) {
            saw_occluded_far_pixel = saw_occluded_far_pixel
                || (isGreen(far.rgba8, offset) && isRed(combined.rgba8, offset));
        }
        EXPECT_TRUE(saw_occluded_far_pixel)
            << "a nearer red box did not occlude a later-drawn far green box";
        EXPECT_EQ(renderer.validationErrorCount(), 0U);
    } catch (core::graphics::vulkan::VulkanError const& error) {
#if MC_RENDERER_GOLDEN_FAIL_UNSUPPORTED
        FAIL() << "true offscreen renderer is unavailable: " << error.what();
#else
        GTEST_SKIP() << "true offscreen renderer is unavailable: " << error.what();
#endif
    } catch (std::exception const& error) {
        FAIL() << "true offscreen renderer failed: " << error.what();
    }
}

} // namespace
