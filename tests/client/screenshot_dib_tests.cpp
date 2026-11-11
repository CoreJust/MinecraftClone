#include <client/detail/ScreenshotDibV5.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

uint32_t readLittleEndian32(std::span<uint8_t const> const bytes, uint32_t const offset) noexcept
{
    return static_cast<uint32_t>(bytes[offset])
        | (static_cast<uint32_t>(bytes[offset + 1U]) << 8U)
        | (static_cast<uint32_t>(bytes[offset + 2U]) << 16U)
        | (static_cast<uint32_t>(bytes[offset + 3U]) << 24U);
}

} // namespace

TEST(ScreenshotDibV5Test, EncodesRgbaAsBottomUpBgraWithV5SrgbHeader)
{
    static constexpr std::array<uint8_t, 16U> RGBA8{
        11U, 22U, 33U, 44U, 55U, 66U, 77U, 88U,
        99U, 110U, 121U, 132U, 143U, 154U, 165U, 176U,
    };
    static constexpr std::array<uint8_t, 16U> EXPECTED_BOTTOM_UP_BGRA{
        121U, 110U, 99U, 132U, 165U, 154U, 143U, 176U,
        33U, 22U, 11U, 44U, 77U, 66U, 55U, 88U,
    };
    static constexpr uint32_t DIB_V5_HEADER_SIZE = 124U;
    std::string error = "stale error";

    std::optional<std::vector<uint8_t>> const dib = client::detail::encodeRgba8DibV5(
        2U,
        2U,
        RGBA8,
        error
    );

    ASSERT_TRUE(dib.has_value()) << error;
    EXPECT_TRUE(error.empty());
    ASSERT_EQ(dib->size(), DIB_V5_HEADER_SIZE + EXPECTED_BOTTOM_UP_BGRA.size());
    EXPECT_EQ(readLittleEndian32(*dib, 0U), DIB_V5_HEADER_SIZE);
    EXPECT_EQ(readLittleEndian32(*dib, 4U), 2U);
    EXPECT_EQ(readLittleEndian32(*dib, 8U), 2U);
    EXPECT_EQ((*dib)[12U], 1U);
    EXPECT_EQ((*dib)[14U], 32U);
    EXPECT_EQ(readLittleEndian32(*dib, 16U), 3U);
    EXPECT_EQ(readLittleEndian32(*dib, 20U), EXPECTED_BOTTOM_UP_BGRA.size());
    EXPECT_EQ(readLittleEndian32(*dib, 40U), 0x00FF'0000U);
    EXPECT_EQ(readLittleEndian32(*dib, 44U), 0x0000'FF00U);
    EXPECT_EQ(readLittleEndian32(*dib, 48U), 0x0000'00FFU);
    EXPECT_EQ(readLittleEndian32(*dib, 52U), 0xFF00'0000U);
    EXPECT_EQ(readLittleEndian32(*dib, 56U), 0x7352'4742U);
    EXPECT_TRUE(std::equal(
        EXPECTED_BOTTOM_UP_BGRA.begin(),
        EXPECTED_BOTTOM_UP_BGRA.end(),
        dib->begin() + DIB_V5_HEADER_SIZE
    ));
}

TEST(ScreenshotDibV5Test, RejectsInvalidDimensionsAndStorageWithoutReturningPartialData)
{
    static constexpr std::array<uint8_t, 4U> ONE_PIXEL{ 1U, 2U, 3U, 4U };
    std::string error;

    std::optional<std::vector<uint8_t>> const empty_dimensions = client::detail::encodeRgba8DibV5(
        0U,
        1U,
        ONE_PIXEL,
        error
    );
    EXPECT_FALSE(empty_dimensions.has_value());
    EXPECT_FALSE(error.empty());

    error.clear();
    std::optional<std::vector<uint8_t>> const out_of_range_dimensions = client::detail::encodeRgba8DibV5(
        0x8000'0000U,
        1U,
        ONE_PIXEL,
        error
    );
    EXPECT_FALSE(out_of_range_dimensions.has_value());
    EXPECT_FALSE(error.empty());

    error.clear();
    std::optional<std::vector<uint8_t>> const mismatched_storage = client::detail::encodeRgba8DibV5(
        2U,
        1U,
        ONE_PIXEL,
        error
    );
    EXPECT_FALSE(mismatched_storage.has_value());
    EXPECT_FALSE(error.empty());
}
