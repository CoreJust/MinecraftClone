#include <client/PlayerPreviewLod.hpp>

#include <shared/net/Message.hpp>
#include <shared/world/HeightTileInterest.hpp>
#include <shared/world/HeightTileSurfaceMesher.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldGeneration.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

struct ReferenceTile final {
    shared::HeightTileKey key;
    uint16_t minimum;
    uint16_t maximum;
    shared::HeightTileSurfaceFaceAreas face_areas;
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

[[nodiscard]] shared::HeightTileSurfaceFaceAreas faceAreasForVerticalSpan(uint16_t const vertical_span)
{
    shared::HeightTileSurfaceFaceAreas face_areas;
    for (uint32_t level = 0U; level < face_areas.maximum.size(); ++level) {
        double const cell_size = shared::HeightTileSurfaceFaceAreas::CELL_SIZES[level];
        face_areas.maximum[level] = std::max(cell_size * cell_size, cell_size * vertical_span);
    }
    return face_areas;
}

[[nodiscard]] ReferenceTile canonicalReliefTile(
    client::PlayerPreviewLod& lod,
    shared::TerrainGenerator const& terrain,
    shared::HeightTileKey const key
)
{
    static constexpr int64_t TILE_SIDE = shared::HEIGHT_TILE_SIDE_LENGTH;
    static constexpr int64_t HALF_TILE_SIDE = TILE_SIDE / 2;
    int64_t const world_x = static_cast<int64_t>(key.x) * TILE_SIDE + HALF_TILE_SIDE;
    int64_t const world_y = static_cast<int64_t>(key.y) * TILE_SIDE + HALF_TILE_SIDE;
    std::array<uint16_t, 5U> const samples{
        terrain.heightAt(world_x, world_y),
        terrain.heightAt(world_x - TILE_SIDE, world_y),
        terrain.heightAt(world_x + TILE_SIDE, world_y),
        terrain.heightAt(world_x, world_y - TILE_SIDE),
        terrain.heightAt(world_x, world_y + TILE_SIDE),
    };
    auto const [minimum, maximum] = std::ranges::minmax(samples);
    uint16_t const vertical_span = static_cast<uint16_t>(maximum - minimum);
    shared::HeightTileSurfaceFaceAreas const face_areas = faceAreasForVerticalSpan(vertical_span);
    shared::HeightTileSurfaceDetail const detail = lod.selectDetail(key, minimum, maximum, face_areas);
    return { key, minimum, maximum, face_areas, detail };
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
        shared::HeightTileSurfaceFaceAreas const face_areas = faceAreasForVerticalSpan(span);
        shared::HeightTileSurfaceDetail const detail = lod.selectDetail(key, minimum, maximum, face_areas);
        result.push_back({ key, minimum, maximum, face_areas, detail });
    }
    return result;
}

[[nodiscard]] std::vector<ReferenceTile> populateCanonicalRelief(
    client::PlayerPreviewLod& lod,
    shared::TerrainGenerator const& terrain,
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
        result.push_back(canonicalReliefTile(lod, terrain, key));
    }
    return result;
}

[[nodiscard]] client::PlayerPreviewLodRefresh advanceCanonicalRelief(
    client::PlayerPreviewLod& lod,
    shared::TerrainGenerator const& terrain,
    std::vector<ReferenceTile>& reference,
    shared::HeightTileKey const current_center,
    shared::HeightTileKey const next_center,
    shared::HeightTileSurfaceBounds const viewer,
    shared::HeightTileSurfaceProjection const projection
)
{
    client::PlayerPreviewInterestDelta const delta = client::playerPreviewInterestDelta(
        current_center, next_center
    );
    lod.eraseBatch(delta.removals);
    KeySet const removed_keys = [&delta] {
        KeySet keys;
        for (shared::HeightTileKey const key : delta.removals) {
            keys.emplace(key.x, key.y);
        }
        return keys;
    }();
    std::erase_if(reference, [&removed_keys](ReferenceTile const& tile) {
        return removed_keys.contains({ tile.key.x, tile.key.y });
    });
    for (shared::HeightTileKey const key : delta.additions) {
        reference.push_back(canonicalReliefTile(lod, terrain, key));
    }
    return lod.refresh(next_center, viewer, projection);
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
        shared::HeightTileSurfaceDetail const selected = policy.detailFor(
            viewer, surface, tile.face_areas, tile.detail
        );
        std::optional<shared::HeightTileSurfaceDetail> const actual = lod.detail(tile.key);
        ASSERT_TRUE(actual.has_value()) << tile.key.x << ',' << tile.key.y;
        bool const may_defer_coarsening = tile.detail == shared::HeightTileSurfaceDetail::Fine
            && selected != shared::HeightTileSurfaceDetail::Fine;
        if (may_defer_coarsening) {
            EXPECT_TRUE(*actual == shared::HeightTileSurfaceDetail::Fine || *actual == selected)
                << tile.key.x << ',' << tile.key.y;
        } else {
            EXPECT_EQ(*actual, selected) << tile.key.x << ',' << tile.key.y;
        }
        if (*actual != tile.detail) {
            expected_changes.emplace(tile.key.x, tile.key.y);
        }
        tile.detail = *actual;
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
    static constexpr uint32_t MAX_STATIONARY_DRAIN_CALLS = shared::HEIGHT_TILE_INTEREST_COUNT / 64U + 2U;
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
        EXPECT_LT(refreshed.visited_buckets, shared::HEIGHT_TILE_INTEREST_COUNT / 32U);
        EXPECT_LT(refreshed.evaluated_tiles, shared::HEIGHT_TILE_INTEREST_COUNT / 4U);
    }
    shared::HeightTileSurfaceBounds const unchanged_viewer = viewerBounds(8.9, 8.4, 80.0);
    auto const unchanged = lod.refresh(CENTER, unchanged_viewer, PROJECTION);
    expectReference(lod, reference, CENTER, unchanged_viewer, PROJECTION, unchanged);
    EXPECT_LT(unchanged.visited_buckets, shared::HEIGHT_TILE_INTEREST_COUNT / 32U);
    EXPECT_LT(unchanged.evaluated_tiles, shared::HEIGHT_TILE_INTEREST_COUNT / 4U);
    EXPECT_TRUE(unchanged.changed_keys.empty());

    bool reached_idle = false;
    for (uint32_t attempt = 0U; attempt < MAX_STATIONARY_DRAIN_CALLS; ++attempt) {
        auto const drained = lod.refresh(CENTER, unchanged_viewer, PROJECTION);
        expectReference(lod, reference, CENTER, unchanged_viewer, PROJECTION, drained);
        EXPECT_LT(drained.visited_buckets, shared::HEIGHT_TILE_INTEREST_COUNT / 32U);
        EXPECT_LT(drained.evaluated_tiles, shared::HEIGHT_TILE_INTEREST_COUNT / 4U);
        if (drained.visited_buckets == 0U) {
            EXPECT_EQ(drained.classified_buckets, 0U);
            EXPECT_EQ(drained.evaluated_tiles, 0U);
            EXPECT_TRUE(drained.changed_keys.empty());
            reached_idle = true;
            break;
        }
    }
    EXPECT_TRUE(reached_idle);
}

