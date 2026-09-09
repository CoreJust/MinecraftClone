#include <acceptance/ImageEvidence.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

TEST(ImageEvidence, WritesPortableRgbPixelsFromRgbaCapture)
{
    std::array<uint8_t, 8> const rgba8{
        1U, 2U, 3U, 4U,
        5U, 6U, 7U, 8U,
    };
    std::filesystem::path const output_path = std::filesystem::temp_directory_path()
        / "minecraftclone-image-evidence-test.ppm";

    ASSERT_TRUE(acceptance::writeRgba8Ppm(output_path, 2U, 1U, rgba8).has_value());
    std::ifstream input{ output_path, std::ios::binary };
    std::string const output{
        std::istreambuf_iterator<char>{ input },
        std::istreambuf_iterator<char>{ },
    };
    std::string const expected{ "P6\n2 1\n255\n\x01\x02\x03\x05\x06\x07", 17 };
    EXPECT_EQ(output, expected);
    EXPECT_TRUE(std::filesystem::remove(output_path));
}

TEST(ImageEvidence, RejectsMismatchedCaptureSize)
{
    std::array<uint8_t, 3> const incomplete_rgba8{ };
    std::filesystem::path const output_path = std::filesystem::temp_directory_path()
        / "minecraftclone-image-evidence-invalid-test.ppm";

    EXPECT_FALSE(acceptance::writeRgba8Ppm(output_path, 1U, 1U, incomplete_rgba8).has_value());
    EXPECT_FALSE(std::filesystem::exists(output_path));
}

std::vector<uint8_t> gameplayCapture(
    bool const expected_player_positions,
    bool const srgb_encoded
)
{
    static constexpr uint32_t EXTENT{ 32U };
    std::vector<uint8_t> capture(EXTENT * EXTENT * 4U, 10U);
    auto setPixel = [&capture](
        uint32_t const x,
        uint32_t const y,
        std::array<uint8_t, 4> const color
    ) {
        uint64_t const offset = (static_cast<uint64_t>(y) * EXTENT + x) * 4U;
        std::ranges::copy(color, capture.begin() + static_cast<int64_t>(offset));
    };
    uint8_t const background = srgb_encoded ? 69U : 15U;
    uint8_t const grid = srgb_encoded ? 111U : 41U;
    std::ranges::fill(capture, background);
    setPixel(1U, 1U, { grid, grid, grid, 255U });
    setPixel(
        expected_player_positions ? 2U : 10U,
        expected_player_positions ? 3U : 10U,
        { 255U, 0U, 0U, 255U }
    );
    setPixel(
        expected_player_positions ? 29U : 20U,
        expected_player_positions ? 28U : 20U,
        { 0U, 255U, 0U, 255U }
    );
    return capture;
}

TEST(ImageEvidence, RequiresSceneColorsInExpectedGameplayRegions)
{
    std::vector<uint8_t> const complete_capture = gameplayCapture(true, true);
    std::vector<uint8_t> const misplaced_players = gameplayCapture(false, true);

    EXPECT_TRUE(acceptance::validateGameplayFrameCapture(32U, 32U, complete_capture, true).has_value());
    EXPECT_FALSE(
        acceptance::validateGameplayFrameCapture(32U, 32U, misplaced_players, true).has_value()
    );
}

TEST(ImageEvidence, AcceptsLinearUnormSceneColors)
{
    std::vector<uint8_t> const complete_capture = gameplayCapture(true, false);

    EXPECT_TRUE(acceptance::validateGameplayFrameCapture(
        32U,
        32U,
        complete_capture,
        false
    ).has_value());
}

} // namespace
