#include <client/PlayerClient.hpp>

#include <shared/world/SparseWorld.hpp>
#include <shared/world/World.hpp>

#include <gtest/gtest.h>
#include <testsupport/TerrainFixtures.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace client {

struct PlayerClientTestAccess final {
    static void initializePreviewMeshFrontier(
        PlayerClient& client,
        shared::HeightTileKey const center,
        uint32_t const radius
    )
    {
        client.m_height_tile_interest.set(center, radius);
        std::vector<shared::HeightTileKey> visible_keys;
        visible_keys.reserve(client.m_visible_preview_mesh_tiles.size());
        for (auto const& [key, tile] : client.m_visible_preview_mesh_tiles) {
            static_cast<void>(tile);
            visible_keys.push_back(key);
        }
        std::vector<shared::HeightTileKey> const detached = client.m_preview_mesh_connectivity.rebase(
            center, radius, visible_keys
        );
        client.queueDisconnectedPreviewMeshes(detached);
        if (client.m_height_tile_residency.resident(center)
            && !client.m_preview_mesh_connectivity.connected(center)) {
            client.queuePreviewMesh(center);
        }
    }

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
            shared::SurfaceTile const surface = testsupport::makeSurfaceTile(
                tile_key,
                testsupport::makeTerrainIdentity(client.m_world.configuration().seed, revision.revision),
                heights
            );
            EXPECT_EQ(
                client.m_height_tile_residency.accept(
                    tile_key,
                    revision,
                    1U,
                    heights,
                    surface
                ).replacement,
                HeightTileReplacement::Published
            );
        }
        initializePreviewMeshFrontier(client, key, 1U);
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

    static HeightTileHandle seedLodTile(
        PlayerClient& client,
        shared::HeightTileKey const key,
        HeightTileRevision const revision,
        uint16_t const height
    )
    {
        std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> heights;
        heights.fill(height);
        EXPECT_EQ(
            client.m_height_tile_residency.accept(
                key,
                revision,
                1U,
                heights,
                testsupport::makeSurfaceTile(
                    key,
                    testsupport::makeTerrainIdentity(client.m_world.configuration().seed, revision.revision),
                    heights
                )
            ).replacement,
            HeightTileReplacement::Published
        );
        return client.m_height_tile_residency.resident(key);
    }

    static HeightTileHandle residentLodTile(PlayerClient const& client, shared::HeightTileKey const key)
    {
        return client.m_height_tile_residency.resident(key);
    }

    static bool publishLodTile(
        PlayerClient& client,
        HeightTileHandle const& tile,
        std::chrono::steady_clock::time_point const deadline,
        shared::HeightTileSurfaceDetail const detail = shared::HeightTileSurfaceDetail::Fine
    )
    {
        shared::HeightTile const surface{
            .coordinate = { .x = tile->key().x, .y = tile->key().y },
            .heights = tile->heights(),
            .surface = tile->surface(),
        };
        shared::HeightTileSurfaceMesh mesh = shared::HeightTileSurfaceMesher{}.build(surface, {
            .negative_x = surface.heights,
            .positive_x = surface.heights,
            .negative_y = surface.heights,
            .positive_y = surface.heights,
        }, detail);
        bool const published = client.publishPreviewMesh(tile->key(), mesh, tile, deadline);
        if (published) {
            static_cast<void>(client.m_pending_preview_meshes.erase(tile->key()));
        }
        return published;
    }

    static void publishResidentLodTile(PlayerClient& client, shared::HeightTileKey const key)
    {
        HeightTileHandle const tile = client.m_height_tile_residency.resident(key);
        ASSERT_TRUE(tile);
        ASSERT_TRUE(publishLodTile(client, tile, std::chrono::steady_clock::now() + std::chrono::seconds{ 2 }));
    }

    static shared::HeightTileSurfaceDetail lodDetail(PlayerClient& client, shared::HeightTileKey const key)
    {
        return client.previewMeshDetail(key, client.m_height_tile_residency.resident(key));
    }

    static HeightTileHandle replaceLodHeights(
        PlayerClient& client,
        shared::HeightTileKey const key,
        shared::HeightTile::Heights const& heights
    )
    {
        HeightTileRevision const revision = client.m_height_tile_residency.currentRevision();
        EXPECT_EQ(client.m_height_tile_residency.accept(
            key,
            revision,
            2U,
            heights,
            testsupport::makeSurfaceTile(
                key,
                testsupport::makeTerrainIdentity(client.m_world.configuration().seed, revision.revision),
                heights
            )
        ).replacement, HeightTileReplacement::Published);
        return client.m_height_tile_residency.resident(key);
    }

    static std::array<uint64_t, 4> neighborProfileTokens(
        PlayerClient const& client,
        shared::HeightTileKey const key
    )
    {
        return client.previewMeshNeighborProfileTokens(key);
    }

    static std::array<uint64_t, 4> cachedNeighborProfileTokens(
        PlayerClient const& client,
        shared::HeightTileKey const key
    )
    {
        return client.m_preview_tile_elevations.at(key).neighbor_profile_tokens;
    }

    static std::array<shared::HeightTileSurfaceDetail, 4> neighborDetails(
        PlayerClient const& client,
        shared::HeightTileKey const key
    )
    {
        return client.previewMeshNeighborDetails(key);
    }

    static shared::HeightTileSurfaceEdgeProfile installedProfile(
        PlayerClient const& client,
        shared::HeightTileKey const key,
        uint32_t const edge
    )
    {
        return client.m_visible_preview_mesh_edge_profiles.at(key).edges[edge];
    }

    static bool drainLodNeighborhood(PlayerClient& client, shared::HeightTileKey const key)
    {
        for (shared::HeightTileKey const tile_key : {
                 key,
                 shared::normalizeHeightTileKey({ key.x - 1, key.y }),
                 shared::normalizeHeightTileKey({ key.x + 1, key.y }),
                 shared::normalizeHeightTileKey({ key.x, key.y - 1 }),
                 shared::normalizeHeightTileKey({ key.x, key.y + 1 }),
             }) {
            client.queuePreviewMesh(tile_key);
        }
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 2 };
        while (std::chrono::steady_clock::now() < deadline) {
            client.processPendingPreviewMeshes(16U, deadline);
            if (client.m_pending_preview_meshes.empty() && client.m_preview_mesh_jobs.empty()
                && client.m_preview_mesh_dirty_jobs.empty()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{ 1 });
        }
        return false;
    }

    static bool prepareCompletedLodJob(PlayerClient& client, shared::HeightTileKey const key)
    {
        for (shared::HeightTileKey const tile_key : {
                 key,
                 shared::normalizeHeightTileKey({ key.x - 1, key.y }),
                 shared::normalizeHeightTileKey({ key.x + 1, key.y }),
                 shared::normalizeHeightTileKey({ key.x, key.y - 1 }),
                 shared::normalizeHeightTileKey({ key.x, key.y + 1 }),
             }) {
            static_cast<void>(lodDetail(client, tile_key));
        }
        client.m_pending_preview_meshes.clear();
        client.m_preview_mesh_dirty_jobs.clear();
        client.m_capture.emplace();
        bool const track_mesh_stages = client.m_benchmark.has_value()
            && client.m_benchmark->enable_mesh_stage_diagnostics;
        client.m_benchmark.emplace();
        client.m_benchmark->enable_mesh_stage_diagnostics = track_mesh_stages;
        client::PlayerClientBenchmarkFullTerrainInfo const before = client.benchmarkFullTerrainStreamingInfo();
        std::optional<shared::HeightTileKey> const center = client.m_height_tile_interest.center();
        HeightTileHandle const tile = client.m_height_tile_residency.resident(key);
        if (!client.m_preview_mesh_jobs.empty() || before.worker_result_count != 0U
            || before.worker_outstanding_count != 0U || !center.has_value() || *center != key
            || !client.m_height_tile_interest.contains(key) || !tile) {
            ADD_FAILURE() << "Expected an idle, interested target before preparing a single-key mesh job";
            return false;
        }
        client.queuePreviewMesh(key);
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 2 };
        std::span<shared::HeightTileKey const> const no_nearby_candidates{};
        if (!client.enqueuePreviewMeshJob(key, tile, key, no_nearby_candidates, deadline)) {
            ADD_FAILURE() << "Failed to submit the requested completed mesh job";
            return false;
        }
        while (std::chrono::steady_clock::now() < deadline) {
            client::PlayerClientBenchmarkFullTerrainInfo const info = client.benchmarkFullTerrainStreamingInfo();
            if (info.worker_result_count == 1U && info.worker_outstanding_count == 1U
                && info.active_mesh_job_count == 1U && info.pending_mesh_count == 0U
                && client.m_preview_mesh_jobs.contains(key)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{ 1 });
        }
        client::PlayerClientBenchmarkFullTerrainInfo const info = client.benchmarkFullTerrainStreamingInfo();
        ADD_FAILURE() << "Timed out preparing the requested completed mesh: results=" << info.worker_result_count
                      << " outstanding=" << info.worker_outstanding_count
                      << " pending=" << info.pending_mesh_count
                      << " active=" << info.active_mesh_job_count
                      << " target_job_active=" << client.m_preview_mesh_jobs.contains(key);
        return false;
    }

    static void processOneLodResult(
        PlayerClient& client,
        std::chrono::steady_clock::time_point const deadline
    )
    {
        client.processPendingPreviewMeshes(1U, deadline);
    }

    static void processOneLodResult(PlayerClient& client)
    {
        processOneLodResult(client, std::chrono::steady_clock::now() + std::chrono::seconds{ 2 });
    }

    static void processOneIsolatedLodResult(PlayerClient& client)
    {
        auto const renderer_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 2 };
        client.processPendingPreviewMeshes(1U, renderer_deadline);
    }

    static uint64_t meshAdmissionCount(PlayerClient const& client)
    {
        return client.m_benchmark_accepted_near_admissions + client.m_benchmark_accepted_far_admissions;
    }

    static uint64_t meshResultCount(PlayerClient const& client)
    {
        return client.m_capture_mesh_results;
    }

    static void initializeInterest(PlayerClient& client, uint32_t const radius)
    {
        client.m_render_distance = radius;
        client.m_height_tile_interest.clear();
        client.refreshHeightTileInterest(shared::Player{
            .id = 1,
            .x = 0,
            .y = 0,
            .z = 512,
            .ch = '@',
        });
    }

    static HeightTileInterestRegion interestRegion(PlayerClient const& client)
    {
        return client.m_height_tile_interest;
    }

    static void refreshInterestWithHeadingAndProjection(
        PlayerClient& client,
        int8_t const heading_x,
        int8_t const heading_y,
        shared::HeightTileSurfaceProjection const projection
    )
    {
        client.m_interest_heading_x = heading_x;
        client.m_interest_heading_y = heading_y;
        client.refreshHeightTileInterest(shared::Player{
            .id = 1,
            .x = 0,
            .y = 0,
            .z = 512,
            .ch = '@',
        }, projection);
    }

    static void moveInterestTo(PlayerClient& client, int32_t const tile_x)
    {
        client.refreshHeightTileInterest(shared::Player{
            .id = 1,
            .x = tile_x * static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
            .y = 0,
            .z = 512,
            .ch = '@',
        });
    }

    static void setInterestAroundTile(PlayerClient& client, shared::HeightTileKey const key)
    {
        initializePreviewMeshFrontier(client, key, 1U);
    }

    static bool prepareNearCompletedLodJob(
        PlayerClient& client,
        shared::HeightTileKey const key,
        shared::HeightTileKey const already_completed_key
    )
    {
        client::PlayerClientBenchmarkFullTerrainInfo const before = client.benchmarkFullTerrainStreamingInfo();
        std::optional<shared::HeightTileKey> const center = client.m_height_tile_interest.center();
        HeightTileHandle const tile = client.m_height_tile_residency.resident(key);
        if (before.worker_result_count != 1U || before.worker_outstanding_count != 1U
            || before.active_mesh_job_count != 1U || !client.m_preview_mesh_jobs.contains(already_completed_key)
            || !center.has_value() || *center != key || !client.m_height_tile_interest.contains(key) || !tile) {
            ADD_FAILURE() << "Expected the older result and a centered, resident near target";
            return false;
        }
        client.m_pending_preview_meshes.clear();
        client.queuePreviewMesh(key);
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 2 };
        std::span<shared::HeightTileKey const> const no_nearby_candidates{};
        if (!client.enqueuePreviewMeshJob(key, tile, key, no_nearby_candidates, deadline)) {
            ADD_FAILURE() << "Failed to submit the near completed mesh job";
            return false;
        }
        while (std::chrono::steady_clock::now() < deadline) {
            client::PlayerClientBenchmarkFullTerrainInfo const info = client.benchmarkFullTerrainStreamingInfo();
            if (info.worker_result_count == 2U && info.worker_outstanding_count == 2U
                && info.active_mesh_job_count == 2U && info.pending_mesh_count == 0U
                && client.m_preview_mesh_jobs.contains(already_completed_key)
                && client.m_preview_mesh_jobs.contains(key)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{ 1 });
        }
        ADD_FAILURE() << "Timed out waiting for both named mesh results";
        return false;
    }

    static bool currentMeshVisible(PlayerClient const& client, shared::HeightTileKey const key)
    {
        auto const visible = client.m_visible_preview_mesh_tiles.find(key);
        return visible != client.m_visible_preview_mesh_tiles.end()
            && visible->second == client.m_height_tile_residency.resident(key);
    }

    static shared::HeightTileSurfaceDetail installedDetail(PlayerClient const& client, shared::HeightTileKey const key)
    {
        return client.m_visible_preview_mesh_bases.at(key).detail;
    }

    static uint64_t baseStorageBytes(PlayerClient const& client, shared::HeightTileKey const key)
    {
        return static_cast<uint64_t>(client.m_visible_preview_mesh_bases.at(key).quads.capacity())
            * sizeof(shared::HeightTileSurfaceQuad);
    }

    static uint64_t retiredPreviewCacheHighWaterBytes(PlayerClient const& client)
    {
        return client.previewCacheRetirementHighWaterBytes();
    }

    static uint64_t pendingPreviewCacheRetirementBytes(PlayerClient const& client)
    {
        return client.previewCacheRetirementOutstandingBytes();
    }

    static uint64_t cacheRetirementSubmissionCount(PlayerClient const& client)
    {
        return client.previewCacheRetirementSubmissionCount();
    }

    static uint64_t cacheRetirementFallbackCount(PlayerClient const& client)
    {
        return client.previewCacheRetirementFallbackCount();
    }

    static bool previewCacheContains(PlayerClient const& client, shared::HeightTileKey const key)
    {
        return client.m_visible_preview_meshes.contains(key)
            || client.m_visible_preview_mesh_tiles.contains(key)
            || client.m_visible_preview_mesh_bases.contains(key)
            || client.m_visible_preview_mesh_edge_profiles.contains(key)
            || client.m_visible_preview_mesh_seam_bridges.contains(key);
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
        client.m_pending_preview_meshes.clear();
        std::array const removed_keys{ key };
        std::vector<shared::HeightTileKey> const detached = client.m_preview_mesh_connectivity.remove(removed_keys);
        client.queueDisconnectedPreviewMeshes(detached);
        std::vector<shared::HeightTileCoordinate> renderer_removals;
        client.removePreviewMesh(key, renderer_removals);
        static_cast<void>(client.m_renderer.removeHeightTileMeshes(renderer_removals));
        for (shared::HeightTileKey const neighbor : {
                 shared::normalizeHeightTileKey({ .x = key.x - 1, .y = key.y }),
                 shared::normalizeHeightTileKey({ .x = key.x + 1, .y = key.y }),
                 shared::normalizeHeightTileKey({ .x = key.x, .y = key.y - 1 }),
                 shared::normalizeHeightTileKey({ .x = key.x, .y = key.y + 1 }),
             }) {
            client.queuePreviewMesh(neighbor);
        }
        client.submitPreviewCacheRetirements();
    }

    static bool pendingLodMesh(PlayerClient const& client, shared::HeightTileKey const key)
    {
        return client.m_pending_preview_meshes.contains(key);
    }

    static void queueLodMesh(PlayerClient& client, shared::HeightTileKey const key)
    {
        client.queuePreviewMesh(key);
    }

    static bool waitingForConnectedLodMesh(PlayerClient const& client, shared::HeightTileKey const key)
    {
        return client.m_preview_mesh_connectivity_waiting.contains(key);
    }

    static shared::HeightTileKey seedPreviewState(
        PlayerClient& client,
        uint32_t const radius = shared::HEIGHT_TILE_INTEREST_RADIUS
    )
    {
        client.m_height_tile_credit_revision = 1U;
        static_cast<void>(client.applyHeightTileDescriptor({
            .configuration = client.m_world.configuration(),
            .world_identity = testsupport::makeTerrainIdentity(client.m_world.configuration().seed, 1U),
            .world_revision = 1U,
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
            client.m_height_tile_residency.accept(
                key,
                { .generation = 1U, .revision = 1U },
                1U,
                heights,
                testsupport::makeSurfaceTile(
                    key,
                    testsupport::makeTerrainIdentity(client.m_world.configuration().seed, 1U),
                    heights
                )
            )
                .replacement,
            HeightTileReplacement::Published
        );
        client.refreshHeightTileInterest(*client.m_world.playerByCharacter('@'));
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 2 };
        client.processPendingPreviewMeshes(1U, deadline);
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

    static bool prepareCompletedLodJobFromDisconnectedTile(
        PlayerClient& client,
        shared::HeightTileKey const key,
        shared::HeightTileKey const center
    )
    {
        HeightTileHandle const tile = client.m_height_tile_residency.resident(key);
        if (!tile || !client.m_height_tile_interest.contains(key)) {
            ADD_FAILURE() << "Expected an interested resident tile for the disconnected mesh job";
            return false;
        }
        std::span<shared::HeightTileKey const> const no_nearby_candidates{};
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 2 };
        if (!client.enqueuePreviewMeshJob(key, tile, center, no_nearby_candidates, deadline)) {
            ADD_FAILURE() << "Failed to submit the disconnected mesh job";
            return false;
        }
        while (std::chrono::steady_clock::now() < deadline) {
            client::PlayerClientBenchmarkFullTerrainInfo const info = client.benchmarkFullTerrainStreamingInfo();
            if (info.worker_result_count == 1U && info.worker_outstanding_count == 1U
                && info.active_mesh_job_count == 1U && client.m_preview_mesh_jobs.contains(key)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{ 1 });
        }
        ADD_FAILURE() << "Timed out waiting for the disconnected mesh result";
        return false;
    }

    static void queueSecondMesh(PlayerClient& client, shared::HeightTileKey const key)
    {
        client.queuePreviewMesh(key);
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 2 };
        client.processPendingPreviewMeshes(1U, deadline);
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
        return client.m_height_tile_interest.center().has_value();
    }

    [[nodiscard]] static bool previewSetsEmpty(PlayerClient const& client) noexcept
    {
        return client.m_height_tile_interest.empty()
            && client.m_pending_preview_meshes.empty()
            && client.m_preview_mesh_jobs.empty()
            && client.m_preview_mesh_dirty_jobs.empty()
            && client.m_visible_preview_mesh_tiles.empty()
            && client.m_visible_preview_mesh_bases.empty()
            && client.m_visible_preview_mesh_edge_profiles.empty()
            && client.m_visible_preview_mesh_seam_bridges.empty()
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

TEST(PlayerClientResetTest, InterestMembershipRemainsPlayerCenteredWhenHeadingAndProjectionChange)
{
    client::PlayerClient player_client{ shared::WorldMode::Flight };
    client::PlayerClientTestAccess::initializeInterest(player_client, 1U);
    client::HeightTileInterestRegion const baseline =
        client::PlayerClientTestAccess::interestRegion(player_client);

    client::PlayerClientTestAccess::refreshInterestWithHeadingAndProjection(
        player_client,
        -127,
        0,
        { .vertical_fov_degrees = 52.0, .viewport_width_pixels = 1'920U, .viewport_height_pixels = 1'080U }
    );
    EXPECT_EQ(client::PlayerClientTestAccess::interestRegion(player_client), baseline);

    client::PlayerClientTestAccess::refreshInterestWithHeadingAndProjection(
        player_client,
        0,
        127,
        { .vertical_fov_degrees = 84.0, .viewport_width_pixels = 800U, .viewport_height_pixels = 600U }
    );
    EXPECT_EQ(client::PlayerClientTestAccess::interestRegion(player_client), baseline);
}

TEST(PlayerClientResetTest, RemovedPreviewCacheStorageIsRetiredAndConnectionResetDrainsIt)
{
    static constexpr shared::HeightTileKey KEY{ 200, 0 };
    static constexpr client::HeightTileRevision REVISION{ 1U, 1U };
    client::PlayerClient player_client{ shared::WorldMode::Flight };
    auto const tile = client::PlayerClientTestAccess::seedLodNeighborhood(player_client, KEY, REVISION, 20U);
    client::PlayerClientTestAccess::initializePreviewMeshFrontier(player_client, tile->key(), 1U);
    ASSERT_TRUE(client::PlayerClientTestAccess::publishLodTile(
        player_client, tile, std::chrono::steady_clock::now() + std::chrono::seconds{ 2 }
    ));
    client::PlayerClientTestAccess::initializePreviewMeshFrontier(player_client, KEY, 1U);
    ASSERT_EQ(client::PlayerClientTestAccess::rendererMeshCount(player_client), 1U);
    uint64_t const storage_bytes = client::PlayerClientTestAccess::baseStorageBytes(player_client, tile->key());
    uint64_t const submissions_before_remove =
        client::PlayerClientTestAccess::cacheRetirementSubmissionCount(player_client);
    ASSERT_GT(storage_bytes, 0U);
    ASSERT_EQ(client::PlayerClientTestAccess::retiredPreviewCacheHighWaterBytes(player_client), 0U);

    client::PlayerClientTestAccess::removeLodTile(player_client, tile->key());

    EXPECT_FALSE(client::PlayerClientTestAccess::currentMeshVisible(player_client, tile->key()));
    EXPECT_FALSE(client::PlayerClientTestAccess::previewCacheContains(player_client, tile->key()));
    EXPECT_EQ(client::PlayerClientTestAccess::rendererMeshCount(player_client), 0U);
    EXPECT_GE(client::PlayerClientTestAccess::retiredPreviewCacheHighWaterBytes(player_client), storage_bytes);
    EXPECT_EQ(client::PlayerClientTestAccess::cacheRetirementSubmissionCount(player_client),
        submissions_before_remove + 1U);
    EXPECT_EQ(client::PlayerClientTestAccess::cacheRetirementFallbackCount(player_client), 0U);
    EXPECT_TRUE(client::PlayerClientTestAccess::pendingLodMesh(
        player_client,
        shared::normalizeHeightTileKey({ .x = tile->key().x - 1, .y = tile->key().y })
    ));
    client::PlayerClientTestAccess::resetConnectionState(player_client);
    EXPECT_EQ(client::PlayerClientTestAccess::pendingPreviewCacheRetirementBytes(player_client), 0U);
}

TEST(PlayerClientResetTest, ConnectionResetDrainsSeveralPreviewCacheBatches)
{
    static constexpr uint32_t TILE_COUNT = 257U;
    static constexpr client::HeightTileRevision REVISION{ 1U, 1U };
    static constexpr shared::HeightTileKey CENTER{ 200, 0 };
    client::PlayerClient player_client{ shared::WorldMode::Flight };
    client::PlayerClientTestAccess::initializePreviewMeshFrontier(player_client, CENTER, TILE_COUNT - 1U);
    std::array<client::HeightTileHandle, TILE_COUNT> tiles;
    for (uint32_t index = 0U; index < TILE_COUNT; ++index) {
        shared::HeightTileKey const key{ CENTER.x + static_cast<int32_t>(index), CENTER.y };
        tiles[index] = client::PlayerClientTestAccess::seedLodTile(player_client, key, REVISION, 20U);
    }
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 2 };
    for (client::HeightTileHandle const& tile : tiles) {
        ASSERT_TRUE(client::PlayerClientTestAccess::publishLodTile(player_client, tile, deadline));
        ASSERT_TRUE(client::PlayerClientTestAccess::previewCacheContains(player_client, tile->key()));
    }
    ASSERT_EQ(client::PlayerClientTestAccess::rendererMeshCount(player_client), TILE_COUNT);

    client::PlayerClientTestAccess::resetConnectionState(player_client);

    EXPECT_EQ(client::PlayerClientTestAccess::rendererMeshCount(player_client), 0U);
    EXPECT_GE(client::PlayerClientTestAccess::cacheRetirementSubmissionCount(player_client), 3U);
    EXPECT_EQ(client::PlayerClientTestAccess::cacheRetirementFallbackCount(player_client), 0U);
    EXPECT_EQ(client::PlayerClientTestAccess::pendingPreviewCacheRetirementBytes(player_client), 0U);
    EXPECT_TRUE(client::PlayerClientTestAccess::previewSetsEmpty(player_client));
}

