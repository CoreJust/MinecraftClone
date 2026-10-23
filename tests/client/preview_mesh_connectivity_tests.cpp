#include <client/PreviewMeshConnectivity.hpp>

#include <shared/net/Message.hpp>

#include <gtest/gtest.h>

#include <array>
#include <span>
#include <string>
#include <vector>

namespace {

using Key = shared::HeightTileKey;

TEST(PreviewMeshConnectivityTest, FarReadyMeshWaitsForVisibleBridgeFromTheCurrentRoot)
{
    static constexpr Key CENTER{ .x = 100, .y = 100 };
    static constexpr Key BRIDGE{ .x = 101, .y = 100 };
    static constexpr Key FAR{ .x = 102, .y = 100 };
    client::PreviewMeshConnectivity connectivity;

    EXPECT_TRUE(connectivity.rebase(CENTER, 8U, {}).empty());
    EXPECT_TRUE(connectivity.canPublish(CENTER));
    EXPECT_FALSE(connectivity.canPublish(BRIDGE));
    EXPECT_FALSE(connectivity.canPublish(FAR));

    ASSERT_TRUE(connectivity.published(CENTER));
    EXPECT_FALSE(connectivity.canPublish(FAR));
    ASSERT_TRUE(connectivity.published(BRIDGE));
    EXPECT_TRUE(connectivity.canPublish(FAR));
    ASSERT_TRUE(connectivity.published(FAR));
    EXPECT_EQ(connectivity.connectedCount(), 3U);

    // A replacement of an already connected tile stays eligible without duplicating membership.
    EXPECT_TRUE(connectivity.published(BRIDGE));
    EXPECT_EQ(connectivity.connectedCount(), 3U);
}

TEST(PreviewMeshConnectivityTest, PublicationUsesWrappedCardinalNeighbors)
{
    static constexpr Key CENTER{ .x = 4'095, .y = 0 };
    static constexpr Key ACROSS_SEAM{ .x = 0, .y = 0 };
    static constexpr Key DIAGONAL{ .x = 0, .y = 1 };
    client::PreviewMeshConnectivity connectivity;

    EXPECT_TRUE(connectivity.rebase(CENTER, 2U, {}).empty());
    ASSERT_TRUE(connectivity.published(CENTER));
    EXPECT_TRUE(connectivity.canPublish({ .x = 4'096, .y = 0 }));
    EXPECT_FALSE(connectivity.canPublish(DIAGONAL));

    ASSERT_TRUE(connectivity.published(ACROSS_SEAM));
    EXPECT_TRUE(connectivity.canPublish(DIAGONAL));
}

TEST(PreviewMeshConnectivityTest, OverlapRebaseRetainsTheComponentAndTeleportRequiresItsNewRoot)
{
    static constexpr Key OLD_CENTER{ .x = 100, .y = 100 };
    static constexpr Key MOVED_CENTER{ .x = 101, .y = 100 };
    static constexpr Key TELEPORT_CENTER{ .x = 2'000, .y = 2'000 };
    static constexpr std::array VISIBLE{
        OLD_CENTER,
        Key{ .x = 101, .y = 100 },
        Key{ .x = 102, .y = 100 },
    };
    client::PreviewMeshConnectivity connectivity;

    EXPECT_TRUE(connectivity.rebase(OLD_CENTER, 4U, {}).empty());
    for (Key const key : VISIBLE) {
        ASSERT_TRUE(connectivity.published(key));
    }

    EXPECT_TRUE(connectivity.rebase(MOVED_CENTER, 4U, VISIBLE).empty());
    EXPECT_EQ(connectivity.connectedCount(), 3U);
    EXPECT_TRUE(connectivity.connected(VISIBLE.back()));

    std::vector<Key> const detached = connectivity.rebase(TELEPORT_CENTER, 4U, VISIBLE);
    EXPECT_EQ(detached, (std::vector<Key>{ VISIBLE[0], VISIBLE[1], VISIBLE[2] }));
    EXPECT_EQ(connectivity.connectedCount(), 0U);
    EXPECT_FALSE(connectivity.canPublish(VISIBLE[0]));
    EXPECT_TRUE(connectivity.canPublish(TELEPORT_CENTER));
    ASSERT_TRUE(connectivity.published(TELEPORT_CENTER));
    EXPECT_TRUE(connectivity.canPublish({ .x = 2'001, .y = 2'000 }));
}

TEST(PreviewMeshConnectivityTest, CurrentCenterCanBootstrapAVisibleIslandAfterMovement)
{
    static constexpr Key OLD_CENTER{ .x = 100, .y = 100 };
    static constexpr Key TELEPORT_CENTER{ .x = 2'000, .y = 2'000 };
    static constexpr std::array VISIBLE{
        OLD_CENTER,
        Key{ .x = 101, .y = 100 },
    };
    client::PreviewMeshConnectivity connectivity;

    EXPECT_TRUE(connectivity.rebase(OLD_CENTER, 4U, {}).empty());
    for (Key const key : VISIBLE) {
        ASSERT_TRUE(connectivity.published(key));
    }
    EXPECT_EQ(connectivity.rebase(TELEPORT_CENTER, 4U, VISIBLE),
        (std::vector<Key>{ VISIBLE[0], VISIBLE[1] }));
    EXPECT_EQ(connectivity.connectedCount(), 0U);

    EXPECT_TRUE(connectivity.rebase(OLD_CENTER, 4U, VISIBLE).empty());
    EXPECT_EQ(connectivity.connectedCount(), 2U);
    EXPECT_TRUE(connectivity.connected(OLD_CENTER));
    EXPECT_TRUE(connectivity.connected(VISIBLE[1]));
}

TEST(PreviewMeshConnectivityTest, BridgeRemovalDefersOnlyDetachedMeshesUntilTheyCanBeRemeshed)
{
    static constexpr Key CENTER{ .x = 20, .y = 20 };
    static constexpr Key BRIDGE{ .x = 21, .y = 20 };
    static constexpr Key FAR{ .x = 22, .y = 20 };
    static constexpr Key FARTHEST{ .x = 23, .y = 20 };
    static constexpr std::array VISIBLE{ CENTER, BRIDGE, FAR, FARTHEST };
    client::PreviewMeshConnectivity connectivity;

    EXPECT_TRUE(connectivity.rebase(CENTER, 8U, VISIBLE).empty());
    ASSERT_EQ(connectivity.connectedCount(), 4U);

    std::array const removed{ BRIDGE };
    std::vector<Key> const detached = connectivity.remove(removed);

    EXPECT_EQ(detached, (std::vector<Key>{ FAR, FARTHEST }));
    EXPECT_EQ(connectivity.connectedCount(), 1U);
    EXPECT_TRUE(connectivity.connected(CENTER));
    EXPECT_FALSE(connectivity.connected(FAR));
    EXPECT_FALSE(connectivity.canPublish(FAR));

    // The detached resident tiles become eligible again after the visible bridge is remeshed.
    ASSERT_TRUE(connectivity.published(BRIDGE));
    ASSERT_TRUE(connectivity.published(FAR));
    EXPECT_TRUE(connectivity.canPublish(FARTHEST));
}

TEST(PreviewMeshConnectivityTest, InterestCropReturnsOutsideMeshesForRendererRetirement)
{
    static constexpr Key CENTER{ .x = 40, .y = 40 };
    static constexpr Key EAST{ .x = 41, .y = 40 };
    static constexpr Key FAR{ .x = 42, .y = 40 };
    static constexpr Key FARTHEST{ .x = 43, .y = 40 };
    static constexpr std::array VISIBLE{ CENTER, EAST, FAR, FARTHEST };
    client::PreviewMeshConnectivity connectivity;

    EXPECT_TRUE(connectivity.rebase(CENTER, 8U, VISIBLE).empty());
    std::vector<Key> const detached = connectivity.rebase(CENTER, 1U, VISIBLE);

    EXPECT_EQ(detached, (std::vector<Key>{ FAR, FARTHEST }));
    EXPECT_EQ(connectivity.connectedCount(), 2U);
    EXPECT_TRUE(connectivity.connected(CENTER));
    EXPECT_TRUE(connectivity.connected(EAST));
}

} // namespace
