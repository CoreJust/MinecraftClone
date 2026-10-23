#include <client/render/MapImageData.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

TEST(MapImageDataTest, CopiesOpaquePixelsAndKeepsUnchangedRevisionStable)
{
    static constexpr std::array<client::detail::MapImageData::Pixel, 4> PIXELS{
        client::detail::MapImageData::Pixel{ 1U, 2U, 3U, 255U },
        client::detail::MapImageData::Pixel{ 4U, 5U, 6U, 255U },
        client::detail::MapImageData::Pixel{ 7U, 8U, 9U, 255U },
        client::detail::MapImageData::Pixel{ 10U, 11U, 12U, 255U },
    };
    std::array<client::detail::MapImageData::Pixel, PIXELS.size()> input = PIXELS;
    client::detail::MapImageData image;

    ASSERT_TRUE(image.set(2U, 2U, input));
    ASSERT_EQ(image.pixels().size(), PIXELS.size());
    EXPECT_EQ(image.pixels()[0], PIXELS[0]);
    EXPECT_EQ(image.pixels()[3], PIXELS[3]);
    uint64_t const first_revision = image.revision();
    ASSERT_NE(first_revision, 0U);

    input[0] = { 200U, 201U, 202U, 255U };
    EXPECT_EQ(image.pixels()[0], PIXELS[0]);
    EXPECT_TRUE(image.set(2U, 2U, PIXELS));
    EXPECT_EQ(image.revision(), first_revision);

    static constexpr std::array<client::detail::MapImageData::Pixel, 1> REPLACEMENT{
        client::detail::MapImageData::Pixel{ 20U, 30U, 40U, 255U },
    };
    EXPECT_TRUE(image.set(1U, 1U, REPLACEMENT));
    EXPECT_EQ(image.pixels().size(), 1U);
    EXPECT_EQ(image.pixels()[0], REPLACEMENT[0]);
    EXPECT_GT(image.revision(), first_revision);
}

TEST(MapImageDataTest, InvalidImagesDoNotReplaceTheLatestValidPixels)
{
    static constexpr std::array<client::detail::MapImageData::Pixel, 1> ORIGINAL{
        client::detail::MapImageData::Pixel{ 31U, 42U, 53U, 255U },
    };
    static constexpr std::array<client::detail::MapImageData::Pixel, 1> TRANSPARENT{
        client::detail::MapImageData::Pixel{ 61U, 72U, 83U, 254U },
    };
    client::detail::MapImageData image;
    ASSERT_TRUE(image.set(1U, 1U, ORIGINAL));
    uint64_t const revision = image.revision();
    std::vector<client::detail::MapImageData::Pixel> const over_bound_pixels(513U, ORIGINAL[0]);

    EXPECT_FALSE(image.set(0U, 1U, ORIGINAL));
    EXPECT_FALSE(image.set(513U, 1U, over_bound_pixels));
    EXPECT_FALSE(image.set(1U, 513U, over_bound_pixels));
    EXPECT_FALSE(image.set(2U, 1U, ORIGINAL));
    EXPECT_FALSE(image.set(1U, 1U, TRANSPARENT));

    ASSERT_EQ(image.pixels().size(), ORIGINAL.size());
    EXPECT_EQ(image.pixels()[0], ORIGINAL[0]);
    EXPECT_EQ(image.revision(), revision);
}

TEST(MapImageDataTest, AcceptsTheMaximumBoundedImage)
{
    static constexpr uint32_t DIMENSION = client::detail::MapImageData::MAXIMUM_DIMENSION;
    static constexpr client::detail::MapImageData::Pixel PIXEL{ 12U, 34U, 56U, 255U };
    std::vector<client::detail::MapImageData::Pixel> const pixels(
        static_cast<size_t>(DIMENSION) * DIMENSION,
        PIXEL
    );
    client::detail::MapImageData image;

    ASSERT_TRUE(image.set(DIMENSION, DIMENSION, pixels));
    EXPECT_EQ(image.pixels().size(), static_cast<size_t>(DIMENSION) * DIMENSION);
    EXPECT_EQ(image.pixels().front(), PIXEL);
    EXPECT_EQ(image.pixels().back(), PIXEL);
}

TEST(MapImageDataTest, ClearInvalidatesOnlyAnExistingImage)
{
    static constexpr std::array<client::detail::MapImageData::Pixel, 1> PIXELS{
        client::detail::MapImageData::Pixel{ 91U, 102U, 113U, 255U },
    };
    client::detail::MapImageData image;

    image.clear();
    EXPECT_EQ(image.revision(), 0U);
    ASSERT_TRUE(image.set(1U, 1U, PIXELS));
    uint64_t const revision = image.revision();
    image.clear();
    EXPECT_TRUE(image.empty());
    EXPECT_EQ(image.pixels().size(), 0U);
    EXPECT_GT(image.revision(), revision);
    uint64_t const cleared_revision = image.revision();
    image.clear();
    EXPECT_EQ(image.revision(), cleared_revision);
}
