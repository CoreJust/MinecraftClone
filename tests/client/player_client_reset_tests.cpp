#include <client/PlayerClient.hpp>

#include <shared/world/SparseWorld.hpp>
#include <shared/world/World.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <optional>
#include <thread>
#include <utility>

namespace client {

struct PlayerClientTestAccess final {
    static HeightTileHandle seedLodNeighborhood(
        PlayerClient& client,
        shared::HeightTileKey const key,
        HeightTileRevision const revision,
        uint16_t const height
    )
    {
        client.m_height_tile_residency.advanceRevision(revision);
        std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> heights;
        heights.fill(height);
        for (shared::HeightTileKey const tile_key : {
                 key,
                 shared::normalizeHeightTileKey({ .x = key.x - 1, .y = key.y }),
                 shared::normalizeHeightTileKey({ .x = key.x + 1, .y = key.y }),
                 shared::normalizeHeightTileKey({ .x = key.x, .y = key.y - 1 }),
                 shared::normalizeHeightTileKey({ .x = key.x, .y = key.y + 1 }),
             }) {
            EXPECT_EQ(
                client.m_height_tile_residency.accept(tile_key, revision, 1U, heights).replacement,
                HeightTileReplacement::Published
            );
            client.m_height_tile_interest.insert(tile_key);
        }
        client.m_height_tile_center = shared::HeightTileKey{};
        EXPECT_EQ(client.m_height_tile_residency.currentRevision(), revision);
        for (HeightTileHandle const& neighbor : client.previewMeshNeighbors(key)) {
            EXPECT_TRUE(neighbor);
            if (neighbor) {
                EXPECT_EQ(neighbor->revision(), revision);
                EXPECT_EQ(neighbor->heights().front(), height);
            }
        }
        static_cast<void>(client.m_preview_lod.refresh(
            {}, { .min_z_blocks = 1'024.0, .max_z_blocks = 1'024.0 }, {}
        ));
        return client.m_height_tile_residency.resident({ .x = key.x + 1, .y = key.y });
    }

    static void publishLodTile(
        PlayerClient& client,
        HeightTileHandle const& tile,
        std::chrono::steady_clock::time_point const deadline
    )
    {
        shared::HeightTile const surface{
            .coordinate = { .x = tile->key().x, .y = tile->key().y },
            .heights = tile->heights(),
        };
        shared::HeightTileSurfaceMesh mesh = shared::HeightTileSurfaceMesher{}.build(surface, {
            .negative_x = surface.heights,
            .positive_x = surface.heights,
            .negative_y = surface.heights,
            .positive_y = surface.heights,
        });
        client.publishPreviewMesh(tile->key(), std::move(mesh), tile, deadline);
    }

    static shared::HeightTileSurfaceDetail lodDetail(PlayerClient& client, shared::HeightTileKey const key)
    {
        return client.previewMeshDetail(key, client.m_height_tile_residency.resident(key));
    }

    static std::optional<shared::HeightTileSurfaceDetail> selectedLodDetail(
        PlayerClient const& client,
        shared::HeightTileKey const key
    )
    {
        return client.m_preview_lod.detail(key);
    }

    static void expectLodElevation(
        PlayerClient const& client,
        shared::HeightTileKey const key,
        uint16_t const minimum,
        uint16_t const maximum,
        uint16_t const installed_height
    )
    {
        PlayerClient::PreviewTileElevation const& elevation = client.m_preview_tile_elevations.at(key);
        HeightTileHandle const tile = client.m_height_tile_residency.resident(key);
        EXPECT_EQ(elevation.tile_revision, tile->revision());
        EXPECT_EQ(elevation.minimum, minimum);
        EXPECT_EQ(elevation.maximum, maximum);
        ASSERT_TRUE(elevation.neighbor_mesh_ranges[1U].has_value());
        EXPECT_EQ(elevation.neighbor_mesh_ranges[1U]->minimum, installed_height);
        EXPECT_EQ(elevation.neighbor_mesh_ranges[1U]->maximum, installed_height);
    }

    static void removeLodTile(PlayerClient& client, shared::HeightTileKey const key)
    {
        client.m_height_tile_interest.erase(key);
        client.removePreviewMesh(key, std::chrono::steady_clock::now() + std::chrono::seconds{ 2 });
    }

