#include <acceptance/FramebufferExtent.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <expected>
#include <limits>
#include <string>

namespace {

TEST(FramebufferExtent, KeepsLogicalDimensionsWhenFramebufferAlreadyMatches)
{
    static constexpr acceptance::PixelExtent REQUESTED{ .width = 1'920U, .height = 1'080U };
    static constexpr acceptance::PixelExtent LOGICAL{ .width = 960U, .height = 540U };

    std::expected<acceptance::PixelExtent, std::string> const adjusted = acceptance::adjustedLogicalWindowExtent(
        REQUESTED,
        LOGICAL,
        REQUESTED
    );

    ASSERT_TRUE(adjusted.has_value());
    EXPECT_EQ(adjusted->width, LOGICAL.width);
    EXPECT_EQ(adjusted->height, LOGICAL.height);
}

TEST(FramebufferExtent, ScalesLogicalDimensionsForFractionalFramebufferDensity)
{
    static constexpr acceptance::PixelExtent REQUESTED{ .width = 1'280U, .height = 720U };
    static constexpr acceptance::PixelExtent LOGICAL{ .width = 640U, .height = 360U };
    static constexpr acceptance::PixelExtent ACTUAL{ .width = 960U, .height = 540U };
    static constexpr acceptance::PixelExtent EXPECTED{ .width = 853U, .height = 480U };

    std::expected<acceptance::PixelExtent, std::string> const adjusted = acceptance::adjustedLogicalWindowExtent(
        REQUESTED,
        LOGICAL,
        ACTUAL
    );

    ASSERT_TRUE(adjusted.has_value());
    EXPECT_EQ(adjusted->width, EXPECTED.width);
    EXPECT_EQ(adjusted->height, EXPECTED.height);
}

TEST(FramebufferExtent, RejectsZeroAndUnrepresentableResizeDimensions)
{
    static constexpr acceptance::PixelExtent REQUESTED{ .width = 1'920U, .height = 1'080U };
    static constexpr acceptance::PixelExtent LOGICAL{ .width = 960U, .height = 540U };
    static constexpr acceptance::PixelExtent ZERO_FRAMEBUFFER{ .width = 0U, .height = 1'080U };
    static constexpr acceptance::PixelExtent MAXIMUM{
        .width = std::numeric_limits<uint32_t>::max(),
        .height = std::numeric_limits<uint32_t>::max(),
    };
    static constexpr acceptance::PixelExtent ONE_PIXEL{ .width = 1U, .height = 1U };

    EXPECT_FALSE(acceptance::adjustedLogicalWindowExtent(REQUESTED, LOGICAL, ZERO_FRAMEBUFFER).has_value());
    EXPECT_FALSE(acceptance::adjustedLogicalWindowExtent(MAXIMUM, MAXIMUM, ONE_PIXEL).has_value());
}

} // namespace
