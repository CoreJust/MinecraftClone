#include <client/PlayerClient.hpp>

#include <shared/world/World.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <thread>

namespace client {

struct PlayerClientTestAccess final {
    static shared::HeightTileKey seedPreviewState(PlayerClient& client)
    {
        client.m_local_character = '@';
        shared::PlayerPosition const spawn = shared::World::FLIGHT_SPAWN;
        client.m_world.spawnPlayer(0U, '@', spawn);
        shared::HeightTileKey const key = shared::normalizeHeightTileKey({
            .x = spawn.x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
            .y = spawn.y / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
        });
        std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> heights;
        heights.fill(8U);
        EXPECT_EQ(
            client.m_height_tile_residency.accept(key, { .generation = 1U, .revision = 1U }, 1U, heights)
                .replacement,
            HeightTileReplacement::Published
        );
        client.refreshHeightTileInterest(*client.m_world.playerByCharacter('@'));
        client.processPendingPreviewMeshes(1U, std::chrono::steady_clock::now() + std::chrono::seconds{ 2 });
        return key;
    }

    static bool waitForVisibleMesh(PlayerClient& client, shared::HeightTileKey const key)
    {
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 2 };
        while (std::chrono::steady_clock::now() < deadline) {
            client.processPendingPreviewMeshes(16U, deadline);
            if (client.m_visible_preview_meshes.contains(key)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{ 1 });
        }
        return false;
    }

    static void queueSecondMesh(PlayerClient& client, shared::HeightTileKey const key)
    {
        client.queuePreviewMesh(key);
        client.processPendingPreviewMeshes(1U, std::chrono::steady_clock::now() + std::chrono::seconds{ 2 });
    }

    static void resetConnectionState(PlayerClient& client)
    {
        client.resetConnectionState();
    }

    static void drainAfterReset(PlayerClient& client)
    {
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{ 250 };
        while (std::chrono::steady_clock::now() < deadline) {
            client.processPendingPreviewMeshes(16U, deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds{ 1 });
        }
    }

    [[nodiscard]] static uint64_t epoch(PlayerClient const& client) noexcept
    {
        return client.m_preview_mesh_epoch;
    }

    [[nodiscard]] static bool hasJob(PlayerClient const& client, shared::HeightTileKey const key) noexcept
    {
        return client.m_preview_mesh_jobs.contains(key);
    }

    [[nodiscard]] static bool hasCenter(PlayerClient const& client) noexcept
    {
        return client.m_height_tile_center.has_value();
    }

    [[nodiscard]] static bool previewSetsEmpty(PlayerClient const& client) noexcept
    {
        return client.m_height_tile_interest.empty()
            && client.m_pending_preview_meshes.empty()
            && client.m_pending_preview_mesh_set.empty()
            && client.m_preview_mesh_jobs.empty()
            && client.m_preview_mesh_dirty_jobs.empty()
            && client.m_visible_preview_mesh_tiles.empty()
            && client.m_visible_preview_meshes.empty();
    }

    [[nodiscard]] static uint32_t rendererMeshCount(PlayerClient const& client)
    {
        return client.m_renderer.runtimeInfo().height_tile_mesh_count;
    }
};

} // namespace client

TEST(PlayerClientResetTest, ClearsRendererInterestAndDropsQueuedWorkerResults)
{
    client::PlayerClient player_client{ shared::WorldMode::Flight };
    shared::HeightTileKey const key = client::PlayerClientTestAccess::seedPreviewState(player_client);
    ASSERT_TRUE(client::PlayerClientTestAccess::waitForVisibleMesh(player_client, key));
    ASSERT_EQ(client::PlayerClientTestAccess::rendererMeshCount(player_client), 1U);

    uint64_t const epoch_before_reset = client::PlayerClientTestAccess::epoch(player_client);
    client::PlayerClientTestAccess::queueSecondMesh(player_client, key);
    ASSERT_TRUE(client::PlayerClientTestAccess::hasJob(player_client, key));

    client::PlayerClientTestAccess::resetConnectionState(player_client);

    EXPECT_EQ(client::PlayerClientTestAccess::epoch(player_client), epoch_before_reset + 1U);
    EXPECT_FALSE(client::PlayerClientTestAccess::hasCenter(player_client));
    EXPECT_TRUE(client::PlayerClientTestAccess::previewSetsEmpty(player_client));
    EXPECT_EQ(client::PlayerClientTestAccess::rendererMeshCount(player_client), 0U);

    client::PlayerClientTestAccess::drainAfterReset(player_client);
    EXPECT_TRUE(client::PlayerClientTestAccess::previewSetsEmpty(player_client));
    EXPECT_EQ(client::PlayerClientTestAccess::rendererMeshCount(player_client), 0U);
}