TEST(PlayerPreviewLodTest, TileCenterMovementReusesTranslatedCertificates)
{
    static constexpr shared::HeightTileKey INITIAL_CENTER{ 2'000, 2'000 };
    static constexpr shared::HeightTileSurfaceProjection PROJECTION{};
    shared::HeightTileSurfaceBounds const VIEWER = viewerBounds(8.0, 8.0, 80.0);
    shared::HeightTileSurfaceBounds const PRIMED_VIEWER = viewerBounds(8.1, 8.0, 80.0);
    client::PlayerPreviewLod lod;
    auto reference = populate(lod, INITIAL_CENTER, VIEWER, PROJECTION);
    auto const primed = lod.refresh(INITIAL_CENTER, PRIMED_VIEWER, PROJECTION);
    expectReference(lod, reference, INITIAL_CENTER, PRIMED_VIEWER, PROJECTION, primed);

    auto const first = lod.refresh({ INITIAL_CENTER.x + 1, INITIAL_CENTER.y }, PRIMED_VIEWER, PROJECTION);
    expectReference(lod, reference, { INITIAL_CENTER.x + 1, INITIAL_CENTER.y }, PRIMED_VIEWER, PROJECTION, first);
    EXPECT_LT(first.visited_buckets, shared::HEIGHT_TILE_INTEREST_COUNT / 32U);
    EXPECT_LT(first.classified_buckets, shared::HEIGHT_TILE_INTEREST_COUNT / 100U);
    RecordProperty("one_tile_crossing_classified_buckets", first.classified_buckets);

    auto const second = lod.refresh({ INITIAL_CENTER.x + 2, INITIAL_CENTER.y }, PRIMED_VIEWER, PROJECTION);
    expectReference(lod, reference, { INITIAL_CENTER.x + 2, INITIAL_CENTER.y }, PRIMED_VIEWER, PROJECTION, second);
    EXPECT_LT(second.visited_buckets, shared::HEIGHT_TILE_INTEREST_COUNT / 32U);
    EXPECT_GT(second.classified_buckets, 0U);
    RecordProperty("expired_certificate_classified_buckets", second.classified_buckets);

    for (int32_t step = 3; step <= 8; ++step) {
        shared::HeightTileKey const center{ INITIAL_CENTER.x + step, INITIAL_CENTER.y };
        auto const refreshed = lod.refresh(center, PRIMED_VIEWER, PROJECTION);
        expectReference(lod, reference, center, PRIMED_VIEWER, PROJECTION, refreshed);
        EXPECT_LE(refreshed.optional_buckets_inspected, 64U);
        if (step == 3) {
            EXPECT_LT(refreshed.classified_buckets, shared::HEIGHT_TILE_INTEREST_COUNT / 10U);
            RecordProperty("post_refresh_classified_buckets", refreshed.classified_buckets);
        }
    }
}