TEST(PlayerClientResetTest, OlderInstalledNeighborBoundsSurviveRejectedReplacementAndInvalidateOnPublication)
{
    static constexpr shared::HeightTileKey KEY{ .x = 200, .y = 0 };
    static constexpr client::HeightTileRevision FIRST{ .generation = 1U, .revision = 1U };
    static constexpr client::HeightTileRevision NEXT{ .generation = 2U, .revision = 2U };
    static constexpr uint16_t OLD_HEIGHT = 512U;
    static constexpr uint16_t NEW_HEIGHT = 1'021U;
    client::PlayerClient player_client{ shared::WorldMode::Flight };
    client::HeightTileHandle const old_east = client::PlayerClientTestAccess::seedLodNeighborhood(
        player_client, KEY, FIRST, OLD_HEIGHT
    );
    client::PlayerClientTestAccess::initializePreviewMeshFrontier(player_client, old_east->key(), 1U);
    ASSERT_TRUE(client::PlayerClientTestAccess::publishLodTile(
        player_client, old_east, std::chrono::steady_clock::now() + std::chrono::seconds{ 2 }
    ));
    client::PlayerClientTestAccess::initializePreviewMeshFrontier(player_client, KEY, 1U);
    ASSERT_EQ(client::PlayerClientTestAccess::rendererMeshCount(player_client), 1U);
    auto const old_edge_profile = client::PlayerClientTestAccess::installedProfile(
        player_client,
        old_east->key(),
        0U
    );
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
    EXPECT_EQ(
        client::PlayerClientTestAccess::installedProfile(player_client, old_east->key(), 0U),
        old_edge_profile
    );
    EXPECT_TRUE(client::PlayerClientTestAccess::previewCacheContains(player_client, old_east->key()));
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

TEST(PlayerClientResetTest, InstalledNeighborRemovalDefersRetainedTileRefreshToPriorityQueue)
{
    static constexpr shared::HeightTileKey KEY{ .x = 200, .y = 0 };
    static constexpr client::HeightTileRevision FIRST{ .generation = 1U, .revision = 1U };
    static constexpr client::HeightTileRevision NEXT{ .generation = 2U, .revision = 2U };
    client::PlayerClient player_client{ shared::WorldMode::Flight };
    client::HeightTileHandle const old_east = client::PlayerClientTestAccess::seedLodNeighborhood(
        player_client, KEY, FIRST, 512U
    );
    client::PlayerClientTestAccess::initializePreviewMeshFrontier(player_client, old_east->key(), 1U);
    ASSERT_TRUE(client::PlayerClientTestAccess::publishLodTile(
        player_client, old_east, std::chrono::steady_clock::now() + std::chrono::seconds{ 2 }
    ));
    client::PlayerClientTestAccess::initializePreviewMeshFrontier(player_client, KEY, 1U);
    static_cast<void>(client::PlayerClientTestAccess::seedLodNeighborhood(player_client, KEY, NEXT, 1'021U));
    EXPECT_EQ(client::PlayerClientTestAccess::lodDetail(player_client, KEY), shared::HeightTileSurfaceDetail::Fine);

    client::PlayerClientTestAccess::removeLodTile(player_client, old_east->key());

    EXPECT_TRUE(client::PlayerClientTestAccess::pendingLodMesh(player_client, KEY));
    EXPECT_EQ(
        client::PlayerClientTestAccess::selectedLodDetail(player_client, KEY), shared::HeightTileSurfaceDetail::Fine
    );
}

TEST(PlayerClientResetTest, NeighborDetailSnapshotDoesNotSelectOrMutateUnknownNeighbors)
{
    static constexpr shared::HeightTileKey KEY{ 200, 0 };
    static constexpr shared::HeightTileKey EAST{ 201, 0 };
    static constexpr client::HeightTileRevision REVISION{ 1U, 1U };
    static constexpr uint16_t HEIGHT = 1'021U;
    static constexpr std::array<shared::HeightTileSurfaceDetail, 4> FINE_DETAILS{
        shared::HeightTileSurfaceDetail::Fine, shared::HeightTileSurfaceDetail::Fine,
        shared::HeightTileSurfaceDetail::Fine, shared::HeightTileSurfaceDetail::Fine,
    };
    client::PlayerClient player_client{ shared::WorldMode::Flight };
    static_cast<void>(client::PlayerClientTestAccess::seedLodNeighborhood(player_client, KEY, REVISION, HEIGHT));
    EXPECT_EQ(client::PlayerClientTestAccess::selectedLodDetail(player_client, EAST), std::nullopt);
    EXPECT_EQ(client::PlayerClientTestAccess::neighborDetails(player_client, KEY), FINE_DETAILS);
    EXPECT_EQ(client::PlayerClientTestAccess::selectedLodDetail(player_client, EAST), std::nullopt);
}

TEST(PlayerClientResetTest, InstalledTopIdentityIsStableUntilAChangedProfileSuccessfullyPublishes)
{
    static constexpr shared::HeightTileKey KEY{ 200, 0 };
    static constexpr shared::HeightTileKey EAST{ 201, 0 };
    static constexpr client::HeightTileRevision REVISION{ 1U, 1U };
    static constexpr uint16_t HEIGHT = 8U;
    static constexpr uint32_t SIDE = shared::HeightTile::SIDE_LENGTH;
    client::PlayerClient player_client{ shared::WorldMode::Flight };
    static_cast<void>(client::PlayerClientTestAccess::seedLodNeighborhood(player_client, KEY, REVISION, HEIGHT));
    ASSERT_TRUE(client::PlayerClientTestAccess::publishLodTile(
        player_client,
        client::PlayerClientTestAccess::residentLodTile(player_client, KEY),
        std::chrono::steady_clock::now() + std::chrono::seconds{ 2 }
    ));
    shared::HeightTile::Heights heights;
    for (uint32_t y = 0U; y < SIDE; ++y) {
        uint16_t const height = y < 3U ? 2U : y == 3U ? 6U : y < 14U ? 10U : 16U;
        for (uint32_t x = 0U; x < SIDE; ++x) {
            heights[y * SIDE + x] = height;
        }
    }
    auto const east = client::PlayerClientTestAccess::replaceLodHeights(player_client, EAST, heights);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 2 };
    client::PlayerClientTestAccess::publishLodTile(player_client, east, deadline);
    auto const fine_profile = client::PlayerClientTestAccess::installedProfile(player_client, EAST, 0U);
    auto const first_tokens = client::PlayerClientTestAccess::neighborProfileTokens(player_client, KEY);
    ASSERT_NE(first_tokens[1U], 0U);
    client::PlayerClientTestAccess::publishLodTile(player_client, east, deadline);
    EXPECT_EQ(client::PlayerClientTestAccess::neighborProfileTokens(player_client, KEY), first_tokens);
    client::PlayerClientTestAccess::publishLodTile(player_client, east,
        std::chrono::steady_clock::now() - std::chrono::seconds{ 1 }, shared::HeightTileSurfaceDetail::Coarse2);
    EXPECT_EQ(client::PlayerClientTestAccess::neighborProfileTokens(player_client, KEY), first_tokens);
    EXPECT_EQ(client::PlayerClientTestAccess::installedProfile(player_client, EAST, 0U), fine_profile);
    client::PlayerClientTestAccess::publishLodTile(player_client, east, deadline,
        shared::HeightTileSurfaceDetail::Coarse2);
    auto const coarse_profile = client::PlayerClientTestAccess::installedProfile(player_client, EAST, 0U);
    auto const changed_tokens = client::PlayerClientTestAccess::neighborProfileTokens(player_client, KEY);
    EXPECT_EQ(coarse_profile.elevation, fine_profile.elevation);
    EXPECT_NE(coarse_profile.top, fine_profile.top);
    EXPECT_NE(changed_tokens[1U], first_tokens[1U]);
    EXPECT_EQ(client::PlayerClientTestAccess::cachedNeighborProfileTokens(player_client, KEY), changed_tokens);
    client::PlayerClientTestAccess::publishLodTile(player_client, east, deadline,
        shared::HeightTileSurfaceDetail::Coarse2);
    EXPECT_EQ(client::PlayerClientTestAccess::neighborProfileTokens(player_client, KEY), changed_tokens);
    client::PlayerClientTestAccess::removeLodTile(player_client, EAST);
    EXPECT_EQ(client::PlayerClientTestAccess::neighborProfileTokens(player_client, KEY)[1U], 0U);
}

TEST(PlayerClientResetTest, StationaryNeighborhoodQueuesDrainAfterStableTopRepublishing)
{
    static constexpr shared::HeightTileKey KEY{ 200, 0 };
    static constexpr client::HeightTileRevision REVISION{ 1U, 1U };
    static constexpr uint16_t HEIGHT = 1'021U;
    static constexpr uint32_t TILES = 5U;
    client::PlayerClient player_client{ shared::WorldMode::Flight };
    static_cast<void>(client::PlayerClientTestAccess::seedLodNeighborhood(player_client, KEY, REVISION, HEIGHT));
    ASSERT_EQ(client::PlayerClientTestAccess::lodDetail(player_client, KEY), shared::HeightTileSurfaceDetail::Coarse2);
    ASSERT_TRUE(client::PlayerClientTestAccess::drainLodNeighborhood(player_client, KEY));
    EXPECT_EQ(client::PlayerClientTestAccess::rendererMeshCount(player_client), TILES);
    auto const tokens = client::PlayerClientTestAccess::neighborProfileTokens(player_client, KEY);
    ASSERT_TRUE(client::PlayerClientTestAccess::drainLodNeighborhood(player_client, KEY));
    EXPECT_EQ(client::PlayerClientTestAccess::neighborProfileTokens(player_client, KEY), tokens);
    EXPECT_EQ(client::PlayerClientTestAccess::rendererMeshCount(player_client), TILES);
}

TEST(PlayerClientResetTest, ReadyFarMeshWaitsForItsConnectingMeshToPublish)
{
    static constexpr shared::HeightTileKey CENTER{ 200, 0 };
    static constexpr shared::HeightTileKey BRIDGE{ 201, 0 };
    static constexpr shared::HeightTileKey FAR{ 202, 0 };
    static constexpr client::HeightTileRevision REVISION{ 1U, 1U };
    client::PlayerClient player_client{ shared::WorldMode::Flight };
    client::HeightTileHandle const bridge = client::PlayerClientTestAccess::seedLodNeighborhood(
        player_client, CENTER, REVISION, 512U
    );
    static_cast<void>(client::PlayerClientTestAccess::seedLodTile(player_client, FAR, REVISION, 512U));
    client::PlayerClientTestAccess::initializePreviewMeshFrontier(player_client, CENTER, 4U);

    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 2 };
    ASSERT_TRUE(client::PlayerClientTestAccess::publishLodTile(
        player_client, client::PlayerClientTestAccess::residentLodTile(player_client, CENTER), deadline
    ));
    ASSERT_TRUE(client::PlayerClientTestAccess::currentMeshVisible(player_client, CENTER));
    ASSERT_FALSE(client::PlayerClientTestAccess::currentMeshVisible(player_client, BRIDGE));

    client::PlayerClientTestAccess::queueLodMesh(player_client, FAR);
    ASSERT_TRUE(client::PlayerClientTestAccess::waitingForConnectedLodMesh(player_client, FAR));
    EXPECT_FALSE(client::PlayerClientTestAccess::pendingLodMesh(player_client, FAR));
    ASSERT_TRUE(client::PlayerClientTestAccess::prepareCompletedLodJobFromDisconnectedTile(
        player_client, FAR, CENTER
    ));
    client::PlayerClientTestAccess::processOneLodResult(player_client);
    EXPECT_FALSE(client::PlayerClientTestAccess::currentMeshVisible(player_client, FAR));
    EXPECT_TRUE(client::PlayerClientTestAccess::waitingForConnectedLodMesh(player_client, FAR));

    ASSERT_TRUE(client::PlayerClientTestAccess::publishLodTile(
        player_client, bridge, std::chrono::steady_clock::now() + std::chrono::seconds{ 2 }
    ));
    EXPECT_TRUE(client::PlayerClientTestAccess::pendingLodMesh(player_client, FAR));
    EXPECT_FALSE(client::PlayerClientTestAccess::waitingForConnectedLodMesh(player_client, FAR));
    ASSERT_TRUE(client::PlayerClientTestAccess::waitForVisibleMesh(player_client, FAR));
    EXPECT_TRUE(client::PlayerClientTestAccess::currentMeshVisible(player_client, FAR));
}

TEST(PlayerClientResetTest, ProfileOnlyPublicationReusesCurrentCompletedGeometry)
{
    static constexpr shared::HeightTileKey KEY{ 200, 0 };
    static constexpr client::HeightTileRevision REVISION{ 1U, 1U };
    static constexpr uint16_t HEIGHT = 1'021U;
    client::PlayerClient player_client{ shared::WorldMode::Flight };
    auto const east = client::PlayerClientTestAccess::seedLodNeighborhood(player_client, KEY, REVISION, HEIGHT);
    ASSERT_TRUE(client::PlayerClientTestAccess::publishLodTile(
        player_client,
        client::PlayerClientTestAccess::residentLodTile(player_client, KEY),
        std::chrono::steady_clock::now() + std::chrono::seconds{ 2 }
    ));
    for (shared::HeightTileKey const neighbor_key : {
             shared::normalizeHeightTileKey({ .x = KEY.x - 1, .y = KEY.y }),
             shared::normalizeHeightTileKey({ .x = KEY.x, .y = KEY.y - 1 }),
             shared::normalizeHeightTileKey({ .x = KEY.x, .y = KEY.y + 1 }),
         }) {
        client::PlayerClientTestAccess::publishResidentLodTile(player_client, neighbor_key);
    }
    ASSERT_TRUE(client::PlayerClientTestAccess::prepareCompletedLodJob(player_client, KEY));
    ASSERT_EQ(client::PlayerClientTestAccess::meshAdmissionCount(player_client), 1U);
    ASSERT_EQ(client::PlayerClientTestAccess::selectedLodDetail(player_client, KEY),
        shared::HeightTileSurfaceDetail::Coarse2);
    auto const previous_tokens = client::PlayerClientTestAccess::neighborProfileTokens(player_client, KEY);

    client::PlayerClientTestAccess::setInterestAroundTile(player_client, east->key());
    ASSERT_TRUE(client::PlayerClientTestAccess::publishLodTile(
        player_client, east, std::chrono::steady_clock::now() + std::chrono::seconds{ 2 }
    ));
    EXPECT_NE(client::PlayerClientTestAccess::neighborProfileTokens(player_client, KEY), previous_tokens);
    ASSERT_EQ(client::PlayerClientTestAccess::selectedLodDetail(player_client, KEY),
        shared::HeightTileSurfaceDetail::Coarse2);
    client::PlayerClientTestAccess::setInterestAroundTile(player_client, KEY);
    client::PlayerClientTestAccess::processOneIsolatedLodResult(player_client);

    EXPECT_EQ(client::PlayerClientTestAccess::meshResultCount(player_client), 1U);
    ASSERT_TRUE(client::PlayerClientTestAccess::currentMeshVisible(player_client, KEY));
    EXPECT_EQ(client::PlayerClientTestAccess::installedDetail(player_client, KEY),
        shared::HeightTileSurfaceDetail::Coarse2);
    EXPECT_EQ(client::PlayerClientTestAccess::meshAdmissionCount(player_client), 1U);
    EXPECT_EQ(player_client.benchmarkFullTerrainStreamingInfo().worker_outstanding_count, 0U);
}

TEST(PlayerClientResetTest, ReportsCompletedNearMeshWaitingForTheClientDrain)
{
    static constexpr shared::HeightTileKey KEY{ 200, 0 };
    static constexpr client::HeightTileRevision REVISION{ 1U, 1U };
    client::PlayerClient player_client{
        shared::WorldMode::Flight,
        std::nullopt,
        client::PlayerClientBenchmarkOptions{ .enable_mesh_stage_diagnostics = true },
    };
    client::PlayerClientTestAccess::initializeInterest(player_client, 1U);
    static_cast<void>(client::PlayerClientTestAccess::seedLodNeighborhood(player_client, KEY, REVISION, 512U));
    client::PlayerClientTestAccess::moveInterestTo(player_client, KEY.x);
    client::PlayerClientTestAccess::setInterestAroundTile(player_client, KEY);
    ASSERT_TRUE(client::PlayerClientTestAccess::prepareCompletedLodJob(player_client, KEY));

    client::PlayerClientBenchmarkTerrainInfo const info = player_client.benchmarkTerrainStreamingInfo();
    ASSERT_TRUE(info.center.has_value());
    EXPECT_EQ(*info.center, KEY);
    EXPECT_EQ(info.resident_count, 5U);
    EXPECT_EQ(player_client.benchmarkFullTerrainStreamingInfo().worker_result_count, 1U);

    EXPECT_EQ(info.completed_result_waiting_count, 1U);
    EXPECT_EQ(info.client_pending_admission_count, 0U);
    EXPECT_EQ(info.worker_queued_count, 0U);
    EXPECT_EQ(info.worker_building_count, 0U);
    EXPECT_GE(info.oldest_completed_result_waiting_age, std::chrono::nanoseconds::zero());
}

TEST(PlayerClientResetTest, CurrentNearCompletedMeshPrecedesAnOlderFarResultAfterTravel)
{
    static constexpr shared::HeightTileKey FAR_KEY{ 15, 0 };
    static constexpr shared::HeightTileKey NEAR_KEY{ 8, 0 };
    static constexpr client::HeightTileRevision REVISION{ 1U, 1U };
    client::PlayerClient player_client{ shared::WorldMode::Flight };
    client::PlayerClientTestAccess::initializeInterest(player_client, 20U);
    static_cast<void>(client::PlayerClientTestAccess::seedLodNeighborhood(
        player_client, FAR_KEY, REVISION, 512U
    ));
    ASSERT_TRUE(client::PlayerClientTestAccess::prepareCompletedLodJob(player_client, FAR_KEY));

    static_cast<void>(client::PlayerClientTestAccess::seedLodNeighborhood(
        player_client, NEAR_KEY, REVISION, 512U
    ));
    client::PlayerClientTestAccess::setInterestAroundTile(player_client, FAR_KEY);
    client::PlayerClientTestAccess::moveInterestTo(player_client, NEAR_KEY.x);
    ASSERT_TRUE(client::PlayerClientTestAccess::prepareNearCompletedLodJob(
        player_client, NEAR_KEY, FAR_KEY
    ));

    client::PlayerClientTestAccess::processOneLodResult(player_client);

    EXPECT_TRUE(client::PlayerClientTestAccess::currentMeshVisible(player_client, NEAR_KEY));
}

TEST(PlayerClientResetTest, CompletedMeshSurvivesAnExpiredPublicationDeadline)
{
    static constexpr shared::HeightTileKey KEY{ 200, 0 };
    client::PlayerClient player_client{ shared::WorldMode::Flight };
    static_cast<void>(client::PlayerClientTestAccess::seedLodNeighborhood(
        player_client, KEY, { 1U, 1U }, 1'021U
    ));
    ASSERT_TRUE(client::PlayerClientTestAccess::publishLodTile(
        player_client,
        client::PlayerClientTestAccess::residentLodTile(player_client, KEY),
        std::chrono::steady_clock::now() + std::chrono::seconds{ 2 }
    ));
    for (shared::HeightTileKey const neighbor_key : {
             shared::normalizeHeightTileKey({ .x = KEY.x - 1, .y = KEY.y }),
             shared::normalizeHeightTileKey({ .x = KEY.x + 1, .y = KEY.y }),
             shared::normalizeHeightTileKey({ .x = KEY.x, .y = KEY.y - 1 }),
             shared::normalizeHeightTileKey({ .x = KEY.x, .y = KEY.y + 1 }),
         }) {
        client::PlayerClientTestAccess::publishResidentLodTile(player_client, neighbor_key);
    }
    ASSERT_TRUE(client::PlayerClientTestAccess::prepareCompletedLodJob(player_client, KEY));
    ASSERT_EQ(player_client.benchmarkFullTerrainStreamingInfo().worker_result_count, 1U);
    ASSERT_EQ(player_client.benchmarkFullTerrainStreamingInfo().worker_outstanding_count, 1U);
    ASSERT_EQ(client::PlayerClientTestAccess::meshAdmissionCount(player_client), 1U);

    auto const expired_deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds{ 1 };
    client::PlayerClientTestAccess::processOneLodResult(player_client, expired_deadline);
    auto const deferred = player_client.benchmarkFullTerrainStreamingInfo();
    EXPECT_EQ(deferred.worker_result_count, 1U);
    EXPECT_EQ(deferred.worker_outstanding_count, 1U);
    EXPECT_EQ(deferred.pending_mesh_count, 0U);
    EXPECT_EQ(deferred.active_mesh_job_count, 1U);
    EXPECT_EQ(client::PlayerClientTestAccess::meshAdmissionCount(player_client), 1U);

    client::PlayerClientTestAccess::processOneLodResult(player_client, expired_deadline);
    EXPECT_EQ(player_client.benchmarkFullTerrainStreamingInfo().worker_result_count, 1U);
    EXPECT_EQ(client::PlayerClientTestAccess::meshAdmissionCount(player_client), 1U);

    client::PlayerClientTestAccess::processOneIsolatedLodResult(player_client);
    auto const published = player_client.benchmarkFullTerrainStreamingInfo();
    EXPECT_EQ(published.worker_result_count, 0U);
    EXPECT_EQ(published.worker_outstanding_count, 0U);
    EXPECT_EQ(published.pending_mesh_count, 0U);
    EXPECT_EQ(published.active_mesh_job_count, 0U);
    EXPECT_EQ(client::PlayerClientTestAccess::meshAdmissionCount(player_client), 1U);
    EXPECT_TRUE(client::PlayerClientTestAccess::currentMeshVisible(player_client, KEY));
}

TEST(PlayerClientResetTest, ChangedInstalledProfileRejectsUnsafeCompletedCoarseGeometry)
{
    static constexpr shared::HeightTileKey KEY{ 200, 0 };
    static constexpr client::HeightTileRevision FIRST{ 1U, 1U };
    static constexpr client::HeightTileRevision NEXT{ 2U, 2U };
    client::PlayerClient player_client{ shared::WorldMode::Flight };
    auto const old_east = client::PlayerClientTestAccess::seedLodNeighborhood(player_client, KEY, FIRST, 512U);
    ASSERT_TRUE(client::PlayerClientTestAccess::publishLodTile(
        player_client,
        client::PlayerClientTestAccess::residentLodTile(player_client, KEY),
        std::chrono::steady_clock::now() + std::chrono::seconds{ 2 }
    ));
    static_cast<void>(client::PlayerClientTestAccess::seedLodNeighborhood(player_client, KEY, NEXT, 1'021U));
    ASSERT_TRUE(client::PlayerClientTestAccess::prepareCompletedLodJob(player_client, KEY));
    ASSERT_EQ(client::PlayerClientTestAccess::meshAdmissionCount(player_client), 1U);
    ASSERT_EQ(client::PlayerClientTestAccess::selectedLodDetail(player_client, KEY),
        shared::HeightTileSurfaceDetail::Coarse2);

    ASSERT_TRUE(client::PlayerClientTestAccess::publishLodTile(
        player_client, old_east, std::chrono::steady_clock::now() + std::chrono::seconds{ 2 }
    ));
    ASSERT_EQ(client::PlayerClientTestAccess::selectedLodDetail(player_client, KEY),
        shared::HeightTileSurfaceDetail::Fine);
    client::PlayerClientTestAccess::processOneLodResult(player_client);

    EXPECT_EQ(client::PlayerClientTestAccess::meshResultCount(player_client), 1U);
    EXPECT_FALSE(client::PlayerClientTestAccess::currentMeshVisible(player_client, KEY));
    EXPECT_GT(client::PlayerClientTestAccess::meshAdmissionCount(player_client), 1U);
    ASSERT_TRUE(client::PlayerClientTestAccess::waitForVisibleMesh(player_client, KEY));
    EXPECT_EQ(client::PlayerClientTestAccess::installedDetail(player_client, KEY),
        shared::HeightTileSurfaceDetail::Fine);
}
