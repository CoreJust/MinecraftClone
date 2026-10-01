#include <client/PlayerPreviewLod.hpp>

#include <shared/world/HeightTileInterest.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/World.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace {

struct ReferenceTile final {
    shared::HeightTileKey key;
    uint16_t minimum;
    uint16_t maximum;
    shared::HeightTileSurfaceDetail detail;
};

using KeySet = std::set<std::pair<int32_t, int32_t>>;

[[nodiscard]] KeySet keySet(std::vector<shared::HeightTileKey> const& keys)
{
    KeySet result;
    for (shared::HeightTileKey const key : keys) {
        result.emplace(key.x, key.y);
    }
    return result;
}

[[nodiscard]] shared::HeightTileSurfaceBounds viewerBounds(double const x, double const y, double const z)
{
    return { x, x + 0.625, y, y + 0.625, z, z + 1.75 };
}

[[nodiscard]] std::vector<ReferenceTile> populate(
    client::PlayerPreviewLod& lod,
    shared::HeightTileKey const center,
    shared::HeightTileSurfaceBounds const viewer,
    shared::HeightTileSurfaceProjection const projection
)
{
    static_cast<void>(lod.refresh(center, viewer, projection));
    shared::HeightTileInterest const interest = shared::makeHeightTileInterest(center, 0, 127);
    std::vector<ReferenceTile> result;
    result.reserve(interest.keys.size());
    for (shared::HeightTileKey const key : interest.keys) {
        uint32_t const seed = static_cast<uint32_t>(key.x) * 1'664'525U
            + static_cast<uint32_t>(key.y) * 1'013'904'223U;
        uint16_t const minimum = static_cast<uint16_t>(20 + (key.x % 128) / 8 + (key.y % 128) / 8);
        uint16_t const span = seed % 97U == 0U ? 600U : static_cast<uint16_t>((seed >> 8U) % 4U);
        uint16_t const maximum = minimum + span;
        result.push_back({ key, minimum, maximum, lod.selectDetail(key, minimum, maximum) });
    }
    return result;
}

void expectReference(
    client::PlayerPreviewLod& lod,
    std::vector<ReferenceTile>& reference,
    shared::HeightTileKey const center,
    shared::HeightTileSurfaceBounds const viewer,
    shared::HeightTileSurfaceProjection const projection,
    client::PlayerPreviewLodRefresh const& refresh
)
{
    shared::HeightTileSurfaceLodPolicy const policy{ projection };
    KeySet expected_changes;
    for (ReferenceTile& tile : reference) {
        shared::HeightTileSurfaceBounds const surface = shared::HeightTileSurfaceMesher::boundsForTile(
            { center.x, center.y }, { tile.key.x, tile.key.y }, tile.minimum, tile.maximum
        );
        shared::HeightTileSurfaceDetail const selected = policy.detailFor(viewer, surface, tile.detail);
        if (selected != tile.detail) {
            expected_changes.emplace(tile.key.x, tile.key.y);
        }
        tile.detail = selected;
        ASSERT_EQ(lod.detail(tile.key), std::optional{selected}) << tile.key.x << ',' << tile.key.y;
    }
    EXPECT_EQ(keySet(refresh.changed_keys), expected_changes);
    EXPECT_EQ(refresh.changed_keys.size(), expected_changes.size());
}

} // namespace