TEST(PlayerPreviewLodTest, FineBucketRecertificationIsBoundedAndCoarseRefinementRemainsImmediate)
{
    static constexpr shared::HeightTileKey CENTER{ 2'000, 2'000 };
    static constexpr shared::HeightTileSurfaceProjection HIGH_RESOLUTION{ 70.0, 3'840U, 2'160U };
    static constexpr shared::HeightTileSurfaceProjection LOW_RESOLUTION{ 70.0, 320U, 180U };
    static constexpr shared::HeightTileSurfaceBounds VIEWER{ 8.0, 8.625, 8.0, 8.625, 80.0, 81.75 };
    static constexpr uint16_t HEIGHT = 20U;
    static constexpr uint32_t BUCKET_GRID_SIDE = 17U;
    static constexpr int32_t BUCKET_SPACING = 16;
    static constexpr uint32_t BUCKET_WORK_LIMIT = 64U;
    static constexpr uint32_t MAX_DRAIN_CALLS =
        (BUCKET_GRID_SIDE * BUCKET_GRID_SIDE + BUCKET_WORK_LIMIT - 1U) / BUCKET_WORK_LIMIT + 2U;
    shared::HeightTileSurfaceFaceAreas const FACE_AREAS = faceAreasForVerticalSpan(0U);
    client::PlayerPreviewLod lod;
    static_cast<void>(lod.refresh(CENTER, VIEWER, HIGH_RESOLUTION));

    std::vector<shared::HeightTileKey> keys;
    keys.reserve(static_cast<decltype(keys)::size_type>(BUCKET_GRID_SIDE) *
        BUCKET_GRID_SIDE);
    for (int32_t y = -8; y <= 8; ++y) {
        for (int32_t x = -8; x <= 8; ++x) {
            shared::HeightTileKey const key = shared::normalizeHeightTileKey({
                CENTER.x + x * BUCKET_SPACING,
                CENTER.y + y * BUCKET_SPACING,
            });
            keys.push_back(key);
            EXPECT_EQ(lod.selectDetail(key, HEIGHT, HEIGHT, FACE_AREAS),
                shared::HeightTileSurfaceDetail::Fine);
        }
    }
    auto const initialized = lod.refresh(CENTER, VIEWER, HIGH_RESOLUTION);
    EXPECT_EQ(initialized.classified_buckets, static_cast<uint32_t>(keys.size()));
    EXPECT_LE(initialized.optional_buckets_inspected, BUCKET_WORK_LIMIT);

    shared::HeightTileSurfaceLodPolicy const low_resolution_policy{ LOW_RESOLUTION };
    KeySet expected_low_resolution_coarsening;
    for (shared::HeightTileKey const key : keys) {
        shared::HeightTileSurfaceBounds const surface = shared::HeightTileSurfaceMesher::boundsForTile(
            { CENTER.x, CENTER.y }, { key.x, key.y }, HEIGHT, HEIGHT
        );
        if (low_resolution_policy.detailFor(VIEWER, surface, FACE_AREAS,
                shared::HeightTileSurfaceDetail::Fine) != shared::HeightTileSurfaceDetail::Fine) {
            expected_low_resolution_coarsening.emplace(key.x, key.y);
        }
    }
    ASSERT_GT(expected_low_resolution_coarsening.size(), BUCKET_WORK_LIMIT);

    auto const first_low_resolution = lod.refresh(CENTER, VIEWER, LOW_RESOLUTION);
    ASSERT_TRUE(first_low_resolution.full_scan);
    EXPECT_LE(first_low_resolution.classified_buckets, BUCKET_WORK_LIMIT);
    EXPECT_LE(first_low_resolution.visited_buckets, BUCKET_WORK_LIMIT);
    EXPECT_EQ(first_low_resolution.optional_buckets_inspected, BUCKET_WORK_LIMIT);
    KeySet const first_coarsened_keys = keySet(first_low_resolution.changed_keys);
    EXPECT_FALSE(first_coarsened_keys.empty());
    EXPECT_LT(first_coarsened_keys.size(), expected_low_resolution_coarsening.size());

    std::optional<shared::HeightTileKey> deferred_key;
    for (shared::HeightTileKey const key : keys) {
        if (expected_low_resolution_coarsening.contains({ key.x, key.y })
            && lod.detail(key) == std::optional{shared::HeightTileSurfaceDetail::Fine}
            && !first_coarsened_keys.contains({ key.x, key.y })) {
            deferred_key = key;
            break;
        }
    }
    ASSERT_TRUE(deferred_key.has_value());
    lod.erase(*deferred_key);
    std::erase(keys, *deferred_key);
    expected_low_resolution_coarsening.erase({ deferred_key->x, deferred_key->y });

    // A projection change must refine every tile coarsened above, even with fine-only work pending.
    auto const restored_high_resolution = lod.refresh(CENTER, VIEWER, HIGH_RESOLUTION);
    ASSERT_TRUE(restored_high_resolution.full_scan);
    KeySet const restored_keys = keySet(restored_high_resolution.changed_keys);
    EXPECT_EQ(restored_keys, first_coarsened_keys);
    for (auto const& [x, y] : first_coarsened_keys) {
        EXPECT_TRUE(restored_keys.contains({ x, y })) << x << ',' << y;
        EXPECT_EQ(lod.detail({ x, y }), std::optional{shared::HeightTileSurfaceDetail::Fine});
    }

    uint32_t stationary_drain_calls = 0U;
    uint32_t prior_changed_buckets = static_cast<uint32_t>(restored_high_resolution.changed_keys.size());
    while (true) {
        auto const drained = lod.refresh(CENTER, VIEWER, HIGH_RESOLUTION);
        EXPECT_LE(drained.classified_buckets, BUCKET_WORK_LIMIT + prior_changed_buckets);
        EXPECT_LE(drained.optional_buckets_inspected, BUCKET_WORK_LIMIT);
        if (drained.classified_buckets == 0U) {
            EXPECT_TRUE(drained.changed_keys.empty());
            break;
        }
        prior_changed_buckets = static_cast<uint32_t>(drained.changed_keys.size());
        ASSERT_LT(++stationary_drain_calls, MAX_DRAIN_CALLS);
    }
    for (shared::HeightTileKey const key : keys) {
        EXPECT_EQ(lod.detail(key), std::optional{shared::HeightTileSurfaceDetail::Fine});
    }

    // Return to the lower projection and let stationary refreshes finish optional coarsening.
    auto const second_low_resolution = lod.refresh(CENTER, VIEWER, LOW_RESOLUTION);
    EXPECT_LE(second_low_resolution.classified_buckets, BUCKET_WORK_LIMIT);
    EXPECT_LE(second_low_resolution.optional_buckets_inspected, BUCKET_WORK_LIMIT);
    KeySet coarsened_keys = keySet(second_low_resolution.changed_keys);
    prior_changed_buckets = static_cast<uint32_t>(second_low_resolution.changed_keys.size());
    for (uint32_t attempt = 0U; attempt < MAX_DRAIN_CALLS; ++attempt) {
        auto const drained = lod.refresh(CENTER, VIEWER, LOW_RESOLUTION);
        EXPECT_LE(drained.classified_buckets, BUCKET_WORK_LIMIT + prior_changed_buckets);
        EXPECT_LE(drained.optional_buckets_inspected, BUCKET_WORK_LIMIT);
        coarsened_keys.merge(keySet(drained.changed_keys));
        if (drained.classified_buckets == 0U) {
            break;
        }
        prior_changed_buckets = static_cast<uint32_t>(drained.changed_keys.size());
        ASSERT_LT(attempt + 1U, MAX_DRAIN_CALLS);
    }
    EXPECT_EQ(coarsened_keys, expected_low_resolution_coarsening);
    for (shared::HeightTileKey const key : keys) {
        EXPECT_EQ(lod.detail(key), low_resolution_policy.detailFor(
            VIEWER,
            shared::HeightTileSurfaceMesher::boundsForTile({ CENTER.x, CENTER.y }, { key.x, key.y }, HEIGHT, HEIGHT),
            FACE_AREAS,
            shared::HeightTileSurfaceDetail::Fine
        ));
    }

    // Clearing an active optional sweep drops its work along with the resident tiles.
    auto const final_refinement = lod.refresh(CENTER, VIEWER, HIGH_RESOLUTION);
    EXPECT_EQ(keySet(final_refinement.changed_keys), expected_low_resolution_coarsening);
    lod.clear();
    auto const cleared = lod.refresh(CENTER, VIEWER, HIGH_RESOLUTION);
    EXPECT_EQ(cleared.visited_buckets, 0U);
    EXPECT_EQ(cleared.classified_buckets, 0U);
    EXPECT_EQ(cleared.optional_buckets_inspected, 0U);
    EXPECT_TRUE(cleared.changed_keys.empty());
}

