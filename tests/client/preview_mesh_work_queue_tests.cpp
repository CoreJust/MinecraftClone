#include <client/PreviewMeshWorkQueue.hpp>

#include <client/PreviewResidency.hpp>

#include <shared/net/Message.hpp>

#include <gtest/gtest.h>

#include <testsupport/TerrainFixtures.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>
#include <string>
#include <utility>

namespace {

TEST(PreviewMeshWorkQueueTest, TerrainChangesQueueTheTileAndWrappedNeighborsOnce)
{
    client::PreviewMeshWorkQueue queue;
    queue.enqueueChange({
        .key = { 0, 0 },
        .kind = client::HeightTileChangeKind::Upsert,
    });
    queue.enqueueChange({
        .key = { 0, 0 },
        .kind = client::HeightTileChangeKind::Remove,
    });

    ASSERT_EQ(queue.size(), 5U);
    auto const center = queue.take();
    auto const west = queue.take();
    auto const east = queue.take();
    auto const north = queue.take();
    auto const south = queue.take();
    ASSERT_TRUE(center.has_value());
    ASSERT_TRUE(west.has_value());
    ASSERT_TRUE(east.has_value());
    ASSERT_TRUE(north.has_value());
    ASSERT_TRUE(south.has_value());
    EXPECT_EQ(*center, (client::HeightTileKey{ 0, 0 }));
    EXPECT_EQ(*west, (client::HeightTileKey{ 4'095, 0 }));
    EXPECT_EQ(*east, (client::HeightTileKey{ 1, 0 }));
    EXPECT_EQ(*north, (client::HeightTileKey{ 0, 4'095 }));
    EXPECT_EQ(*south, (client::HeightTileKey{ 0, 1 }));
    EXPECT_TRUE(queue.empty());
}

TEST(PreviewMeshWorkQueueTest, ACompletedKeyCanBeQueuedAgain)
{
    client::PreviewMeshWorkQueue queue;
    queue.enqueue({ 12, 34 });

    auto const first = queue.take();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, (client::HeightTileKey{ 12, 34 }));
    queue.enqueue({ 12, 34 });

    auto const second = queue.take();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*second, (client::HeightTileKey{ 12, 34 }));
    EXPECT_TRUE(queue.empty());
}

TEST(PreviewMeshWorkQueueTest, CompletedFirstPublicationSurvivesAFullWindowOfReplacementReplenishment)
{
    struct Result final {
        client::HeightTileKey key;
        bool missing_current_mesh;
    };
    static constexpr uint32_t WINDOW = 128U;
    static constexpr client::HeightTileKey CENTER{ 2000, 2000 };
    static constexpr client::HeightTileKey MISSING{ 2100, 2100 };
    std::deque<Result> results{ { .key = MISSING, .missing_current_mesh = true } };
    for (uint32_t index = 1U; index < WINDOW; ++index) {
        results.push_back({
            .key = { CENTER.x + 40 + static_cast<int32_t>(index % 8U),
                CENTER.y + static_cast<int32_t>(index / 8U) },
            .missing_current_mesh = false,
        });
    }
    auto const ordinary = [](client::HeightTileKey) { return uint8_t{ 3U }; };
    bool published = false;
    for (uint32_t serviced = 0U; serviced < WINDOW; ++serviced) {
        auto const selected = client::preferredPreviewMeshResultIndex(results, CENTER, 127, 0, ordinary);
        ASSERT_TRUE(selected);
        Result const result = results[*selected];
        results.erase(results.begin() + static_cast<std::deque<Result>::difference_type>(*selected));
        if (result.key == MISSING) {
            published = true;
            break;
        }
        results.push_back(result);
        ASSERT_EQ(results.size(), WINDOW);
    }
    EXPECT_TRUE(published);
}

TEST(PreviewMeshWorkQueueTest, MovingQueuedFirstPublicationsFollowVisibleNearAndForecastUpdates)
{
    struct Work final {
        client::HeightTileKey key;
        bool missing_current_mesh;
    };
    static constexpr uint32_t STEPS = 16U;
    static constexpr client::HeightTileKey INITIAL_CENTER{ 2000, 2000 };
    for (uint32_t step = 0U; step < STEPS; ++step) {
        client::HeightTileKey const center{ INITIAL_CENTER.x + static_cast<int32_t>(step), INITIAL_CENTER.y };
        client::HeightTileKey const near{ center.x + 7, center.y + 7 };
        client::HeightTileKey const forecast{ center.x + 23, center.y + 7 };
        client::HeightTileKey const missing{ center.x + 100, center.y + 100 };
        client::HeightTileKey const replacement{ center.x + 40, center.y };
        std::deque<Work> queued{
            { .key = replacement, .missing_current_mesh = false },
            { .key = missing, .missing_current_mesh = true },
            { .key = forecast, .missing_current_mesh = false },
            { .key = near, .missing_current_mesh = false },
        };
        auto const geographic_tier = [center](client::HeightTileKey const key) {
            if (std::abs(key.x - center.x) <= 7 && std::abs(key.y - center.y) <= 7) {
                return uint8_t{ 0U };
            }
            if (std::abs(key.x - center.x - 16) <= 7 && std::abs(key.y - center.y) <= 7) {
                return uint8_t{ 1U };
            }
            return uint8_t{ 3U };
        };
        std::array const expected{ near, forecast, missing, replacement };
        for (auto const expected_key : expected) {
            auto const selected = client::preferredPreviewMeshResultIndex(
                queued, center, step % 2U == 0U ? 127 : 0, step % 2U == 0U ? 0 : 127, geographic_tier);
            ASSERT_TRUE(selected);
            EXPECT_EQ(queued[*selected].key, expected_key);
            queued.erase(queued.begin() + static_cast<std::deque<Work>::difference_type>(*selected));
        }
        EXPECT_TRUE(queued.empty());
    }
}

TEST(PreviewMeshWorkQueueTest, ProductionDeferralProtectsMissingWorkAndSpatialOverrides)
{
    struct Work final {
        client::HeightTileKey key;
        bool missing_current_mesh;
    };
    std::deque<Work> work{
        { { 0, 0 }, false },
        { { 1, 0 }, false },
        { { 2, 0 }, true },
        { { 3, 0 }, true },
        { { 4, 0 }, false },
        { { 5, 0 }, false },
    };
    auto const geographic = [](client::HeightTileKey const key) {
        return static_cast<uint8_t>(key.x < 2 ? key.x : (key.x % 2 == 0 ? 2 : 3));
    };
    // This is also the production selector used before ownership retirement in either pool.
    for (int32_t const expected : { 5, 4, 3, 2, 1 }) {
        auto const deferred = client::lowestPriorityPreviewMeshAbove(work, geographic, 0U);
        ASSERT_NE(deferred, work.end());
        EXPECT_EQ(deferred->key.x, expected);
        work.erase(deferred);
    }
    EXPECT_EQ(client::lowestPriorityPreviewMeshAbove(work, geographic, 0U), work.end());

    std::array<Work, 2> const ordinary{{ { { 4, 0 }, false }, { { 3, 0 }, true } }};
    auto const first = client::preferredPreviewMeshResultIndex(ordinary, { 0, 0 }, 0, 0, geographic);
    ASSERT_TRUE(first);
    EXPECT_TRUE(ordinary[*first].missing_current_mesh);
}

TEST(PreviewMeshWorkQueueTest, CompletedMeshResultsFollowTheCurrentInterestAfterTravel)
{
    struct Result final {
        client::HeightTileKey key;
        uint32_t sequence;
        bool missing_current_mesh = false;
    };
    static constexpr client::HeightTileKey OLD_CENTER{ 0, 0 };
    static constexpr client::HeightTileKey NEW_CENTER{ 50, 0 };
    std::array<Result, 2> const results{{
        { .key = OLD_CENTER, .sequence = 1U },
        { .key = NEW_CENTER, .sequence = 2U },
    }};

    auto const selected = client::preferredPreviewMeshResultIndex(results, NEW_CENTER, 0, 127);

    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(results[*selected].sequence, 2U);
}

TEST(PreviewMeshWorkQueueTest, CurrentNearSquareOutranksRadiallyCloserOutsideResults)
{
    struct Result final {
        client::HeightTileKey key;
        uint32_t sequence;
        bool missing_current_mesh = false;
    };
    static constexpr client::HeightTileKey CENTER{ 2'000, 2'000 };
    std::array<Result, 2> const results{{
        { .key = { CENTER.x + 8, CENTER.y }, .sequence = 1U },
        { .key = { CENTER.x + 7, CENTER.y + 7 }, .sequence = 2U },
    }};
    auto const is_current_near = [](client::HeightTileKey const key) {
        return static_cast<uint8_t>(std::abs(key.x - CENTER.x) <= 7 && std::abs(key.y - CENTER.y) <= 7 ? 0U : 3U);
    };

    auto const selected = client::preferredPreviewMeshResultIndex(
        results, CENTER, 0, 127, is_current_near
    );

    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(results[*selected].sequence, 2U);
    EXPECT_LT(
        client::previewMeshWorkPriority(CENTER, 0, 127, results[1].key, is_current_near),
        client::previewMeshWorkPriority(CENTER, 0, 127, results[0].key, is_current_near)
    );
}

TEST(PreviewMeshWorkQueueTest, DefersOneNonNearResultWithoutDiscardingItsIdentity)
{
    struct Work final {
        client::HeightTileKey key;
        uint32_t sequence;
    };
    static constexpr client::HeightTileKey CENTER{ 100, 100 };
    std::deque<Work> work{
        { .key = { CENTER.x + 8, CENTER.y }, .sequence = 1U },
        { .key = { CENTER.x + 7, CENTER.y + 7 }, .sequence = 2U },
        { .key = { CENTER.x - 9, CENTER.y }, .sequence = 3U },
    };
    auto const is_current_near = [](client::HeightTileKey const key) {
        return static_cast<uint8_t>(std::abs(key.x - CENTER.x) <= 7 && std::abs(key.y - CENTER.y) <= 7 ? 0U : 3U);
    };

    auto const deferred = client::deferLowerPriorityPreviewMesh(work, is_current_near, 0U);

    ASSERT_TRUE(deferred.has_value());
    EXPECT_EQ(*deferred, (client::HeightTileKey{ CENTER.x + 8, CENTER.y }));
    ASSERT_EQ(work.size(), 2U);
    EXPECT_EQ(work[0].key, (client::HeightTileKey{ CENTER.x + 7, CENTER.y + 7 }));
    EXPECT_EQ(work[1].key, (client::HeightTileKey{ CENTER.x - 9, CENTER.y }));
    auto const deferred_second = client::deferLowerPriorityPreviewMesh(work, is_current_near, 0U);
    ASSERT_TRUE(deferred_second.has_value());
    EXPECT_EQ(*deferred_second, (client::HeightTileKey{ CENTER.x - 9, CENTER.y }));
    ASSERT_EQ(work.size(), 1U);
    EXPECT_EQ(work[0].key, (client::HeightTileKey{ CENTER.x + 7, CENTER.y + 7 }));
    EXPECT_FALSE(client::deferLowerPriorityPreviewMesh(work, is_current_near, 0U).has_value());
}

TEST(PreviewMeshWorkQueueTest, ChangesCanBeFilteredAndOldInterestPruned)
{
    client::PreviewMeshWorkQueue queue;
    queue.enqueueChange({
        .key = { 0, 0 },
        .kind = client::HeightTileChangeKind::Remove,
    }, [](client::HeightTileKey const key) {
        return key == client::HeightTileKey{ 0, 0 } || key == client::HeightTileKey{ 1, 0 };
    });
    queue.enqueue({ 10, 20 });

    queue.retain([](client::HeightTileKey const key) {
        return key == client::HeightTileKey{ 0, 0 } || key == client::HeightTileKey{ 1, 0 };
    });

    ASSERT_EQ(queue.size(), 2U);
    auto const center = queue.take();
    auto const east = queue.take();
    ASSERT_TRUE(center.has_value());
    ASSERT_TRUE(east.has_value());
    EXPECT_EQ(*center, (client::HeightTileKey{ 0, 0 }));
    EXPECT_EQ(*east, (client::HeightTileKey{ 1, 0 }));
    EXPECT_TRUE(queue.empty());
}

TEST(PreviewMeshWorkQueueTest, ResidencyChangesWaitUntilPlayerInterestIsKnown)
{
    static constexpr client::HeightTileRevision REVISION{ 1U, 1U };
    static constexpr client::HeightTileKey KEY{ 15, 27 };
    client::PreviewResidency residency{ REVISION };
    std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> const heights = testsupport::makeHeightSamples();
    ASSERT_EQ(
        residency.accept(
            KEY,
            REVISION,
            1U,
            heights,
            testsupport::makeSurfaceTile(
                KEY,
                testsupport::makeTerrainIdentity(42U, REVISION.revision),
                heights
            )
        ).replacement,
        client::HeightTileReplacement::Published
    );
    client::PreviewMeshWorkQueue queue;
    auto const enqueue_change = [&queue](client::HeightTileChange const change) {
        queue.enqueueChange(change);
    };

    client::drainPreviewTileChanges(residency, 1U, false, enqueue_change);

    EXPECT_EQ(residency.pendingChangeCount(), 1U);
    EXPECT_TRUE(queue.empty());

    client::drainPreviewTileChanges(residency, 1U, true, enqueue_change);

    EXPECT_EQ(residency.pendingChangeCount(), 0U);
    ASSERT_EQ(queue.size(), 5U);
    auto const queued = queue.take();
    ASSERT_TRUE(queued.has_value());
    EXPECT_EQ(*queued, KEY);
}

TEST(PreviewMeshWorkQueueTest, WorkerAndResultPrioritiesPreserveForecastAndCurrentNearTiers)
{
    struct Work final {
        client::HeightTileKey key;
        uint32_t sequence;
        bool missing_current_mesh = false;
    };
    static constexpr client::HeightTileKey CENTER{ 100, 100 };
    std::deque<Work> work{
        { .key = { 108, 100 }, .sequence = 2U },
        { .key = { 107, 107 }, .sequence = 1U },
        { .key = { 109, 100 }, .sequence = 3U },
        { .key = { 110, 100 }, .sequence = 4U },
    };
    auto const tier = [](client::HeightTileKey const key) {
        return static_cast<uint8_t>(key.x == 107 ? 0U : key.x == 108 ? 1U : key.x == 109 ? 2U : 3U);
    };
    auto const current = client::preferredPreviewMeshResultIndex(work, CENTER, 127, 0, tier);
    ASSERT_TRUE(current);
    EXPECT_EQ(work[*current].sequence, 1U);
    auto const deferred_far = client::deferLowerPriorityPreviewMesh(work, tier, 1U);
    ASSERT_TRUE(deferred_far);
    EXPECT_EQ(deferred_far->x, 110);
    auto const deferred_prewarm = client::deferLowerPriorityPreviewMesh(work, tier, 1U);
    ASSERT_TRUE(deferred_prewarm);
    EXPECT_EQ(deferred_prewarm->x, 109);
    EXPECT_FALSE(client::deferLowerPriorityPreviewMesh(work, tier, 1U));
    ASSERT_EQ(work.size(), 2U);
    auto const deferred_forecast = client::deferLowerPriorityPreviewMesh(work, tier, 0U);
    ASSERT_TRUE(deferred_forecast);
    EXPECT_EQ(deferred_forecast->x, 108);
    ASSERT_EQ(work.size(), 1U);
    EXPECT_EQ(work.front().sequence, 1U);
    EXPECT_FALSE(client::deferLowerPriorityPreviewMesh(work, tier, 0U));
}

} // namespace