    static shared::HeightTileKey seedPreviewState(
        PlayerClient& client,
        uint32_t const radius = shared::HEIGHT_TILE_INTEREST_RADIUS
    )
    {
        static_cast<void>(client.applyHeightTileDescriptor({
            .configuration = client.m_world.configuration(),
            .max_height_tiles = shared::heightTileInterestCount(radius),
        }));
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

    static uint32_t interestCount(PlayerClient const& client)
    {
        return static_cast<uint32_t>(client.m_height_tile_interest.size());
    }

    static CameraProjection cameraProjection(PlayerClient const& client)
    {
        return client.m_camera.projection();
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
            && client.m_preview_mesh_jobs.empty()
            && client.m_preview_mesh_dirty_jobs.empty()
            && client.m_visible_preview_mesh_tiles.empty()
            && client.m_visible_preview_mesh_elevation_ranges.empty()
            && client.m_visible_preview_meshes.empty();
    }

    [[nodiscard]] static uint32_t rendererMeshCount(PlayerClient const& client)
    {
        return client.m_renderer.runtimeInfo().height_tile_mesh_count;
    }
};

} // namespace client

TEST(PlayerClientResetTest, AcceptedServerRadiusDefinesCameraAndInterestWithoutChangingFov)
{
    static constexpr std::array<uint32_t, 2> RADII{128U, 256U};
    for (uint32_t const radius : RADII) {
        client::PlayerClient player_client{shared::WorldMode::Flight};
        auto const initial_projection = client::PlayerClientTestAccess::cameraProjection(player_client);
        static_cast<void>(client::PlayerClientTestAccess::seedPreviewState(player_client, radius));
        EXPECT_EQ(player_client.renderDistance(), radius);
        EXPECT_EQ(
            client::PlayerClientTestAccess::interestCount(player_client), shared::heightTileInterestCount(radius)
        );
        auto const projection = client::PlayerClientTestAccess::cameraProjection(player_client);
        EXPECT_EQ(projection.far_plane,
            static_cast<double>(radius) * shared::HEIGHT_TILE_SIDE_LENGTH + shared::WorldExtent::DEPTH);
        EXPECT_EQ(projection.vertical_fov_degrees, initial_projection.vertical_fov_degrees);
    }
}

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

TEST(PlayerClientResetTest, OlderInstalledNeighborBoundsSurviveRejectedReplacementAndInvalidateOnPublication)
{
    static constexpr shared::HeightTileKey KEY{ .x = 200, .y = 0 };
    static constexpr client::HeightTileRevision FIRST{ .generation = 1U, .revision = 1U };
    static constexpr client::HeightTileRevision NEXT{ .generation = 2U, .revision = 2U };
    static constexpr uint16_t OLD_HEIGHT = 512U;
    static constexpr uint16_t NEW_HEIGHT = 1'024U;
    client::PlayerClient player_client{ shared::WorldMode::Flight };
    client::HeightTileHandle const old_east = client::PlayerClientTestAccess::seedLodNeighborhood(
        player_client, KEY, FIRST, OLD_HEIGHT
    );
    client::PlayerClientTestAccess::publishLodTile(
        player_client, old_east, std::chrono::steady_clock::now() + std::chrono::seconds{ 2 }
    );
    ASSERT_EQ(client::PlayerClientTestAccess::rendererMeshCount(player_client), 1U);
    client::PlayerClientTestAccess::expectLodElevation(player_client, KEY, OLD_HEIGHT, OLD_HEIGHT, OLD_HEIGHT);
    client::HeightTileHandle const new_east = client::PlayerClientTestAccess::seedLodNeighborhood(
        player_client, KEY, NEXT, NEW_HEIGHT
    );
    EXPECT_NE(old_east->revision(), new_east->revision());
    EXPECT_EQ(old_east->token(), new_east->token());
    EXPECT_EQ(old_east->heights().front(), OLD_HEIGHT);
    EXPECT_EQ(new_east->heights().front(), NEW_HEIGHT);
    EXPECT_EQ(client::PlayerClientTestAccess::lodDetail(player_client, KEY), shared::HeightTileSurfaceDetail::Fine);
    client::PlayerClientTestAccess::expectLodElevation(player_client, KEY, OLD_HEIGHT, NEW_HEIGHT, OLD_HEIGHT);

    client::PlayerClientTestAccess::publishLodTile(
        player_client, new_east, std::chrono::steady_clock::now() - std::chrono::seconds{ 1 }
    );
    EXPECT_EQ(client::PlayerClientTestAccess::lodDetail(player_client, KEY), shared::HeightTileSurfaceDetail::Fine);
    client::PlayerClientTestAccess::expectLodElevation(player_client, KEY, OLD_HEIGHT, NEW_HEIGHT, OLD_HEIGHT);

    client::PlayerClientTestAccess::publishLodTile(
        player_client, new_east, std::chrono::steady_clock::now() + std::chrono::seconds{ 2 }
    );
    EXPECT_EQ(
        client::PlayerClientTestAccess::selectedLodDetail(player_client, KEY), shared::HeightTileSurfaceDetail::Coarse2
    );
    EXPECT_EQ(client::PlayerClientTestAccess::lodDetail(player_client, KEY), shared::HeightTileSurfaceDetail::Coarse2);
    client::PlayerClientTestAccess::expectLodElevation(player_client, KEY, NEW_HEIGHT, NEW_HEIGHT, NEW_HEIGHT);
    EXPECT_EQ(client::PlayerClientTestAccess::rendererMeshCount(player_client), 1U);
}

TEST(PlayerClientResetTest, InstalledNeighborRemovalInvalidatesRetainedProfileBounds)
{
    static constexpr shared::HeightTileKey KEY{ .x = 200, .y = 0 };
    static constexpr client::HeightTileRevision FIRST{ .generation = 1U, .revision = 1U };
    static constexpr client::HeightTileRevision NEXT{ .generation = 2U, .revision = 2U };
    client::PlayerClient player_client{ shared::WorldMode::Flight };
    client::HeightTileHandle const old_east = client::PlayerClientTestAccess::seedLodNeighborhood(
        player_client, KEY, FIRST, 512U
    );
    client::PlayerClientTestAccess::publishLodTile(
        player_client, old_east, std::chrono::steady_clock::now() + std::chrono::seconds{ 2 }
    );
    static_cast<void>(client::PlayerClientTestAccess::seedLodNeighborhood(player_client, KEY, NEXT, 1'024U));
    EXPECT_EQ(client::PlayerClientTestAccess::lodDetail(player_client, KEY), shared::HeightTileSurfaceDetail::Fine);

    client::PlayerClientTestAccess::removeLodTile(player_client, old_east->key());

    EXPECT_EQ(
        client::PlayerClientTestAccess::selectedLodDetail(player_client, KEY), shared::HeightTileSurfaceDetail::Coarse2
    );
    EXPECT_EQ(client::PlayerClientTestAccess::lodDetail(player_client, KEY), shared::HeightTileSurfaceDetail::Coarse2);
    EXPECT_EQ(client::PlayerClientTestAccess::rendererMeshCount(player_client), 0U);
}
