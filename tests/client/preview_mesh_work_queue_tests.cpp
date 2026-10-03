#include <client/PreviewMeshWorkQueue.hpp>

#include <gtest/gtest.h>

#include <array>
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
        residency.accept(KEY, REVISION, 1U, std::move(heights)).replacement,
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

} // namespace