TEST(PlayerPreviewLodTest, UnchangedProjectionAndSubtileMovementAvoidFullResidencyEvaluation)
{
    static constexpr shared::HeightTileKey CENTER{ 2'000, 2'000 };
    static constexpr shared::HeightTileSurfaceProjection PROJECTION{};
    static constexpr uint32_t STEPS = 8U;
    client::PlayerPreviewLod lod;
    auto reference = populate(lod, CENTER, viewerBounds(8.0, 8.0, 80.0), PROJECTION);
    auto const first = lod.refresh(CENTER, viewerBounds(8.1, 8.0, 80.0), PROJECTION);
    expectReference(lod, reference, CENTER, viewerBounds(8.1, 8.0, 80.0), PROJECTION, first);
    EXPECT_LT(first.classified_buckets, shared::HEIGHT_TILE_INTEREST_COUNT / 8U);
    EXPECT_LT(first.evaluated_tiles, shared::HEIGHT_TILE_INTEREST_COUNT / 4U);
    RecordProperty("resident_tiles", shared::HEIGHT_TILE_INTEREST_COUNT);
    RecordProperty("first_classified_buckets", first.classified_buckets);
    RecordProperty("first_evaluated_tiles", first.evaluated_tiles);
    for (uint32_t step = 1U; step <= STEPS; ++step) {
        auto const viewer = viewerBounds(8.1 + step * 0.1, 8.0 + step * 0.05, 80.0);
        auto const refreshed = lod.refresh(CENTER, viewer, PROJECTION);
        expectReference(lod, reference, CENTER, viewer, PROJECTION, refreshed);
        EXPECT_LT(refreshed.evaluated_tiles, shared::HEIGHT_TILE_INTEREST_COUNT / 4U);
    }
    auto const unchanged = lod.refresh(CENTER, viewerBounds(8.9, 8.4, 80.0), PROJECTION);
    EXPECT_EQ(unchanged.classified_buckets, 0U);
    EXPECT_EQ(unchanged.evaluated_tiles, 0U);
    EXPECT_TRUE(unchanged.changed_keys.empty());
}