TEST(PlayerPreviewLodTest, ResidentCoordinateHistogramsMatchExactWrappedImageOracle)
{
    static constexpr int32_t TILE_EXTENT = static_cast<int32_t>(
        shared::WorldExtent::WIDTH / shared::HEIGHT_TILE_SIDE_LENGTH
    );
    static constexpr int32_t HALF_EXTENT = TILE_EXTENT / 2;
    static constexpr std::array<int32_t, 4U> CENTERS{ 0, 1, HALF_EXTENT, TILE_EXTENT - 1 };
    static constexpr std::array<int32_t, 9U> COORDINATES{
        0, 1, HALF_EXTENT - 1, HALF_EXTENT, HALF_EXTENT + 1,
        TILE_EXTENT - 2, TILE_EXTENT - 1, 17, TILE_EXTENT - 19,
    };
    static constexpr shared::HeightTileSurfaceBounds VIEWER{ 8.0, 8.625, 8.0, 8.625, 80.0, 81.75 };
    static constexpr shared::HeightTileSurfaceProjection PROJECTION{};
    static constexpr uint16_t HEIGHT = 20U;
    shared::HeightTileSurfaceFaceAreas const FACE_AREAS = faceAreasForVerticalSpan(0U);

    auto const shortest = [](int32_t const from, int32_t const to) {
        int64_t displacement = static_cast<int64_t>(to) - from;
        if (displacement > HALF_EXTENT) {
            displacement -= TILE_EXTENT;
        } else if (displacement < -HALF_EXTENT) {
            displacement += TILE_EXTENT;
        }
        return displacement;
    };
    for (int32_t const old_x : CENTERS) {
        for (int32_t const old_y : CENTERS) {
            for (int32_t const direction : { -1, 1 }) {
                shared::HeightTileKey const old_center{ old_x, old_y };
                shared::HeightTileKey const new_center = shared::normalizeHeightTileKey({
                    old_x + direction,
                    old_y - direction,
                });
                client::PlayerPreviewLod lod;
                static_cast<void>(lod.refresh(old_center, VIEWER, PROJECTION));
                std::vector<shared::HeightTileKey> residents;
                for (int32_t const x : COORDINATES) {
                    for (int32_t const y : COORDINATES) {
                        shared::HeightTileKey const key{ x, y };
                        static_cast<void>(lod.selectDetail(key, HEIGHT, HEIGHT, FACE_AREAS));
                        residents.push_back(key);
                    }
                }
                static_cast<void>(lod.refresh(old_center, VIEWER, PROJECTION));

                std::vector<shared::HeightTileKey> removals;
                for (uint32_t index = 0U; index < residents.size(); ++index) {
                    if (index % 3U == 0U) {
                        removals.push_back(residents[index]);
                    }
                }
                lod.eraseBatch(removals);
                std::erase_if(residents, [&removals](shared::HeightTileKey const resident) {
                    return std::ranges::find(removals, resident) != removals.end();
                });
                static_cast<void>(lod.refresh(old_center, VIEWER, PROJECTION));

                int64_t const shift_x = -shortest(old_center.x, new_center.x)
                    * static_cast<int64_t>(shared::HEIGHT_TILE_SIDE_LENGTH);
                int64_t const shift_y = -shortest(old_center.y, new_center.y)
                    * static_cast<int64_t>(shared::HEIGHT_TILE_SIDE_LENGTH);
                bool exact_translation = true;
                for (shared::HeightTileKey const key : residents) {
                    shared::HeightTileSurfaceBounds const old_bounds = shared::HeightTileSurfaceMesher::boundsForTile(
                        { old_center.x, old_center.y }, { key.x, key.y }, 0.0, 0.0
                    );
                    shared::HeightTileSurfaceBounds const new_bounds = shared::HeightTileSurfaceMesher::boundsForTile(
                        { new_center.x, new_center.y }, { key.x, key.y }, 0.0, 0.0
                    );
                    if (new_bounds.min_x_blocks - old_bounds.min_x_blocks != static_cast<double>(shift_x)
                        || new_bounds.min_y_blocks - old_bounds.min_y_blocks != static_cast<double>(shift_y)) {
                        exact_translation = false;
                        break;
                    }
                }
                auto const moved = lod.refresh(new_center, VIEWER, PROJECTION);
                EXPECT_EQ(moved.full_scan, !exact_translation)
                    << "old center " << old_x << ',' << old_y << " direction " << direction;
            }
        }
    }
}

TEST(PlayerPreviewLodTest, FineCursorSurvivesEraseAndResidentMapGrowth)
{
    static constexpr shared::HeightTileKey CENTER{ 2'000, 2'000 };
    static constexpr shared::HeightTileSurfaceBounds VIEWER{ 8.0, 8.625, 8.0, 8.625, 80.0, 81.75 };
    static constexpr shared::HeightTileSurfaceProjection HIGH_RESOLUTION{ 70.0, 3'840U, 2'160U };
    static constexpr shared::HeightTileSurfaceProjection LOW_RESOLUTION{ 70.0, 320U, 180U };
    static constexpr uint32_t INITIAL_GRID_SIDE = 9U;
    static constexpr uint32_t GROWTH_GRID_SIDE = 20U;
    static constexpr uint32_t OPTIONAL_LIMIT = 64U;
    static constexpr uint16_t HEIGHT = 20U;
    static constexpr uint32_t MAX_DRAIN_CALLS =
        (INITIAL_GRID_SIDE * INITIAL_GRID_SIDE + GROWTH_GRID_SIDE * GROWTH_GRID_SIDE) / OPTIONAL_LIMIT + 2U;
    shared::HeightTileSurfaceFaceAreas const FACE_AREAS = faceAreasForVerticalSpan(0U);
    client::PlayerPreviewLod lod;
    static_cast<void>(lod.refresh(CENTER, VIEWER, HIGH_RESOLUTION));

    std::vector<ReferenceTile> reference;
    reference.reserve(INITIAL_GRID_SIDE * INITIAL_GRID_SIDE + GROWTH_GRID_SIDE * GROWTH_GRID_SIDE);
    for (int32_t y = -4; y <= 4; ++y) {
        for (int32_t x = -4; x <= 4; ++x) {
            shared::HeightTileKey const key{ CENTER.x + x * 4, CENTER.y + y * 4 };
            shared::HeightTileSurfaceDetail const selected = lod.selectDetail(key, HEIGHT, HEIGHT, FACE_AREAS);
            ASSERT_EQ(selected, shared::HeightTileSurfaceDetail::Fine);
            reference.push_back({ key, HEIGHT, HEIGHT, FACE_AREAS, selected });
        }
    }
    auto const initial_residency = lod.refresh(CENTER, VIEWER, HIGH_RESOLUTION);
    expectReference(lod, reference, CENTER, VIEWER, HIGH_RESOLUTION, initial_residency);

    auto const first_low_resolution = lod.refresh(CENTER, VIEWER, LOW_RESOLUTION);
    EXPECT_EQ(first_low_resolution.optional_buckets_inspected, OPTIONAL_LIMIT);
    expectReference(lod, reference, CENTER, VIEWER, LOW_RESOLUTION, first_low_resolution);

    shared::HeightTileKey const erased_key = reference.at(OPTIONAL_LIMIT - 1U).key;
    lod.erase(erased_key);
    std::erase_if(reference, [erased_key](ReferenceTile const& tile) { return tile.key == erased_key; });

    for (uint32_t index = 0U; index < GROWTH_GRID_SIDE * GROWTH_GRID_SIDE; ++index) {
        int32_t const x_offset = 32 + static_cast<int32_t>(index % GROWTH_GRID_SIDE) * 4;
        int32_t const y_offset = -160 + static_cast<int32_t>(index / GROWTH_GRID_SIDE) * 4;
        shared::HeightTileKey const key{ CENTER.x + x_offset, CENTER.y + y_offset };
        shared::HeightTileSurfaceDetail const selected = lod.selectDetail(key, HEIGHT, HEIGHT, FACE_AREAS);
        reference.push_back({ key, HEIGHT, HEIGHT, FACE_AREAS, selected });
    }
    auto const after_growth = lod.refresh(CENTER, VIEWER, LOW_RESOLUTION);
    EXPECT_LE(after_growth.optional_buckets_inspected, OPTIONAL_LIMIT);
    expectReference(lod, reference, CENTER, VIEWER, LOW_RESOLUTION, after_growth);

    bool reached_idle = false;
    for (uint32_t attempt = 0U; attempt < MAX_DRAIN_CALLS; ++attempt) {
        auto const drained = lod.refresh(CENTER, VIEWER, LOW_RESOLUTION);
        EXPECT_LE(drained.optional_buckets_inspected, OPTIONAL_LIMIT);
        expectReference(lod, reference, CENTER, VIEWER, LOW_RESOLUTION, drained);
        if (drained.visited_buckets == 0U) {
            reached_idle = true;
            break;
        }
    }
    EXPECT_TRUE(reached_idle);
}

