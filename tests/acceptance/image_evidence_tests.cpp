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
    {
        std::ifstream input{ output_path, std::ios::binary };
        std::string const output{
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{ },
        };
        std::string const expected{ "P6\n2 1\n255\n\x01\x02\x03\x05\x06\x07", 17 };
        EXPECT_EQ(output, expected);
    }
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
    bool const include_players,
    bool const srgb_encoded
)
{
    static constexpr uint32_t EXTENT{ 32U };
    std::vector<uint8_t> capture(EXTENT * EXTENT * 4U, 255U);
    auto setPixel = [&capture](
        uint32_t const x,
        uint32_t const y,
        std::array<uint8_t, 4> const color
    ) {
        uint64_t const offset = (static_cast<uint64_t>(y) * EXTENT + x) * 4U;
        std::ranges::copy(color, capture.begin() + static_cast<int64_t>(offset));
    };
    std::array<uint8_t, 4> const sky = srgb_encoded
        ? std::array<uint8_t, 4>{ 160U, 205U, 240U, 255U }
        : std::array<uint8_t, 4>{ 97U, 158U, 224U, 255U };
    for (uint32_t y = 0U; y < EXTENT; ++y) {
        for (uint32_t x = 0U; x < EXTENT; ++x) {
            setPixel(x, y, sky);
        }
    }
    setPixel(1U, 1U, { 28U, 31U, 36U, 255U });
    if (include_players) {
        setPixel(2U, 3U, { 255U, 0U, 0U, 255U });
        setPixel(29U, 28U, { 0U, 255U, 0U, 255U });
    }
    return capture;
}

TEST(ImageEvidence, RequiresSceneColorsAndBothPlayers)
{
    std::vector<uint8_t> const complete_capture = gameplayCapture(true, true);
    std::vector<uint8_t> const missing_players = gameplayCapture(false, true);

    EXPECT_TRUE(acceptance::validateGameplayFrameCapture(32U, 32U, complete_capture, true).has_value());
    EXPECT_FALSE(
        acceptance::validateGameplayFrameCapture(32U, 32U, missing_players, true).has_value()
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