TEST(PlayerPreviewLodTest, SeededMovementAndX200StepsMatchFullPolicyAcrossWrap)
{
    static constexpr int32_t TILE_EXTENT = shared::WorldExtent::WIDTH / shared::HEIGHT_TILE_SIDE_LENGTH;
    static constexpr shared::HeightTileKey INITIAL_CENTER{ TILE_EXTENT - 4, 2'000 };
    static constexpr shared::HeightTileSurfaceProjection PROJECTION{};
    static constexpr uint32_t STEPS = 24U;
    static constexpr double X200_STEP = static_cast<double>(shared::MOVEMENT_SUBCELLS_PER_TICK) * 200.0
        / shared::SUBCELLS_PER_CELL;
    client::PlayerPreviewLod lod;
    auto reference = populate(lod, INITIAL_CENTER, viewerBounds(8.0, 8.0, 80.0), PROJECTION);
    for (uint32_t step = 1U; step <= STEPS; ++step) {
        double const displacement = step <= STEPS / 2U ? step * 0.56 : (step - STEPS / 2U) * X200_STEP;
        int32_t const tiles_crossed = static_cast<int32_t>(displacement / shared::HEIGHT_TILE_SIDE_LENGTH);
        auto const center = shared::normalizeHeightTileKey({ INITIAL_CENTER.x + tiles_crossed, INITIAL_CENTER.y });
        auto const viewer = viewerBounds(
            8.0 + displacement - static_cast<double>(tiles_crossed) * shared::HEIGHT_TILE_SIDE_LENGTH,
            8.0,
            80.0 + (step % 5U) * 3.5
        );
        auto const refreshed = lod.refresh(center, viewer, PROJECTION);
        expectReference(lod, reference, center, viewer, PROJECTION, refreshed);
        EXPECT_LT(refreshed.evaluated_tiles, shared::HEIGHT_TILE_INTEREST_COUNT / 3U);
    }
}

TEST(PlayerPreviewLodTest, FovAndFramebufferTighteningImmediatelyMatchTheFullPolicy)
{
    static constexpr shared::HeightTileKey CENTER{ 2'000, 2'000 };
    static constexpr std::array<shared::HeightTileSurfaceProjection, 5> PROJECTIONS{
        shared::HeightTileSurfaceProjection{ 100.0, 640U, 360U },
        shared::HeightTileSurfaceProjection{ 70.0, 1'440U, 900U },
        shared::HeightTileSurfaceProjection{ 40.0, 1'440U, 900U },
        shared::HeightTileSurfaceProjection{ 40.0, 3'840U, 2'160U },
        shared::HeightTileSurfaceProjection{ 100.0, 640U, 360U },
    };
    client::PlayerPreviewLod lod;
    auto const viewer = viewerBounds(8.0, 8.0, 80.0);
    auto reference = populate(lod, CENTER, viewer, PROJECTIONS.front());
    for (auto const projection : PROJECTIONS) {
        auto const refreshed = lod.refresh(CENTER, viewer, projection);
        expectReference(lod, reference, CENTER, viewer, projection, refreshed);
    }
}

TEST(PlayerPreviewLodTest, ChangedElevationAndUnknownTilesCannotUseStaleBounds)
{
    static constexpr shared::HeightTileKey CENTER{ 2'000, 2'000 };
    static constexpr shared::HeightTileKey DISTANT{ 2'180, 2'000 };
    static constexpr shared::HeightTileSurfaceProjection PROJECTION{};
    static constexpr uint16_t OLD_HEIGHT = 20U;
    static constexpr uint16_t NEW_MAXIMUM = 820U;
    client::PlayerPreviewLod lod;
    EXPECT_EQ(lod.detail(DISTANT), std::nullopt);
    static_cast<void>(lod.refresh(CENTER, viewerBounds(8.0, 8.0, 80.0), PROJECTION));
    auto const captured_job_detail = lod.selectDetail(DISTANT, OLD_HEIGHT, OLD_HEIGHT);
    ASSERT_NE(captured_job_detail, shared::HeightTileSurfaceDetail::Fine);
    EXPECT_EQ(lod.selectDetail(DISTANT, OLD_HEIGHT, NEW_MAXIMUM), shared::HeightTileSurfaceDetail::Fine);
    EXPECT_NE(lod.detail(DISTANT), std::optional{captured_job_detail});
    lod.erase(DISTANT);
    EXPECT_EQ(lod.detail(DISTANT), std::nullopt);
    EXPECT_EQ(lod.selectDetail(DISTANT, OLD_HEIGHT, NEW_MAXIMUM), shared::HeightTileSurfaceDetail::Fine);
    lod.clear();
    EXPECT_EQ(lod.detail(DISTANT), std::nullopt);
    EXPECT_EQ(lod.selectDetail(DISTANT, OLD_HEIGHT, OLD_HEIGHT), shared::HeightTileSurfaceDetail::Fine);
}

TEST(PlayerPreviewLodTest, ProjectionTighteningRejectsCapturedCoarseJobDetail)
{
    static constexpr shared::HeightTileKey CENTER{ 2'000, 2'000 };
    static constexpr shared::HeightTileKey DISTANT{ 2'100, 2'000 };
    static constexpr shared::HeightTileSurfaceProjection OLD_PROJECTION{ 100.0, 640U, 360U };
    static constexpr shared::HeightTileSurfaceProjection NEW_PROJECTION{ 40.0, 3'840U, 2'160U };
    static constexpr uint16_t HEIGHT = 20U;
    client::PlayerPreviewLod lod;
    auto const viewer = viewerBounds(8.0, 8.0, 80.0);
    static_cast<void>(lod.refresh(CENTER, viewer, OLD_PROJECTION));
    auto const captured_job_detail = lod.selectDetail(DISTANT, HEIGHT, HEIGHT);
    ASSERT_NE(captured_job_detail, shared::HeightTileSurfaceDetail::Fine);
    auto const refreshed = lod.refresh(CENTER, viewer, NEW_PROJECTION);
    EXPECT_EQ(refreshed.changed_keys, (std::vector{DISTANT}));
    EXPECT_EQ(lod.detail(DISTANT), shared::HeightTileSurfaceDetail::Fine);
    EXPECT_NE(lod.selectDetail(DISTANT, HEIGHT, HEIGHT), captured_job_detail);
}

TEST(PlayerPreviewLodTest, BoundaryStripsMatchTheExactDiskForCrossingJumpsAndWrap)
{
    static constexpr int32_t TILE_EXTENT = shared::WorldExtent::WIDTH / shared::HEIGHT_TILE_SIDE_LENGTH;
    static constexpr std::array<shared::HeightTileKey, 8> CENTERS{
        shared::HeightTileKey{ 2'000, 2'000 },
        shared::HeightTileKey{ 2'001, 2'000 },
        shared::HeightTileKey{ 2'008, 2'007 },
        shared::HeightTileKey{ 1'998, 1'993 },
        shared::HeightTileKey{ TILE_EXTENT - 1, TILE_EXTENT - 1 },
        shared::HeightTileKey{ 0, 0 },
        shared::HeightTileKey{ 8, TILE_EXTENT - 7 },
        shared::HeightTileKey{ TILE_EXTENT / 2, TILE_EXTENT / 2 },
    };
    KeySet current = keySet(shared::makeHeightTileInterest(CENTERS.front(), 0, 127).keys);
    for (uint32_t step = 1U; step < CENTERS.size(); ++step) {
        auto const delta = client::playerPreviewInterestDelta(CENTERS[step - 1U], CENTERS[step]);
        EXPECT_EQ(keySet(delta.additions).size(), delta.additions.size());
        EXPECT_EQ(keySet(delta.removals).size(), delta.removals.size());
        for (auto const key : delta.removals) {
            EXPECT_EQ(current.erase({key.x, key.y}), 1U);
        }
        for (auto const key : delta.additions) {
            EXPECT_TRUE(current.emplace(key.x, key.y).second);
        }
        EXPECT_EQ(current, keySet(shared::makeHeightTileInterest(CENTERS[step], 127, 0).keys));
        EXPECT_EQ(current.size(), shared::HEIGHT_TILE_INTEREST_COUNT);
    }
    auto const stationary = client::playerPreviewInterestDelta(CENTERS.front(), CENTERS.front());
    EXPECT_TRUE(stationary.additions.empty());
    EXPECT_TRUE(stationary.removals.empty());
}

TEST(PlayerPreviewLodTest, SelectedRadiusMovementDeltasMatchExactDiskSetDifferences)
{
    static constexpr int32_t TILE_EXTENT = shared::WorldExtent::WIDTH / shared::HEIGHT_TILE_SIDE_LENGTH;
    static constexpr uint32_t RADIUS_128 = 128U;
    static constexpr uint32_t RADIUS_256 = 256U;
    static constexpr std::array<uint32_t, 2> RADII{ RADIUS_128, RADIUS_256 };
    static constexpr std::array<shared::HeightTileKey, 4> CENTERS{
        shared::HeightTileKey{ TILE_EXTENT - 2, TILE_EXTENT - 6 },
        shared::HeightTileKey{ 3, 4 },
        shared::HeightTileKey{ 120, 80 },
        shared::HeightTileKey{ TILE_EXTENT - 96, TILE_EXTENT - 110 },
    };

    for (uint32_t const radius : RADII) {
        for (uint32_t step = 1U; step < CENTERS.size(); ++step) {
            KeySet const previous = keySet(shared::makeHeightTileInterest(CENTERS[step - 1U], 0, 127, radius).keys);
            KeySet const next = keySet(shared::makeHeightTileInterest(CENTERS[step], 127, 0, radius).keys);
            KeySet expected_additions;
            KeySet expected_removals;
            std::ranges::set_difference(next, previous, std::inserter(expected_additions, expected_additions.end()));
            std::ranges::set_difference(previous, next, std::inserter(expected_removals, expected_removals.end()));

            auto const delta = client::playerPreviewInterestDelta(CENTERS[step - 1U], CENTERS[step], radius);
            KeySet const actual_additions = keySet(delta.additions);
            KeySet const actual_removals = keySet(delta.removals);
            EXPECT_EQ(actual_additions, expected_additions) << "radius " << radius << ", step " << step;
            EXPECT_EQ(actual_removals, expected_removals) << "radius " << radius << ", step " << step;
            EXPECT_EQ(actual_additions.size(), delta.additions.size());
            EXPECT_EQ(actual_removals.size(), delta.removals.size());
        }

        auto const stationary = client::playerPreviewInterestDelta(CENTERS.back(), CENTERS.back(), radius);
        EXPECT_TRUE(stationary.additions.empty());
        EXPECT_TRUE(stationary.removals.empty());
    }
}
