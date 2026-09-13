#include <gtest/gtest.h>

#include <testsupport/ImageComparison.hpp>

#include <expected>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void writePpmFixture(
    std::filesystem::path const& file_path,
    std::string_view const header,
    std::vector<uint8_t> const& payload
)
{
    std::filesystem::create_directories(file_path.parent_path());
    std::ofstream output{ file_path, std::ios::binary | std::ios::trunc };
    if (!output) {
        throw std::runtime_error("cannot create PPM test fixture");
    }
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
    output.write(reinterpret_cast<char const*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    if (!output) {
        throw std::runtime_error("cannot write PPM test fixture");
    }
}

TEST(ImageComparisonTest, ReportsDeliberatePixelMismatchWithinTheDeclaredPolicy)
{
    static constexpr uint32_t IMAGE_WIDTH = 2U;
    static constexpr uint32_t IMAGE_HEIGHT = 1U;
    static constexpr uint8_t CHANNEL_TOLERANCE = 2U;
    static constexpr uint64_t ALLOWED_MISMATCHES = 0U;
    testsupport::Rgba8Image const expected{
        .width = IMAGE_WIDTH,
        .height = IMAGE_HEIGHT,
        .srgb_encoded = true,
        .pixels = { 10U, 20U, 30U, 255U, 40U, 50U, 60U, 255U },
    };
    testsupport::Rgba8Image const actual{
        .width = IMAGE_WIDTH,
        .height = IMAGE_HEIGHT,
        .srgb_encoded = true,
        .pixels = { 11U, 20U, 30U, 255U, 40U, 56U, 60U, 255U },
    };

    testsupport::ImageComparisonResult const result = testsupport::compareRgba8(
        expected,
        actual,
        {
            .channel_tolerance = CHANNEL_TOLERANCE,
            .max_mismatched_pixels = ALLOWED_MISMATCHES,
        }
    );

    EXPECT_FALSE(result.matches);
    EXPECT_TRUE(result.dimensions_match);
    EXPECT_EQ(result.mismatched_pixels, 1U);
    EXPECT_EQ(result.maximum_channel_delta, 6U);
    EXPECT_EQ(result.first_mismatch_x, 1U);
    EXPECT_EQ(result.first_mismatch_y, 0U);
    EXPECT_NE(result.diagnostic.find("mismatched pixels=1"), std::string::npos);
}

TEST(ImageComparisonTest, AllowsOnlyTheConfiguredNumberOfMismatchedPixels)
{
    static constexpr uint32_t IMAGE_WIDTH = 2U;
    static constexpr uint32_t IMAGE_HEIGHT = 1U;
    static constexpr uint8_t CHANNEL_TOLERANCE = 0U;
    static constexpr uint64_t ALLOWED_MISMATCHES = 1U;
    testsupport::Rgba8Image const expected{
        .width = IMAGE_WIDTH,
        .height = IMAGE_HEIGHT,
        .srgb_encoded = true,
        .pixels = { 1U, 2U, 3U, 255U, 4U, 5U, 6U, 255U },
    };
    testsupport::Rgba8Image const actual{
        .width = IMAGE_WIDTH,
        .height = IMAGE_HEIGHT,
        .srgb_encoded = true,
        .pixels = { 1U, 2U, 3U, 255U, 4U, 5U, 7U, 255U },
    };

    testsupport::ImageComparisonResult const result = testsupport::compareRgba8(
        expected,
        actual,
        {
            .channel_tolerance = CHANNEL_TOLERANCE,
            .max_mismatched_pixels = ALLOWED_MISMATCHES,
        }
    );

    EXPECT_TRUE(result.matches);
    EXPECT_EQ(result.mismatched_pixels, 1U);
}

TEST(ImageComparisonTest, WritesDiffOnlyForPixelsOutsideTheDeclaredTolerance)
{
    static constexpr uint32_t IMAGE_WIDTH = 2U;
    static constexpr uint32_t IMAGE_HEIGHT = 1U;
    static constexpr uint8_t CHANNEL_TOLERANCE = 2U;
    static constexpr uint64_t ALLOWED_MISMATCHES = 0U;
    static constexpr std::string_view ARTIFACT_STEM = "policy";
    testsupport::Rgba8Image const expected{
        .width = IMAGE_WIDTH,
        .height = IMAGE_HEIGHT,
        .srgb_encoded = false,
        .pixels = { 10U, 20U, 30U, 255U, 40U, 50U, 60U, 255U },
    };
    testsupport::Rgba8Image const actual{
        .width = IMAGE_WIDTH,
        .height = IMAGE_HEIGHT,
        .srgb_encoded = false,
        .pixels = { 11U, 20U, 30U, 255U, 40U, 56U, 60U, 255U },
    };
    testsupport::ImageComparisonPolicy const policy{
        .channel_tolerance = CHANNEL_TOLERANCE,
        .max_mismatched_pixels = ALLOWED_MISMATCHES,
    };
    std::filesystem::path const artifact_directory =
        std::filesystem::current_path() / "image-comparison-test-artifacts";
    testsupport::ImageComparisonResult const result = testsupport::compareRgba8(expected, actual, policy);

    ASSERT_FALSE(result.matches);
    ASSERT_TRUE(testsupport::writeMismatchArtifacts(
        expected,
        actual,
        result,
        policy,
        artifact_directory,
        std::string{ ARTIFACT_STEM }
    ).has_value());
    std::expected<testsupport::Rgba8Image, std::string> const diff = testsupport::readPpm(
        artifact_directory / (std::string{ ARTIFACT_STEM } + "-diff.ppm")
    );

    ASSERT_TRUE(diff.has_value()) << diff.error();
    EXPECT_EQ(diff->pixels[0], 0U);
    EXPECT_EQ(diff->pixels[1], 0U);
    EXPECT_EQ(diff->pixels[2], 0U);
    EXPECT_EQ(diff->pixels[4], 255U);
    EXPECT_EQ(diff->pixels[5], 0U);
    EXPECT_EQ(diff->pixels[6], 0U);
}

TEST(ImageComparisonTest, PreservesDeclaredLinearAndSrgbColorSpacesAcrossRoundTrips)
{
    static constexpr uint32_t IMAGE_WIDTH = 1U;
    static constexpr uint32_t IMAGE_HEIGHT = 1U;
    std::filesystem::path const artifact_directory =
        std::filesystem::current_path() / "image-comparison-test-artifacts";
    testsupport::Rgba8Image const linear{
        .width = IMAGE_WIDTH,
        .height = IMAGE_HEIGHT,
        .srgb_encoded = false,
        .pixels = { 10U, 20U, 30U, 255U },
    };
    testsupport::Rgba8Image const srgb{
        .width = IMAGE_WIDTH,
        .height = IMAGE_HEIGHT,
        .srgb_encoded = true,
        .pixels = { 40U, 50U, 60U, 255U },
    };

    ASSERT_TRUE(testsupport::writePpm(
        linear,
        artifact_directory / "roundtrip-linear.ppm",
        "ImageComparisonTest linear roundtrip"
    ).has_value());
    ASSERT_TRUE(testsupport::writePpm(
        srgb,
        artifact_directory / "roundtrip-srgb.ppm",
        "ImageComparisonTest sRGB roundtrip"
    ).has_value());
    std::expected<testsupport::Rgba8Image, std::string> const linear_roundtrip = testsupport::readPpm(
        artifact_directory / "roundtrip-linear.ppm"
    );
    std::expected<testsupport::Rgba8Image, std::string> const srgb_roundtrip = testsupport::readPpm(
        artifact_directory / "roundtrip-srgb.ppm"
    );

    ASSERT_TRUE(linear_roundtrip.has_value()) << linear_roundtrip.error();
    ASSERT_TRUE(srgb_roundtrip.has_value()) << srgb_roundtrip.error();
    EXPECT_FALSE(linear_roundtrip->srgb_encoded);
    EXPECT_TRUE(srgb_roundtrip->srgb_encoded);
    EXPECT_EQ(linear_roundtrip->pixels, linear.pixels);
    EXPECT_EQ(srgb_roundtrip->pixels, srgb.pixels);
}

TEST(ImageComparisonTest, RejectsInvalidColorDeclarationsAndUnsafePayloadsBeforeAllocation)
{
    static constexpr std::string_view MISSING_COLOR_SPACE = "P6\n1 1\n255\n";
    static constexpr std::string_view DUPLICATE_COLOR_SPACE =
        "P6\n# color-space=linear\n# color-space=srgb\n1 1\n255\n";
    static constexpr std::string_view UNKNOWN_COLOR_SPACE =
        "P6\n# color-space=display-p3\n1 1\n255\n";
    static constexpr std::string_view OVERSIZED_IMAGE =
        "P6\n# color-space=linear\n65536 65536\n255\n";
    static constexpr std::string_view TRUNCATED_IMAGE =
        "P6\n# color-space=linear\n2 2\n255\n";
    std::filesystem::path const artifact_directory =
        std::filesystem::current_path() / "image-comparison-test-artifacts";
    writePpmFixture(artifact_directory / "missing.ppm", MISSING_COLOR_SPACE, { 0U, 0U, 0U });
    writePpmFixture(artifact_directory / "duplicate.ppm", DUPLICATE_COLOR_SPACE, { 0U, 0U, 0U });
    writePpmFixture(artifact_directory / "unknown.ppm", UNKNOWN_COLOR_SPACE, { 0U, 0U, 0U });
    writePpmFixture(artifact_directory / "oversized.ppm", OVERSIZED_IMAGE, {});
    writePpmFixture(artifact_directory / "truncated.ppm", TRUNCATED_IMAGE, { 0U, 0U, 0U });

    std::expected<testsupport::Rgba8Image, std::string> const missing = testsupport::readPpm(
        artifact_directory / "missing.ppm"
    );
    std::expected<testsupport::Rgba8Image, std::string> const duplicate = testsupport::readPpm(
        artifact_directory / "duplicate.ppm"
    );
    std::expected<testsupport::Rgba8Image, std::string> const unknown = testsupport::readPpm(
        artifact_directory / "unknown.ppm"
    );
    std::expected<testsupport::Rgba8Image, std::string> const oversized = testsupport::readPpm(
        artifact_directory / "oversized.ppm"
    );
    std::expected<testsupport::Rgba8Image, std::string> const truncated = testsupport::readPpm(
        artifact_directory / "truncated.ppm"
    );

    ASSERT_FALSE(missing.has_value());
    ASSERT_FALSE(duplicate.has_value());
    ASSERT_FALSE(unknown.has_value());
    ASSERT_FALSE(oversized.has_value());
    ASSERT_FALSE(truncated.has_value());
    EXPECT_EQ(missing.error(), "missing PPM color-space declaration");
    EXPECT_EQ(duplicate.error(), "duplicate PPM color-space declaration");
    EXPECT_EQ(unknown.error(), "unknown PPM color-space declaration: display-p3");
    EXPECT_EQ(oversized.error(), "image exceeds the maximum byte count");
    EXPECT_EQ(truncated.error(), "PPM reference has incomplete pixel data");
}

} // namespace