TEST(PlayerPreviewLodTest, CanonicalR256SevenTileChangesRecordFullScanWorkAndPreserveExactPolicy)
{
    static constexpr int32_t TILE_EXTENT = static_cast<int32_t>(
        shared::WorldExtent::WIDTH / shared::HEIGHT_TILE_SIDE_LENGTH
    );
    static constexpr shared::HeightTileKey INITIAL_CENTER{ 2'056, 2'048 };
    static constexpr shared::HeightTileSurfaceProjection PROJECTION{};
    static constexpr shared::HeightTileSurfaceBounds VIEWER{ 8.0, 8.625, 8.0, 8.625, 80.0, 81.75 };
    static constexpr shared::HeightTileSurfaceBounds PRIMED_VIEWER{ 8.1, 8.725, 8.0, 8.625, 80.0, 81.75 };
    static constexpr uint32_t CENTER_CHANGES = 59U;
    static constexpr uint32_t CENTER_MOVE_TILES = 7U;
    static constexpr uint64_t BASELINE_POLICY_EVALUATIONS = 3'600'275U;
    shared::TerrainGenerator terrain;
    ASSERT_TRUE(terrain.usingCoreLang());
    client::PlayerPreviewLod lod;
    std::vector<ReferenceTile> reference = populateCanonicalRelief(
        lod, terrain, INITIAL_CENTER, VIEWER, PROJECTION
    );
    ASSERT_EQ(reference.size(), shared::HEIGHT_TILE_INTEREST_COUNT);
    auto const lowest = std::ranges::min_element(reference, {}, &ReferenceTile::minimum);
    auto const highest = std::ranges::max_element(reference, {}, &ReferenceTile::maximum);
    ASSERT_NE(lowest, reference.end());
    ASSERT_NE(highest, reference.end());
    EXPECT_GT(static_cast<uint32_t>(highest->maximum) - lowest->minimum, 700U);

    auto const primed = lod.refresh(INITIAL_CENTER, PRIMED_VIEWER, PROJECTION);
    expectReference(lod, reference, INITIAL_CENTER, PRIMED_VIEWER, PROJECTION, primed);
    EXPECT_FALSE(primed.full_scan);
    EXPECT_LE(primed.optional_buckets_inspected, 64U);

    uint32_t full_scan_count = 0U;
    uint32_t maximum_visited_buckets = 0U;
    uint32_t maximum_classified_buckets = 0U;
    uint32_t maximum_evaluated_tiles = 0U;
    uint32_t maximum_non_full_visited_buckets = 0U;
    uint32_t maximum_non_full_classified_buckets = 0U;
    uint32_t maximum_non_full_evaluated_tiles = 0U;
    uint64_t total_visited_buckets = 0U;
    uint64_t total_classified_buckets = 0U;
    uint64_t total_evaluated_tiles = 0U;
    uint64_t total_policy_evaluations = 0U;
    uint64_t full_scan_classified_buckets = 0U;
    uint64_t full_scan_reused_buckets = 0U;
    uint64_t full_scan_evaluated_tiles = 0U;
    shared::HeightTileKey current_center = INITIAL_CENTER;
    for (uint32_t step = 1U; step <= CENTER_CHANGES; ++step) {
        shared::HeightTileKey const next_center{
            (INITIAL_CENTER.x + static_cast<int32_t>(step * CENTER_MOVE_TILES)) % TILE_EXTENT,
            INITIAL_CENTER.y,
        };
        client::PlayerPreviewLodRefresh const refreshed = advanceCanonicalRelief(
            lod, terrain, reference, current_center, next_center, PRIMED_VIEWER, PROJECTION
        );
        expectReference(lod, reference, next_center, PRIMED_VIEWER, PROJECTION, refreshed);
        full_scan_count += refreshed.full_scan ? 1U : 0U;
        maximum_visited_buckets = std::max(maximum_visited_buckets, refreshed.visited_buckets);
        maximum_classified_buckets = std::max(maximum_classified_buckets, refreshed.classified_buckets);
        maximum_evaluated_tiles = std::max(maximum_evaluated_tiles, refreshed.evaluated_tiles);
        total_visited_buckets += refreshed.visited_buckets;
        total_classified_buckets += refreshed.classified_buckets;
        total_evaluated_tiles += refreshed.evaluated_tiles;
        total_policy_evaluations += refreshed.policy_evaluations;
        if (refreshed.full_scan) {
            full_scan_classified_buckets += refreshed.classified_buckets;
            full_scan_reused_buckets += refreshed.visited_buckets - refreshed.classified_buckets;
            full_scan_evaluated_tiles += refreshed.evaluated_tiles;
        } else {
            maximum_non_full_visited_buckets = std::max(
                maximum_non_full_visited_buckets, refreshed.visited_buckets);
            maximum_non_full_classified_buckets = std::max(
                maximum_non_full_classified_buckets, refreshed.classified_buckets);
            maximum_non_full_evaluated_tiles = std::max(
                maximum_non_full_evaluated_tiles, refreshed.evaluated_tiles);
        }
        EXPECT_EQ(reference.size(), shared::HEIGHT_TILE_INTEREST_COUNT);
        current_center = next_center;
    }
    EXPECT_LT(total_policy_evaluations, BASELINE_POLICY_EVALUATIONS * 9U / 10U);
    RecordProperty("canonical_r256_center_changes", CENTER_CHANGES);
    RecordProperty("canonical_r256_full_scan_count", full_scan_count);
    RecordProperty("canonical_r256_maximum_visited_buckets", maximum_visited_buckets);
    RecordProperty("canonical_r256_maximum_classified_buckets", maximum_classified_buckets);
    RecordProperty("canonical_r256_maximum_evaluated_tiles", maximum_evaluated_tiles);
    RecordProperty("canonical_r256_maximum_non_full_visited_buckets", maximum_non_full_visited_buckets);
    RecordProperty("canonical_r256_maximum_non_full_classified_buckets", maximum_non_full_classified_buckets);
    RecordProperty("canonical_r256_maximum_non_full_evaluated_tiles", maximum_non_full_evaluated_tiles);
    RecordProperty("canonical_r256_total_visited_buckets", total_visited_buckets);
    RecordProperty("canonical_r256_total_classified_buckets", total_classified_buckets);
    RecordProperty("canonical_r256_total_evaluated_tiles", total_evaluated_tiles);
    RecordProperty("canonical_r256_total_policy_evaluations", total_policy_evaluations);
    RecordProperty("canonical_r256_full_scan_classified_buckets", full_scan_classified_buckets);
    RecordProperty("canonical_r256_full_scan_reused_buckets", full_scan_reused_buckets);
    RecordProperty("canonical_r256_full_scan_evaluated_tiles", full_scan_evaluated_tiles);
}

