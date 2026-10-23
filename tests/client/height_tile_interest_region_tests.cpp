#include <client/HeightTileInterestRegion.hpp>

#include <shared/net/Message.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

TEST(HeightTileInterestRegionTest, MatchesExactCircularMembershipAcrossSupportedRadiiAndWorldWrap)
{
    static constexpr std::array<uint32_t, 4> RADII{ 1U, 72U, 128U, 256U };
    static constexpr shared::HeightTileKey CENTER{ .x = 4'095, .y = 4'095 };
    client::HeightTileInterestRegion region;

    for (uint32_t const radius : RADII) {
        region.set(CENTER, radius);
        uint32_t expected_count = 0U;
        int32_t const extent = static_cast<int32_t>(radius);
        int64_t const radius_squared = static_cast<int64_t>(radius) * radius;
        for (int32_t y = -extent; y <= extent; ++y) {
            for (int32_t x = -extent; x <= extent; ++x) {
                shared::HeightTileKey const key = shared::normalizeHeightTileKey({
                    .x = CENTER.x + x,
                    .y = CENTER.y + y,
                });
                int64_t const distance_squared = static_cast<int64_t>(x) * x
                    + static_cast<int64_t>(y) * y;
                bool const expected = distance_squared <= radius_squared;
                EXPECT_EQ(region.contains(key), expected) << "radius=" << radius << " x=" << x << " y=" << y;
                expected_count += expected ? 1U : 0U;
            }
        }
        EXPECT_EQ(region.size(), expected_count);
        EXPECT_EQ(region.size(), shared::heightTileInterestCount(radius));
        EXPECT_TRUE(region.contains(shared::normalizeHeightTileKey({ CENTER.x + extent, CENTER.y })));
        EXPECT_FALSE(region.contains(shared::normalizeHeightTileKey({ CENTER.x + extent + 1, CENTER.y })));
    }
}

TEST(HeightTileInterestRegionTest, NormalizedCenterAndRadiusDefineValueAndClearRemovesCoverage)
{
    static constexpr uint32_t RADIUS = 72U;
    static constexpr shared::HeightTileKey CENTER{ .x = 4'096, .y = -1 };
    client::HeightTileInterestRegion first;
    client::HeightTileInterestRegion equivalent;
    first.set(CENTER, RADIUS);
    equivalent.set({ .x = 0, .y = 4'095 }, RADIUS);
    std::optional<shared::HeightTileKey> const EXPECTED_CENTER{ shared::HeightTileKey{ .x = 0, .y = 4'095 } };

    EXPECT_EQ(first, equivalent);
    EXPECT_EQ(first.center(), EXPECTED_CENTER);
    EXPECT_TRUE(first.contains({ .x = 0, .y = 4'095 }));

    first.clear();

    EXPECT_TRUE(first.empty());
    EXPECT_EQ(first.size(), 0U);
    EXPECT_FALSE(first.center().has_value());
    EXPECT_FALSE(first.contains({ .x = 0, .y = 4'095 }));
    EXPECT_NE(first, equivalent);
}

TEST(HeightTileInterestRegionTest, InvalidRadiusUsesTheSharedAssertionContract)
{
    // Re-exec so assertion exit cannot clean up worker handles inherited from earlier tests.
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    client::HeightTileInterestRegion region;
    EXPECT_EXIT(region.set({ .x = 0, .y = 0 }, 0U), ::testing::ExitedWithCode(1),
        "invalid height-tile interest radius");
    EXPECT_EXIT(region.set({ .x = 0, .y = 0 }, 257U), ::testing::ExitedWithCode(1),
        "invalid height-tile interest radius");
}
