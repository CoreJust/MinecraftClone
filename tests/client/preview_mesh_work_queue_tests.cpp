#include <client/PreviewMeshWorkQueue.hpp>

#include <client/PreviewResidency.hpp>

#include <shared/net/Message.hpp>

#include <gtest/gtest.h>

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

TEST(PreviewMeshWorkQueueTest, CompletedMeshResultsFollowTheCurrentInterestAfterTravel)
{
    struct Result final {
        client::HeightTileKey key;
        uint32_t sequence;
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
    std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> heights{};
    ASSERT_EQ(
        residency.accept(KEY, REVISION, 1U, heights).replacement,
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