TEST(PlayerPreviewLodTest, CanonicalR256SevenTileChangesPreserveExactPolicyAcrossWorldWrap)
{
    static constexpr int32_t TILE_EXTENT = static_cast<int32_t>(
        shared::WorldExtent::WIDTH / shared::HEIGHT_TILE_SIDE_LENGTH
    );
    static constexpr shared::HeightTileKey INITIAL_CENTER{ TILE_EXTENT - 14, 2'048 };
    static constexpr shared::HeightTileSurfaceProjection PROJECTION{};
    shared::HeightTileSurfaceBounds const VIEWER = viewerBounds(8.0, 8.0, 80.0);
    shared::HeightTileSurfaceBounds const PRIMED_VIEWER = viewerBounds(8.1, 8.0, 80.0);
    static constexpr uint32_t CENTER_CHANGES = 3U;
    static constexpr uint32_t CENTER_MOVE_TILES = 7U;
    shared::TerrainGenerator terrain;
    ASSERT_TRUE(terrain.usingCoreLang());
    client::PlayerPreviewLod lod;
    auto reference = populateCanonicalRelief(lod, terrain, INITIAL_CENTER, VIEWER, PROJECTION);
    auto const primed = lod.refresh(INITIAL_CENTER, PRIMED_VIEWER, PROJECTION);
    expectReference(lod, reference, INITIAL_CENTER, PRIMED_VIEWER, PROJECTION, primed);

    shared::HeightTileKey current_center = INITIAL_CENTER;
    for (uint32_t step = 1U; step <= CENTER_CHANGES; ++step) {
        shared::HeightTileKey const next_center{
            (INITIAL_CENTER.x + static_cast<int32_t>(step * CENTER_MOVE_TILES)) % TILE_EXTENT,
            INITIAL_CENTER.y,
        };
        auto const refreshed = advanceCanonicalRelief(
            lod, terrain, reference, current_center, next_center, PRIMED_VIEWER, PROJECTION
        );
        expectReference(lod, reference, next_center, PRIMED_VIEWER, PROJECTION, refreshed);
        EXPECT_EQ(reference.size(), shared::HEIGHT_TILE_INTEREST_COUNT);
        current_center = next_center;
    }
}

TEST(PlayerPreviewLodTest, PositionInterestUsesTheExactWrappedDisk)
{
    static constexpr int32_t TILE_EXTENT = static_cast<int32_t>(
        shared::WorldExtent::WIDTH / shared::HEIGHT_TILE_SIDE_LENGTH
    );
    static constexpr uint32_t RADIUS = 2U;
    static constexpr shared::HeightTileKey CENTER{ 0, 0 };

    EXPECT_TRUE(client::playerPreviewTileWithinInterest(CENTER, CENTER, RADIUS));
    EXPECT_TRUE(client::playerPreviewTileWithinInterest(CENTER, { 2, 0 }, RADIUS));
    EXPECT_TRUE(client::playerPreviewTileWithinInterest(CENTER, { TILE_EXTENT - 2, 0 }, RADIUS));
    EXPECT_TRUE(client::playerPreviewTileWithinInterest(CENTER, { TILE_EXTENT - 1, TILE_EXTENT - 1 }, RADIUS));
    EXPECT_FALSE(client::playerPreviewTileWithinInterest(CENTER, { 2, 1 }, RADIUS));
    EXPECT_FALSE(client::playerPreviewTileWithinInterest(CENTER, { TILE_EXTENT - 2, TILE_EXTENT - 1 }, RADIUS));
    EXPECT_FALSE(client::playerPreviewTileWithinInterest(CENTER, { 0, 3 }, RADIUS));
}

TEST(PlayerPreviewLodTest, BatchEraseRemovesSelectedTilesAcrossBuckets)
{
    static constexpr shared::HeightTileKey CENTER{ 1'000, 1'000 };
    static constexpr shared::HeightTileSurfaceProjection PROJECTION{};
    static constexpr std::array<shared::HeightTileKey, 5> TILES{
        shared::HeightTileKey{ 10, 20 },
        shared::HeightTileKey{ 11, 21 },
        shared::HeightTileKey{ 13, 21 },
        shared::HeightTileKey{ 14, 20 },
        shared::HeightTileKey{ 18, 20 },
    };
    static constexpr std::array<shared::HeightTileKey, 4> ERASED_TILES{
        shared::HeightTileKey{ 10, 20 },
        shared::HeightTileKey{ 13, 21 },
        shared::HeightTileKey{ 14, 20 },
        shared::HeightTileKey{ 10, 20 },
    };
    client::PlayerPreviewLod lod;
    static_cast<void>(lod.refresh(CENTER, viewerBounds(8.0, 8.0, 80.0), PROJECTION));
    for (shared::HeightTileKey const key : TILES) {
        static_cast<void>(lod.selectDetail(key, 20U, 24U));
    }

    lod.eraseBatch(ERASED_TILES);

    EXPECT_FALSE(lod.detail(TILES[0]).has_value());
    ASSERT_TRUE(lod.detail(TILES[1]).has_value());
    EXPECT_FALSE(lod.detail(TILES[2]).has_value());
    EXPECT_FALSE(lod.detail(TILES[3]).has_value());
    ASSERT_TRUE(lod.detail(TILES[4]).has_value());
    auto const refreshed = lod.refresh(CENTER, viewerBounds(8.2, 8.0, 80.0), PROJECTION);
    EXPECT_TRUE(std::ranges::none_of(refreshed.changed_keys, [](shared::HeightTileKey const key) {
        return std::ranges::find(ERASED_TILES, key) != ERASED_TILES.end();
    }));
}

TEST(PlayerPreviewLodTest, RemeshClosureIncludesTheNormalizedCardinalNeighborhood)
{
    static constexpr std::array<shared::HeightTileKey, 1> CHANGED_KEYS{
        shared::HeightTileKey{ 10, 20 },
    };
    std::vector<shared::HeightTileKey> const expected{
        { 10, 19 },
        { 9, 20 },
        { 10, 20 },
        { 11, 20 },
        { 10, 21 },
    };

    EXPECT_EQ(client::playerPreviewLodRemeshClosure(CHANGED_KEYS), expected);
}

TEST(PlayerPreviewLodTest, RemeshClosureDeduplicatesAdjacentAndRepeatedChangedKeys)
{
    static constexpr std::array<shared::HeightTileKey, 2> ADJACENT_KEYS{
        shared::HeightTileKey{ 10, 20 },
        shared::HeightTileKey{ 11, 20 },
    };
    static constexpr std::array<shared::HeightTileKey, 2> REPEATED_KEYS{
        shared::HeightTileKey{ 10, 20 },
        shared::HeightTileKey{ 10, 20 },
    };
    std::vector<shared::HeightTileKey> const adjacent_expected{
        { 10, 19 },
        { 11, 19 },
        { 9, 20 },
        { 10, 20 },
        { 11, 20 },
        { 12, 20 },
        { 10, 21 },
        { 11, 21 },
    };
    std::vector<shared::HeightTileKey> const repeated_expected{
        { 10, 19 },
        { 9, 20 },
        { 10, 20 },
        { 11, 20 },
        { 10, 21 },
    };

    EXPECT_EQ(client::playerPreviewLodRemeshClosure(ADJACENT_KEYS), adjacent_expected);
    EXPECT_EQ(client::playerPreviewLodRemeshClosure(REPEATED_KEYS), repeated_expected);
}

TEST(PlayerPreviewLodTest, RemeshClosureWrapsAcrossZeroAndLastTile)
{
    static constexpr std::array<shared::HeightTileKey, 2> CHANGED_KEYS{
        shared::HeightTileKey{ 0, 0 },
        shared::HeightTileKey{ 4'095, 4'095 },
    };
    std::vector<shared::HeightTileKey> const expected{
        { 0, 0 },
        { 1, 0 },
        { 4'095, 0 },
        { 0, 1 },
        { 4'095, 4'094 },
        { 0, 4'095 },
        { 4'094, 4'095 },
        { 4'095, 4'095 },
    };

    EXPECT_EQ(client::playerPreviewLodRemeshClosure(CHANGED_KEYS), expected);
}

TEST(PlayerPreviewLodTest, RemeshClosureOutputOverloadRetainsPreallocatedStorage)
{
    static constexpr uint32_t OUTPUT_CAPACITY = 20U;
    static constexpr std::array<shared::HeightTileKey, 4U> CHANGED_KEYS{
        shared::HeightTileKey{ .x = 10, .y = 20 },
        shared::HeightTileKey{ .x = 11, .y = 20 },
        shared::HeightTileKey{ .x = 0, .y = 0 },
        shared::HeightTileKey{ .x = 4'095, .y = 4'095 },
    };
    static constexpr std::array<shared::HeightTileKey, 2U> SECOND_CHANGED_KEYS{
        shared::HeightTileKey{ .x = 100, .y = 200 },
        shared::HeightTileKey{ .x = 101, .y = 200 },
    };
    std::vector<shared::HeightTileKey> output;
    output.reserve(OUTPUT_CAPACITY);
    shared::HeightTileKey const* const storage = output.data();
    uint32_t const capacity = static_cast<uint32_t>(output.capacity());
    auto const check_reuse = [&](std::span<shared::HeightTileKey const> changed_keys) {
        std::vector<shared::HeightTileKey> const expected = client::playerPreviewLodRemeshClosure(changed_keys);
        client::playerPreviewLodRemeshClosure(changed_keys, output);
        EXPECT_EQ(output, expected);
        EXPECT_EQ(output.data(), storage);
        EXPECT_EQ(output.capacity(), capacity);
    };

    check_reuse(CHANGED_KEYS);
    check_reuse(SECOND_CHANGED_KEYS);
    check_reuse(std::span<shared::HeightTileKey const>{});
    check_reuse(CHANGED_KEYS);
}

TEST(PlayerPreviewLodTest, SeededMovementAndX200StepsMatchFullPolicyAcrossWrap)
{
    static constexpr int32_t TILE_EXTENT = shared::WorldExtent::WIDTH / shared::HEIGHT_TILE_SIDE_LENGTH;
    static constexpr shared::HeightTileKey INITIAL_CENTER{ TILE_EXTENT - 4, 2'000 };
    static constexpr shared::HeightTileSurfaceProjection PROJECTION{};
    static constexpr uint32_t STEPS = 24U;
    static constexpr double HALF_STEPS = static_cast<double>(STEPS) / 2.0;
    static constexpr double X200_STEP = static_cast<double>(shared::MOVEMENT_SUBCELLS_PER_TICK) * 200.0
        / shared::SUBCELLS_PER_CELL;
    client::PlayerPreviewLod lod;
    auto reference = populate(lod, INITIAL_CENTER, viewerBounds(8.0, 8.0, 80.0), PROJECTION);
    for (uint32_t step = 1U; step <= STEPS; ++step) {
        double const step_as_double = static_cast<double>(step);
        double const displacement = step_as_double <= HALF_STEPS
            ? step_as_double * 0.56
            : (step_as_double - HALF_STEPS) * X200_STEP;
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

TEST(PlayerPreviewLodTest, CandidateAreaExtremaMatchExhaustiveSelectionAcrossMovementAndUpdates)
{
    static constexpr shared::HeightTileKey INITIAL_CENTER{ 4'095, 0 };
    static constexpr uint32_t TILE_COUNT = 64U;
    static constexpr uint32_t STEPS = 12U;
    static constexpr uint16_t MINIMUM = 4U;
    static constexpr uint16_t MAXIMUM = 1'024U;
    static constexpr std::array<shared::HeightTileSurfaceProjection, 3> PROJECTIONS{
        shared::HeightTileSurfaceProjection{ 100.0, 640U, 360U },
        shared::HeightTileSurfaceProjection{ 70.0, 2'560U, 1'440U },
        shared::HeightTileSurfaceProjection{ 40.0, 3'840U, 2'160U },
    };
    struct Tile final {
        shared::HeightTileKey key;
        shared::HeightTileSurfaceFaceAreas areas;
        shared::HeightTileSurfaceDetail detail;
    };
    client::PlayerPreviewLod lod;
    auto const initial_viewer = viewerBounds(8.0, 8.0, 80.0);
    static_cast<void>(lod.refresh(INITIAL_CENTER, initial_viewer, PROJECTIONS.front()));
    std::vector<Tile> tiles;
    for (uint32_t index = 0U; index < TILE_COUNT; ++index) {
        auto const key = shared::normalizeHeightTileKey({
            .x = INITIAL_CENTER.x + 150 + static_cast<int32_t>(index % 8U),
            .y = 140 + static_cast<int32_t>(index / 8U),
        });
        shared::HeightTileSurfaceFaceAreas const areas{
            .maximum = { index % 3U == 0U ? 2'048.0 : 4.0, 16.0,
                index % 5U == 0U ? 8'192.0 : 64.0, 256.0 },
        };
        tiles.push_back({ key, areas, lod.selectDetail(key, MINIMUM, MAXIMUM, areas) });
    }
    for (uint32_t step = 0U; step < STEPS; ++step) {
        auto const center = shared::normalizeHeightTileKey({
            INITIAL_CENTER.x + static_cast<int32_t>(step / 3U), static_cast<int32_t>(step / 4U),
        });
        auto const viewer = viewerBounds(8.0 + (step % 3U) * 0.25, 8.0, 80.0 + step * 2.0);
        auto const projection = PROJECTIONS[step % static_cast<uint32_t>(PROJECTIONS.size())];
        shared::HeightTileSurfaceLodPolicy const policy{ projection };
        auto const refreshed = lod.refresh(center, viewer, projection);
        KeySet expected_changes;
        for (Tile& tile : tiles) {
            auto const bounds = shared::HeightTileSurfaceMesher::boundsForTile(
                { center.x, center.y }, { tile.key.x, tile.key.y }, MINIMUM, MAXIMUM
            );
            auto const selected = policy.detailFor(viewer, bounds, tile.areas, tile.detail);
            if (selected != tile.detail) {
                expected_changes.emplace(tile.key.x, tile.key.y);
            }
            tile.detail = selected;
            ASSERT_EQ(lod.detail(tile.key), std::optional{selected});
        }
        EXPECT_EQ(keySet(refreshed.changed_keys), expected_changes);
        Tile& changed = tiles[step];
        changed.areas.maximum[0] = changed.areas.maximum[0] == 4.0 ? 2'048.0 : 4.0;
        auto const changed_bounds = shared::HeightTileSurfaceMesher::boundsForTile(
            { center.x, center.y }, { changed.key.x, changed.key.y }, MINIMUM, MAXIMUM
        );
        changed.detail = policy.detailFor(viewer, changed_bounds, changed.areas, changed.detail);
        EXPECT_EQ(lod.selectDetail(changed.key, MINIMUM, MAXIMUM, changed.areas), changed.detail);
    }
}

TEST(PlayerPreviewLodTest, ChangedCandidateAreaRefinesWithIdenticalElevationBounds)
{
    static constexpr shared::HeightTileKey CENTER{};
    static constexpr shared::HeightTileKey DISTANT{ 240, 0 };
    static constexpr shared::HeightTileSurfaceProjection PROJECTION{ 70.0, 2'560U, 1'440U };
    static constexpr uint16_t MINIMUM = 4U;
    static constexpr uint16_t MAXIMUM = 100U;
    static constexpr shared::HeightTileSurfaceFaceAreas SAFE{};
    static constexpr shared::HeightTileSurfaceFaceAreas CLIFF{ .maximum = { 192.0, 384.0, 768.0, 1'536.0 } };
    client::PlayerPreviewLod lod;
    static_cast<void>(lod.refresh(CENTER, viewerBounds(8.0, 8.0, 80.0), PROJECTION));
    EXPECT_EQ(lod.selectDetail(DISTANT, MINIMUM, MAXIMUM, SAFE), shared::HeightTileSurfaceDetail::Coarse2);
    EXPECT_EQ(lod.selectDetail(DISTANT, MINIMUM, MAXIMUM, CLIFF), shared::HeightTileSurfaceDetail::Fine);
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
        auto const retained_orders = shared::prepareHeightTileInterestOrders(radius);
        ASSERT_NE(retained_orders, nullptr);
        for (uint32_t step = 1U; step < CENTERS.size(); ++step) {
            KeySet const previous = keySet(
                shared::makeHeightTileInterest(CENTERS[step - 1U], 0, 127, radius).keys
            );
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
