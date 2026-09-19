#include <shared/world/HeightTileInterest.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <span>

namespace {

bool contains(shared::HeightTileInterest const& interest, int32_t const x, int32_t const y)
{
    return std::ranges::find(interest.keys, shared::HeightTileKey{.x = x, .y = y}) != interest.keys.end();
}

TEST(HeightTileInterestTest, BuildsCameraIndependentCircularResidency)
{
    shared::HeightTileKey constexpr center{.x = 2'000, .y = 2'000};
    shared::HeightTileInterest const interest = shared::makeHeightTileInterest(center, 127, 0);

    ASSERT_FALSE(interest.keys.empty());
    EXPECT_LE(interest.keys.size(), shared::HEIGHT_TILE_INTEREST_COUNT);
    EXPECT_EQ(interest.keys.front(), center);
    EXPECT_TRUE(contains(interest, center.x + 45, center.y));
    EXPECT_TRUE(contains(interest, center.x - 45, center.y));
    EXPECT_TRUE(contains(interest, center.x, center.y + 45));
    EXPECT_FALSE(contains(interest, center.x + 32, center.y + 32));
    EXPECT_FALSE(contains(interest, center.x - 46, center.y));
}

TEST(HeightTileInterestTest, RotationChangesPriorityWithoutChangingResidency)
{
    shared::HeightTileKey constexpr center{.x = 2'000, .y = 2'000};
    shared::HeightTileInterest const east = shared::makeHeightTileInterest(center, 127, 0);
    shared::HeightTileInterest const north = shared::makeHeightTileInterest(center, 0, 127);

    std::vector<shared::HeightTileKey> east_keys = east.keys;
    std::vector<shared::HeightTileKey> north_keys = north.keys;
    std::ranges::sort(east_keys, {}, [](shared::HeightTileKey const key) { return std::pair{key.x, key.y}; });
    std::ranges::sort(north_keys, {}, [](shared::HeightTileKey const key) { return std::pair{key.x, key.y}; });
    EXPECT_EQ(east_keys, north_keys);
    EXPECT_NE(east.keys, north.keys);
}

TEST(HeightTileInterestTest, ProgressivePriorityUsesNearCirclesThenDirectionalEllipse)
{
    shared::HeightTileKey constexpr center{.x = 2'000, .y = 2'000};
    shared::HeightTileInterest const interest = shared::makeHeightTileInterest(center, 127, 0);

    for (size_t const prefix_size : {500U, 1'500U, 3'000U}) {
        ASSERT_LT(prefix_size, interest.keys.size());
        int32_t forward_extent = 0;
        int32_t rear_extent = 0;
        int32_t side_extent = 0;
        for (shared::HeightTileKey const key : std::span{interest.keys}.first(prefix_size)) {
            forward_extent = std::max(forward_extent, key.x - center.x);
            rear_extent = std::max(rear_extent, center.x - key.x);
            side_extent = std::max(side_extent, std::abs(key.y - center.y));
        }
        EXPECT_GT(forward_extent, side_extent);
        EXPECT_GT(forward_extent, rear_extent);
        EXPECT_LE(forward_extent, 45);
    }
    for (shared::HeightTileKey const key : std::span{interest.keys}.first(29U)) {
        int32_t const x = key.x - center.x;
        int32_t const y = key.y - center.y;
        EXPECT_LE(x * x + y * y, 9);
    }
    for (shared::HeightTileKey const key : std::span{interest.keys}.first(197U)) {
        int32_t const x = key.x - center.x;
        int32_t const y = key.y - center.y;
        EXPECT_LE(x * x + y * y, 64);
    }
}

TEST(HeightTileInterestTest, WrapsAtWorldBoundariesWithoutChangingShape)
{
    shared::HeightTileInterest const interior = shared::makeHeightTileInterest({.x = 2'000, .y = 2'000}, 127, 0);
    shared::HeightTileInterest const boundary = shared::makeHeightTileInterest({.x = 4'095, .y = 4'095}, 127, 0);

    EXPECT_EQ(boundary.keys.size(), interior.keys.size());
    EXPECT_TRUE(contains(boundary, 0, 4'095));
    EXPECT_TRUE(contains(boundary, 4'050, 4'095));
    EXPECT_EQ(boundary.keys.front(), (shared::HeightTileKey{.x = 4'095, .y = 4'095}));
}

TEST(HeightTileInterestTest, KeepsResidencyStableForSmallHeadingJitter)
{
    shared::HeightTileKey constexpr center{.x = 2'000, .y = 2'000};
    shared::HeightTileInterest const first = shared::makeHeightTileInterest(center, 127, 0);
    shared::HeightTileInterest const jittered = shared::makeHeightTileInterest(center, 127, 8);
    shared::HeightTileInterest const turned = shared::makeHeightTileInterest(center, 117, 49);

    EXPECT_EQ(jittered.keys, first.keys);
    EXPECT_NE(turned.keys, first.keys);
}

TEST(HeightTileInterestTest, ClassifiesFixedNearBandsDirectionalMiddleAndBackground)
{
    shared::HeightTileKey constexpr center{.x = 2'000, .y = 2'000};
    using enum shared::HeightTileGenerationBand;
    EXPECT_EQ(shared::heightTileGenerationBand(center, 127, 0, center), Immediate);
    EXPECT_EQ(shared::heightTileGenerationBand(center, 127, 0, {.x = 2'003, .y = 2'000}), Immediate);
    EXPECT_EQ(shared::heightTileGenerationBand(center, 127, 0, {.x = 2'008, .y = 2'000}), Near);
    EXPECT_EQ(shared::heightTileGenerationBand(center, 127, 0, {.x = 2'040, .y = 2'000}), Directional);
    EXPECT_EQ(shared::heightTileGenerationBand(center, 127, 0, {.x = 1'960, .y = 2'000}), Background);
    EXPECT_EQ(shared::heightTileGenerationBand(center, 0, 127, {.x = 2'000, .y = 2'040}), Directional);
}

} // namespace
