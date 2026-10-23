#include <client/ColumnMapClient.hpp>
#include <client/ColumnMapPresentation.hpp>
#include <client/GameClient.hpp>
#include <client/WorldLoading.hpp>
#include <server/GameServer.hpp>
#include <server/WorldPreparation.hpp>
#include <shared/net/Message.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldCoverage.hpp>
#include <generation/Generation.hpp>
#include <core/common/Defer.hpp>
#include <core/net/Address.hpp>
#include <core/net/Event.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

std::shared_ptr<server::WorldPreparation> productionTestPreparation(
    shared::WorldConfiguration const& world_configuration
)
{
    generation::GenerationConfiguration configuration{};
    configuration.seed = world_configuration.seed;
    return std::make_shared<server::WorldPreparation>(configuration, world_configuration.algorithm_version);
}

std::shared_ptr<shared::TerrainWorld const> waitForPreparedTerrainWorld(
    server::WorldPreparation& preparation,
    std::chrono::steady_clock::time_point const deadline
)
{
    static constexpr std::chrono::milliseconds POLL_INTERVAL{ 1 };
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::shared_ptr<shared::TerrainWorld const> world = preparation.readyWorld()) {
            return world;
        }
        if (preparation.latest().state != server::WorldPreparationState::Preparing) {
            return preparation.readyWorld();
        }
        std::this_thread::sleep_for(POLL_INTERVAL);
    }
    return preparation.readyWorld();
}

std::unique_ptr<shared::SurfaceTile> sampleSurfaceOnHeap(
    shared::TerrainWorld const& terrain,
    shared::ChunkCoordinate const coordinate
)
{
    auto surface = std::make_unique<shared::SurfaceTile>();
    *surface = terrain.sampleSurface(coordinate);
    return surface;
}

bool allMapColumnsReady(client::ColumnMapClient const& game_client)
{
    std::vector<client::ColumnCoverage> const& coverage = game_client.presentation().planes().coverage;
    return !coverage.empty() && std::ranges::all_of(
        coverage,
        [](client::ColumnCoverage const column) { return column == client::ColumnCoverage::Ready; }
    );
}

bool allMapColumnsInActiveCoverage(client::ColumnMapClient const& game_client)
{
    std::vector<uint8_t> const& presence = game_client.frameState().active_materialized_column_presence;
    return presence.size() == game_client.presentation().planes().coverage.size()
        && std::ranges::all_of(presence, [](uint8_t const covered) { return covered == 1U; });
}

char const* worldLoadingStateName(client::WorldLoadingState const state) noexcept
{
    switch (state) {
    case client::WorldLoadingState::Idle: return "Idle";
    case client::WorldLoadingState::Creating: return "Creating";
    case client::WorldLoadingState::PreparingAtlas: return "PreparingAtlas";
    case client::WorldLoadingState::PreparingSpawn: return "PreparingSpawn";
    case client::WorldLoadingState::ReadyForStart: return "ReadyForStart";
    case client::WorldLoadingState::Starting: return "Starting";
    case client::WorldLoadingState::Playing: return "Playing";
    case client::WorldLoadingState::Failed: return "Failed";
    case client::WorldLoadingState::Cancelled: return "Cancelled";
    }
    return "Unknown";
}

client::WorldWindow windowCenteredOnPosition(
    shared::PlayerPosition const position,
    uint16_t const width,
    uint16_t const height
)
{
    int64_t const player_x = static_cast<int64_t>(position.x)
        + static_cast<int64_t>(position.x_subcell / shared::SUBCELLS_PER_CELL);
    int64_t const player_y = static_cast<int64_t>(position.y)
        + static_cast<int64_t>(position.y_subcell / shared::SUBCELLS_PER_CELL);
    return {
        .origin_x = player_x - static_cast<int64_t>(width / 2U),
        .origin_y = player_y - static_cast<int64_t>(height / 2U),
        .width = width,
        .height = height,
    };
}

void expectCanonicalMapPixels(
    client::ColumnMapClient const& game_client,
    shared::TerrainWorld const& terrain_world
)
{
    client::WorldWindow const& window = game_client.presentation().window();
    client::ColumnMapPlanes const& planes = game_client.presentation().planes();
    uint32_t const column_count = static_cast<uint32_t>(planes.coverage.size());
    std::vector<shared::SurfaceTile> expected_tiles;
    client::ColumnMapPalette const palette = client::canonicalColumnMapPalette();

    ASSERT_TRUE(game_client.frameState().terrain_identity.has_value());
    EXPECT_EQ(*game_client.frameState().terrain_identity, terrain_world.identity());
    ASSERT_TRUE(allMapColumnsReady(game_client));
    ASSERT_EQ(planes.material_ids.size(), column_count);
    ASSERT_EQ(planes.highest_solid_z.size(), column_count);
    ASSERT_EQ(planes.rgba.size(), column_count);

    for (uint32_t index = 0U; index < column_count; ++index) {
        uint32_t const x_offset = index % window.width;
        uint32_t const y_offset = index / window.width;
        uint32_t const world_x = (shared::WorldBounds::wrapHorizontal(window.origin_x) + x_offset)
            % shared::WorldExtent::WIDTH;
        uint32_t const world_y = (shared::WorldBounds::wrapHorizontal(window.origin_y) + y_offset)
            % shared::WorldExtent::WIDTH;
        shared::ChunkCoordinate const tile_coordinate{
            .x = static_cast<int32_t>(world_x / shared::Chunk::SIDE_LENGTH),
            .y = static_cast<int32_t>(world_y / shared::Chunk::SIDE_LENGTH),
            .z = 0,
        };
        auto tile = std::ranges::find_if(expected_tiles, [tile_coordinate](shared::SurfaceTile const& candidate) {
            return candidate.coordinate == tile_coordinate;
        });
        if (tile == expected_tiles.end()) {
            expected_tiles.push_back(terrain_world.sampleSurface(tile_coordinate));
            tile = std::ranges::find_if(expected_tiles, [tile_coordinate](shared::SurfaceTile const& candidate) {
                return candidate.coordinate == tile_coordinate;
            });
        }

        uint32_t const local_x = world_x % shared::Chunk::SIDE_LENGTH;
        uint32_t const local_y = world_y % shared::Chunk::SIDE_LENGTH;
        shared::TerrainColumn const& column = tile->columns[local_y * shared::Chunk::SIDE_LENGTH + local_x];
        uint8_t const expected_material = static_cast<uint8_t>(
            column.materialAt(column.highest_solid_z, tile->block_sea_level)
        );
        EXPECT_EQ(planes.material_ids[index], expected_material) << "column index " << index;
        EXPECT_EQ(planes.highest_solid_z[index], static_cast<int16_t>(column.highest_solid_z))
            << "column index " << index;
        EXPECT_EQ(planes.rgba[index], palette.colors[expected_material]) << "column index " << index;
    }
}

} // namespace

TEST(ColumnMapClientIntegrationTest, FollowsWrappedMovementAndReplacesConnectedCoverageOnReroot)
{
    static constexpr std::chrono::seconds PREPARATION_DEADLINE{ 15 };
    static constexpr std::chrono::seconds CLIENT_DEADLINE{ 30 };
    static constexpr uint16_t MAP_SIDE = 4U;
    static constexpr uint8_t FULL_AXIS_INPUT = 127U;
    static constexpr uint16_t REROOT_SUBCELL = shared::SUBCELLS_PER_CELL / 2U;
    static constexpr uint32_t PREVIEW_RADIUS = 2U;
    static constexpr uint32_t PREFETCH_MARGIN = shared::MATERIALIZED_PREFETCH_MARGIN_BLOCKS;
    static constexpr int32_t SPAWN_X = shared::World::FLIGHT_MAX_CELL;
    static constexpr int32_t SPAWN_Y = 32'768;
    static constexpr int32_t REROOT_X = shared::NEAR_TERRAIN_RADIUS_BLOCKS + 8U;
    static constexpr char CHARACTER = '@';

    shared::WorldConfiguration const world_configuration = shared::World::canonicalConfiguration();
    std::shared_ptr<server::WorldPreparation> const preparation = productionTestPreparation(world_configuration);
    std::shared_ptr<shared::TerrainWorld const> const terrain_world = waitForPreparedTerrainWorld(
        *preparation,
        std::chrono::steady_clock::now() + PREPARATION_DEADLINE
    );
    ASSERT_NE(terrain_world, nullptr);

    auto const spawn_surface = sampleSurfaceOnHeap(
        *terrain_world,
        {
            .x = SPAWN_X / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH),
            .y = SPAWN_Y / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH),
            .z = 0,
        }
    );
    uint32_t const spawn_column_index = static_cast<uint32_t>(SPAWN_Y)
            % shared::Chunk::SIDE_LENGTH * shared::Chunk::SIDE_LENGTH
        + static_cast<uint32_t>(SPAWN_X) % shared::Chunk::SIDE_LENGTH;
    shared::PlayerPosition const spawn_position{
        .x = SPAWN_X,
        .y = SPAWN_Y,
        .z = static_cast<int32_t>(spawn_surface->columns[spawn_column_index].highest_solid_z) + 1,
    };
    ASSERT_TRUE(shared::World::isFlightPositionInBounds(spawn_position));
    shared::PlayerPosition const reroot_position{
        .x = REROOT_X,
        .y = SPAWN_Y,
        .z = spawn_position.z,
        .x_subcell = REROOT_SUBCELL,
        .y_subcell = REROOT_SUBCELL,
    };
    client::WorldWindow const initial_window{
        .origin_x = static_cast<int64_t>(SPAWN_X) - 1,
        .origin_y = static_cast<int64_t>(SPAWN_Y) - static_cast<int64_t>(MAP_SIDE / 2U),
        .width = MAP_SIDE,
        .height = MAP_SIDE,
    };
    client::WorldWindow const reroot_window = windowCenteredOnPosition(reroot_position, MAP_SIDE, MAP_SIDE);
    server::GameServer server{
        0U,
        { server::GameServer::SpawnPoint{ .character = CHARACTER, .x = SPAWN_X, .y = SPAWN_Y,
            .z = spawn_position.z } },
        shared::WorldMode::Flight,
        world_configuration,
        // Chunk near/full coverage is governed separately from height-tile interest.
        PREVIEW_RADIUS,
        nullptr,
        preparation,
    };
    std::atomic_bool stop_server{ false };
    std::atomic_bool has_player_id{ false };
    std::atomic<shared::PlayerId> player_id{ 0U };
    std::atomic<int32_t> previous_authoritative_x{ -1 };
    std::atomic_bool crossed_wrap{ false };
    std::atomic_bool wrapped_frame_captured{ false };
    std::atomic_bool lead_prefetch_observed{ false };
    std::atomic_bool lead_promotion_observed{ false };
    std::atomic<uint64_t> wrapped_coverage_epoch{ 0U };
    std::atomic<int32_t> wrapped_coverage_center_x{ -1 };
    std::atomic<int32_t> wrapped_coverage_center_y{ -1 };
    std::atomic<uint32_t> last_movement_input_sequence{ 0U };
    std::atomic<uint32_t> acknowledged_input_sequence{ 0U };
    std::atomic_bool reroot_succeeded{ false };
    std::atomic_bool reroot_position_observed{ false };
    std::vector<shared::ChunkCoordinate> lead_promotion_candidates;
    std::optional<shared::PlayerPosition> lead_candidate_center;
    std::optional<shared::PlayerPosition> lead_candidate_interest_center;
    std::optional<shared::ChunkCoordinate> lead_chunk;
    uint64_t lead_coverage_epoch = 0U;
    uint64_t lead_delivery_token = 0U;
    uint32_t lead_packet_occurrences = 0U;
    uint32_t lead_ready_chunks_before = 0U;
    uint32_t lead_ready_chunks_after = 0U;
    bool lead_resident_at_prefetch = false;
    bool lead_was_required_at_prefetch = true;
    bool lead_ready_count_unchanged = false;
    bool lead_is_resident_after_promotion = false;
    bool lead_is_required_after_promotion = false;
    bool lead_epoch_unchanged_on_promotion = false;
    uint32_t lead_packet_occurrences_at_promotion = 0U;
    uint32_t lead_ready_chunks_after_promotion = 0U;
    uint64_t lead_promotion_epoch = 0U;
    auto chunk_less = [](shared::ChunkCoordinate const left, shared::ChunkCoordinate const right) {
        if (left.z != right.z) {
            return left.z < right.z;
        }
        if (left.y != right.y) {
            return left.y < right.y;
        }
        return left.x < right.x;
    };
    auto same_position = [](shared::PlayerPosition const left, shared::PlayerPosition const right) {
        return left.x == right.x
            && left.y == right.y
            && left.z == right.z
            && left.x_subcell == right.x_subcell
            && left.y_subcell == right.y_subcell
            && left.z_subcell == right.z_subcell;
    };
    auto refresh_lead_candidates = [&](
        shared::PlayerPosition const ready_center,
        shared::PlayerPosition const interest_center
    ) {
        std::vector<shared::ChunkCoordinate> const required_now = shared::requiredChunkKeys(
            ready_center,
            shared::NEAR_TERRAIN_RADIUS_BLOCKS
        );
        EXPECT_FALSE(required_now.empty());
        std::vector<shared::ChunkCoordinate> const interest_now = shared::materializedInterestChunkKeys(
            interest_center
        );
        shared::PlayerPosition future_center = ready_center;
        future_center.x = static_cast<int32_t>(shared::WorldBounds::wrapHorizontal(
            static_cast<int64_t>(ready_center.x) + PREFETCH_MARGIN
        ));
        std::vector<shared::ChunkCoordinate> const required_after_move = shared::requiredChunkKeys(
            future_center,
            shared::NEAR_TERRAIN_RADIUS_BLOCKS
        );
        EXPECT_FALSE(required_after_move.empty());
        lead_promotion_candidates.clear();
        for (shared::ChunkCoordinate const coordinate : required_after_move) {
            if (std::ranges::binary_search(interest_now, coordinate, chunk_less)
                && !std::ranges::binary_search(required_now, coordinate, chunk_less)) {
                lead_promotion_candidates.push_back(coordinate);
            }
        }
        lead_candidate_center = ready_center;
        lead_candidate_interest_center = interest_center;
    };
    auto ensure_lead_candidates = [&](
        shared::PlayerPosition const ready_center,
        shared::PlayerPosition const interest_center
    ) {
        bool refresh = !lead_candidate_center.has_value();
        if (lead_candidate_center && lead_candidate_interest_center) {
            uint32_t const previous_x = shared::WorldBounds::wrapHorizontal(lead_candidate_center->x);
            uint32_t const current_x = shared::WorldBounds::wrapHorizontal(ready_center.x);
            uint32_t const forward_distance = (current_x + shared::WorldExtent::WIDTH - previous_x)
                % shared::WorldExtent::WIDTH;
            refresh = forward_distance >= PREFETCH_MARGIN
                || !same_position(*lead_candidate_interest_center, interest_center);
        }
        if (refresh) {
            refresh_lead_candidates(ready_center, interest_center);
        }
    };
    std::optional<shared::PlayerPosition> classified_ready_center;
    std::optional<shared::PlayerPosition> classified_interest_center;
    std::vector<shared::ChunkCoordinate> classified_ready_required_keys;
    std::vector<shared::ChunkCoordinate> classified_interest_keys;
    auto ensure_classified_coverage = [&](
        shared::PlayerPosition const ready_center,
        shared::PlayerPosition const interest_center
    ) {
        if (!classified_ready_center || !same_position(*classified_ready_center, ready_center)) {
            classified_ready_required_keys = shared::requiredChunkKeys(
                ready_center,
                shared::NEAR_TERRAIN_RADIUS_BLOCKS
            );
            EXPECT_FALSE(classified_ready_required_keys.empty());
            classified_ready_center = ready_center;
        }
        if (!classified_interest_center || !same_position(*classified_interest_center, interest_center)) {
            classified_interest_keys = shared::materializedInterestChunkKeys(interest_center);
            classified_interest_center = interest_center;
        }
    };
    bool wrapped_position_observed_in_initial_window = false;
    bool wrapped_pixels_match = false;
    bool wrapped_columns_in_coverage = false;
    struct PreviewMetricsSnapshot final {
        std::chrono::steady_clock::time_point sampled_at;
        server::GameServer::PreviewStreamMetrics metrics;
    };
    std::optional<PreviewMetricsSnapshot> first_preview_metrics;
    std::optional<PreviewMetricsSnapshot> last_preview_metrics;

    server::GameServer::BenchmarkHooks const server_hooks{
        .on_tick = [&](std::chrono::nanoseconds, uint64_t) {
            if (!wrapped_frame_captured.load(std::memory_order_acquire)
                || !lead_promotion_observed.load(std::memory_order_acquire)
                || reroot_succeeded.load(std::memory_order_acquire)
                || !has_player_id.load(std::memory_order_acquire)) {
                return;
            }
            if (shared::isNewerSequence(
                    last_movement_input_sequence.load(std::memory_order_acquire),
                    acknowledged_input_sequence.load(std::memory_order_acquire)
                )) {
                return;
            }
            if (server.rerootPlayerCoverage(player_id.load(std::memory_order_acquire), reroot_position)) {
                reroot_succeeded.store(true, std::memory_order_release);
            }
        },
        .on_preview_buffered = {},
        .on_preview_metrics = [&](core::ClientId, server::GameServer::PreviewStreamMetrics metrics) {
            PreviewMetricsSnapshot const snapshot{
                .sampled_at = std::chrono::steady_clock::now(),
                .metrics = metrics,
            };
            if (!first_preview_metrics.has_value()) {
                first_preview_metrics = snapshot;
            }
            last_preview_metrics = snapshot;
        },
        .before_height_tile_completion = {},
    };
    std::thread server_thread{ [&] { server.run(stop_server, &server_hooks); } };
    defer {
        stop_server.store(true, std::memory_order_release);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };

    client::ColumnMapClient game_client{
        initial_window,
        client::canonicalColumnMapPalette(),
        shared::WorldMode::Flight,
        world_configuration,
        true,
    };
    bool start_requested = false;
    bool input_before_playing = false;
    auto record_lead_ready_observation = [&](
        uint64_t const coverage_epoch,
        uint64_t const delivery_token,
        shared::ChunkCoordinate const coordinate,
        std::array<uint32_t, 2> const ready_counts,
        std::optional<shared::PlayerPosition> const& position_before,
        std::optional<shared::PlayerPosition> const& position_after
    ) {
        if (!lead_chunk
            || coverage_epoch != lead_coverage_epoch
            || delivery_token != lead_delivery_token
            || coordinate != *lead_chunk) {
            return;
        }
        shared::SparseWorld const* const world = game_client.authoritativeWorld();
        lead_resident_at_prefetch = world != nullptr && world->residentChunk(*lead_chunk) != nullptr;
        lead_was_required_at_prefetch = true;
        if (position_before && position_after && same_position(*position_before, *position_after)) {
            std::vector<shared::ChunkCoordinate> const required_now = shared::requiredChunkKeys(
                *position_before,
                shared::NEAR_TERRAIN_RADIUS_BLOCKS
            );
            lead_was_required_at_prefetch = std::ranges::binary_search(
                required_now,
                *lead_chunk,
                chunk_less
            );
        }
        lead_ready_chunks_before = ready_counts[0];
        lead_ready_chunks_after = ready_counts[1];
        lead_ready_count_unchanged = lead_ready_chunks_before == lead_ready_chunks_after;
        lead_prefetch_observed.store(true, std::memory_order_release);
    };
    client::GameClientBenchmarkHooks const client_hooks{
        .deadline = std::chrono::steady_clock::now() + CLIENT_DEADLINE,
        .should_stop = [&] {
            client::ColumnMapFrameState const& frame = game_client.frameState();
            return lead_promotion_observed.load(std::memory_order_acquire)
                && lead_packet_occurrences == 1U
                && reroot_succeeded.load(std::memory_order_acquire)
                && reroot_position_observed.load(std::memory_order_acquire)
                && frame.loading.state == client::WorldLoadingState::Playing
                && frame.active_coverage_epoch.has_value()
                && *frame.active_coverage_epoch > wrapped_coverage_epoch.load(std::memory_order_acquire)
                && frame.active_coverage_center.has_value()
                && frame.active_coverage_center->x == reroot_position.x
                && frame.active_coverage_center->y == reroot_position.y
                && frame.active_coverage_center->x_subcell == reroot_position.x_subcell
                && frame.active_coverage_center->y_subcell == reroot_position.y_subcell
                && game_client.presentation().window() == reroot_window
                && game_client.presentation().window()
                    == windowCenteredOnPosition(*frame.active_coverage_center, MAP_SIDE, MAP_SIDE)
                && allMapColumnsReady(game_client)
                && allMapColumnsInActiveCoverage(game_client);
        },
        .is_uncapped_phase = {},
        .input_override = [&](uint64_t) -> std::optional<shared::Direction> {
            if (game_client.worldLoadingStatus().state != client::WorldLoadingState::Playing) {
                input_before_playing = true;
                return shared::Direction{};
            }
            if (crossed_wrap.load(std::memory_order_acquire)
                && !wrapped_frame_captured.load(std::memory_order_acquire)) {
                return shared::Direction{};
            }
            if (lead_promotion_observed.load(std::memory_order_acquire)) {
                return shared::Direction{};
            }
            return shared::Direction{ .x = FULL_AXIS_INPUT, .y = 0U };
        },
        .start_input_override = [&](uint64_t) {
            client::WorldLoadingStatus const status = game_client.worldLoadingStatus();
            if (!start_requested
                && status.state == client::WorldLoadingState::ReadyForStart
                && status.ready_prompt_frame_rendered_at.has_value()) {
                start_requested = true;
                return true;
            }
            return false;
        },
        .on_loop = [&](client::GameClientLoopSample const&) {
            client::WorldLoadingStatus const loading = game_client.worldLoadingStatus();
            if (lead_prefetch_observed.load(std::memory_order_acquire)
                && !lead_promotion_observed.load(std::memory_order_acquire)
                && lead_chunk
                && game_client.activeCoverageContains(*lead_chunk)
                && loading.current_player_position.has_value()) {
                shared::PlayerPosition const ready_center = *loading.current_player_position;
                std::vector<shared::ChunkCoordinate> const required_now = shared::requiredChunkKeys(
                    ready_center,
                    shared::NEAR_TERRAIN_RADIUS_BLOCKS
                );
                shared::SparseWorld const* const world = game_client.authoritativeWorld();
                lead_is_required_after_promotion = std::ranges::binary_search(
                    required_now,
                    *lead_chunk,
                    chunk_less
                );
                lead_is_resident_after_promotion = world != nullptr && world->residentChunk(*lead_chunk) != nullptr;
                lead_epoch_unchanged_on_promotion = game_client.activeCoverageEpoch() == lead_coverage_epoch;
                lead_packet_occurrences_at_promotion = lead_packet_occurrences;
                lead_ready_chunks_after_promotion = loading.playable_sphere_ready_chunks;
                lead_promotion_epoch = game_client.activeCoverageEpoch().value_or(0U);
                lead_promotion_observed.store(true, std::memory_order_release);
            }
            if (!crossed_wrap.load(std::memory_order_acquire)
                || wrapped_frame_captured.load(std::memory_order_acquire)
                || game_client.presentation().window() != initial_window
                || !game_client.frameState().active_coverage_epoch.has_value()
                || !game_client.frameState().active_coverage_center.has_value()
                || !allMapColumnsReady(game_client)) {
                return;
            }
            wrapped_columns_in_coverage = allMapColumnsInActiveCoverage(game_client);
            expectCanonicalMapPixels(game_client, *terrain_world);
            wrapped_pixels_match = true;
            wrapped_coverage_epoch.store(
                *game_client.frameState().active_coverage_epoch,
                std::memory_order_release
            );
            wrapped_coverage_center_x.store(
                game_client.frameState().active_coverage_center->x,
                std::memory_order_release
            );
            wrapped_coverage_center_y.store(
                game_client.frameState().active_coverage_center->y,
                std::memory_order_release
            );
            if (!lead_chunk) {
                client::WorldLoadingStatus const current_loading = game_client.worldLoadingStatus();
                if (current_loading.current_player_position
                    && game_client.frameState().active_coverage_center) {
                    refresh_lead_candidates(
                        *current_loading.current_player_position,
                        *game_client.frameState().active_coverage_center
                    );
                }
            }
            wrapped_frame_captured.store(true, std::memory_order_release);
        },
        .on_authoritative_player = [&](shared::Player const& player) {
            player_id.store(player.id, std::memory_order_release);
            has_player_id.store(true, std::memory_order_release);
            int32_t const previous_x = previous_authoritative_x.exchange(player.x, std::memory_order_acq_rel);
            if (previous_x >= shared::World::FLIGHT_MAX_CELL - 16 && player.x < 16) {
                crossed_wrap.store(true, std::memory_order_release);
                wrapped_position_observed_in_initial_window = game_client.presentation().window() == initial_window;
            }
            if (player.x == reroot_position.x && player.y == reroot_position.y
                && player.x_subcell == reroot_position.x_subcell
                && player.y_subcell == reroot_position.y_subcell) {
                reroot_position_observed.store(true, std::memory_order_release);
            }
        },
        .on_input_sent = [&](
            shared::ClientInputMessage const& input,
            std::chrono::steady_clock::time_point,
            std::chrono::steady_clock::time_point
        ) {
            if (input.direction.x != 0U || input.direction.y != 0U || input.direction.z != 0U) {
                last_movement_input_sequence.store(input.sequence, std::memory_order_release);
            }
        },
        .on_authoritative_state = [&](
            shared::ServerPlayerPositionMessage const& state,
            shared::Player const&,
            std::chrono::steady_clock::time_point
        ) {
            acknowledged_input_sequence.store(state.acknowledged_input_sequence, std::memory_order_release);
        },
        .on_materialized_chunk_batch_received = [&](shared::ServerMaterializedChunkBatchMessage const& batch) {
            client::WorldLoadingStatus const loading = game_client.worldLoadingStatus();
            if (!lead_chunk.has_value()
                && batch.coverage_epoch == game_client.activeCoverageEpoch().value_or(0U)
                && game_client.activeCoverageCenter().has_value()
                && loading.current_player_position.has_value()) {
                shared::PlayerPosition const interest_center = *game_client.activeCoverageCenter();
                shared::PlayerPosition const ready_center = *loading.current_player_position;
                ensure_lead_candidates(ready_center, interest_center);
                ensure_classified_coverage(ready_center, interest_center);
                for (shared::MaterializedChunkPayload const& payload : batch.chunks) {
                    shared::ChunkCoordinate const coordinate = payload.coordinate;
                    if (!std::ranges::binary_search(lead_promotion_candidates, coordinate, chunk_less)) {
                        continue;
                    }
                    if (std::ranges::binary_search(classified_ready_required_keys, coordinate, chunk_less)
                        || !std::ranges::binary_search(classified_interest_keys, coordinate, chunk_less)) {
                        continue;
                    }
                    lead_chunk = coordinate;
                    lead_coverage_epoch = batch.coverage_epoch;
                    lead_delivery_token = batch.delivery_token;
                    lead_packet_occurrences = static_cast<uint32_t>(std::ranges::count_if(
                        batch.chunks,
                        [coordinate](shared::MaterializedChunkPayload const& candidate) {
                            return candidate.coordinate == coordinate;
                        }
                    ));
                    lead_was_required_at_prefetch = false;
                    break;
                }
            } else if (lead_chunk && batch.coverage_epoch == lead_coverage_epoch) {
                lead_packet_occurrences += static_cast<uint32_t>(std::ranges::count_if(
                    batch.chunks,
                    [&](shared::MaterializedChunkPayload const& candidate) {
                        return candidate.coordinate == *lead_chunk;
                    }
                ));
            }
        },
        .on_materialized_chunk_recorded = [&](
            uint64_t const coverage_epoch,
            uint64_t const delivery_token,
            shared::ChunkCoordinate const coordinate,
            client::WorldLoadingStatus const& before,
            client::WorldLoadingStatus const& after
        ) {
            record_lead_ready_observation(
                coverage_epoch,
                delivery_token,
                coordinate,
                std::array{before.playable_sphere_ready_chunks, after.playable_sphere_ready_chunks},
                before.current_player_position,
                after.current_player_position
            );
        },
    };

    game_client.run(core::Address::localhost(server.port()), CHARACTER, &client_hooks);
    stop_server.store(true, std::memory_order_release);
    server_thread.join();

    client::WorldLoadingStatus const final_loading_status = game_client.worldLoadingStatus();
    client::ColumnMapFrameState const& final_frame = game_client.frameState();
    std::vector<client::ColumnCoverage> const& final_column_coverage = game_client.presentation().planes().coverage;
    uint32_t const final_map_ready_columns = static_cast<uint32_t>(std::ranges::count(
        final_column_coverage,
        client::ColumnCoverage::Ready
    ));
    uint32_t const final_map_unknown_columns = static_cast<uint32_t>(final_column_coverage.size())
        - final_map_ready_columns;
    if (!start_requested
        || !crossed_wrap.load(std::memory_order_acquire)
        || !wrapped_frame_captured.load(std::memory_order_acquire)
        || !lead_prefetch_observed.load(std::memory_order_acquire)
        || !lead_promotion_observed.load(std::memory_order_acquire)
        || !reroot_succeeded.load(std::memory_order_acquire)
        || !reroot_position_observed.load(std::memory_order_acquire)) {
        ::testing::Message diagnostic;
        diagnostic << "start=" << start_requested
            << " wrapped=" << crossed_wrap.load(std::memory_order_acquire)
            << " wrapped_in_initial_window=" << wrapped_position_observed_in_initial_window
            << " wrapped_frame=" << wrapped_frame_captured.load(std::memory_order_acquire)
            << " lead_prefetched/promoted=" << lead_prefetch_observed.load(std::memory_order_acquire)
            << "/" << lead_promotion_observed.load(std::memory_order_acquire)
            << " lead_coordinate=" << (lead_chunk ? lead_chunk->x : -1) << ","
            << (lead_chunk ? lead_chunk->y : -1) << "," << (lead_chunk ? lead_chunk->z : -1)
            << " lead_epoch/current_epoch=" << lead_coverage_epoch << "/"
            << game_client.activeCoverageEpoch().value_or(0U)
            << " lead_packets=" << lead_packet_occurrences
            << " lead_ready_before/after=" << lead_ready_chunks_before << "/" << lead_ready_chunks_after
            << " lead_resident/not_required/ready_unchanged=" << lead_resident_at_prefetch << "/"
            << !lead_was_required_at_prefetch << "/" << lead_ready_count_unchanged
            << " lead_promoted_resident/required/same_epoch=" << lead_is_resident_after_promotion << "/"
            << lead_is_required_after_promotion << "/" << lead_epoch_unchanged_on_promotion
            << " lead_packets_at_promotion=" << lead_packet_occurrences_at_promotion
            << " lead_promotion_epoch=" << lead_promotion_epoch
            << " lead_ready_after_promotion=" << lead_ready_chunks_after_promotion
            << " reroot=" << reroot_succeeded.load(std::memory_order_acquire)
            << " reroot_position=" << reroot_position_observed.load(std::memory_order_acquire)
            << " loading=" << worldLoadingStateName(final_loading_status.state)
            << " loading_error=" << final_loading_status.error
            << " atlas/spawn/initial_ready=" << final_loading_status.atlas_ready << "/"
            << final_loading_status.spawn_safe << "/" << final_loading_status.initial_sphere_ready
            << " initial_sphere=" << final_loading_status.initial_sphere_ready_chunks << "/"
            << final_loading_status.initial_sphere_required_chunks
            << " playable_sphere=" << final_loading_status.playable_sphere_ready_chunks << "/"
            << final_loading_status.playable_sphere_required_chunks
            << " ready_prompt_rendered=" << final_loading_status.ready_prompt_frame_rendered_at.has_value()
            << " current_position="
            << (final_loading_status.current_player_position.has_value()
                    ? final_loading_status.current_player_position->x : -1)
            << "," << (final_loading_status.current_player_position.has_value()
                    ? final_loading_status.current_player_position->y : -1)
            << "," << (final_loading_status.current_player_position.has_value()
                    ? final_loading_status.current_player_position->z : -1)
            << " frame_map_ready/unknown=" << final_frame.map_update.ready_columns << "/"
            << final_frame.map_update.unknown_columns
            << " map_ready/unknown=" << final_map_ready_columns << "/" << final_map_unknown_columns
            << " active_epoch=" << final_frame.active_coverage_epoch.value_or(0U)
            << " active_center="
            << (final_frame.active_coverage_center.has_value() ? final_frame.active_coverage_center->x : -1)
            << "," << (final_frame.active_coverage_center.has_value() ? final_frame.active_coverage_center->y : -1)
            << "," << (final_frame.active_coverage_center.has_value() ? final_frame.active_coverage_center->z : -1)
            << " window=" << game_client.presentation().window().origin_x << ","
            << game_client.presentation().window().origin_y
            << " preview_metrics=" << last_preview_metrics.has_value();
        auto appendPreviewMetrics = [&](char const* const label, PreviewMetricsSnapshot const& snapshot) {
            auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                snapshot.sampled_at - first_preview_metrics->sampled_at
            );
            server::GameServer::PreviewStreamMetrics const& metrics = snapshot.metrics;
            diagnostic << " " << label << "_at_ms=" << elapsed.count()
                << " near_ready/resident/desired=" << metrics.near_window_ready_tiles << "/"
                << metrics.near_window_resident_tiles << "/" << metrics.near_window_desired_tiles
                << " ready/queued/dispatched=" << metrics.ready_tiles << "/"
                << metrics.queued_tiles << "/" << metrics.dispatched_tiles
                << " workers_pending/submitted=" << metrics.pending_worker_jobs << "/"
                << metrics.submitted_worker_jobs
                << " height_pending/queued/running/completed="
                << metrics.height_tile_jobs.pending_jobs << "/"
                << metrics.height_tile_jobs.executor_queued_jobs << "/"
                << metrics.height_tile_jobs.running_jobs << "/"
                << metrics.height_tile_jobs.completed_uncollected_jobs
                << " worldgen_pending/queued/running/completed="
                << metrics.world_generation_jobs.pending_jobs << "/"
                << metrics.world_generation_jobs.executor_queued_jobs << "/"
                << metrics.world_generation_jobs.running_jobs << "/"
                << metrics.world_generation_jobs.completed_uncollected_jobs
                << " preview_inflight_deliveries=" << metrics.inflight_deliveries
                << " untracked=" << metrics.near_window_untracked_tiles
                << " inputs_pending=" << metrics.pending_input_count;
        };
        if (first_preview_metrics.has_value() && last_preview_metrics.has_value()) {
            appendPreviewMetrics("metrics_first", *first_preview_metrics);
            appendPreviewMetrics("metrics_last", *last_preview_metrics);
        }
        ADD_FAILURE() << diagnostic.GetString();
        return;
    }

    EXPECT_TRUE(start_requested);
    EXPECT_FALSE(input_before_playing);
    EXPECT_TRUE(crossed_wrap.load(std::memory_order_acquire));
    EXPECT_TRUE(wrapped_position_observed_in_initial_window);
    EXPECT_TRUE(wrapped_frame_captured.load(std::memory_order_acquire));
    EXPECT_TRUE(lead_prefetch_observed.load(std::memory_order_acquire));
    EXPECT_TRUE(lead_resident_at_prefetch);
    EXPECT_FALSE(lead_was_required_at_prefetch);
    EXPECT_TRUE(lead_ready_count_unchanged)
        << "Dense prefetch must not add authoritative full-volume profile coverage.";
    EXPECT_EQ(lead_packet_occurrences, 1U);
    EXPECT_TRUE(lead_promotion_observed.load(std::memory_order_acquire));
    EXPECT_TRUE(lead_is_resident_after_promotion);
    EXPECT_TRUE(lead_is_required_after_promotion);
    EXPECT_TRUE(lead_epoch_unchanged_on_promotion);
    EXPECT_EQ(lead_promotion_epoch, lead_coverage_epoch);
    EXPECT_EQ(lead_packet_occurrences_at_promotion, 1U);
    EXPECT_GT(wrapped_coverage_epoch.load(std::memory_order_acquire), 0U);
    EXPECT_TRUE(wrapped_columns_in_coverage);
    EXPECT_TRUE(wrapped_pixels_match);
    EXPECT_TRUE(reroot_succeeded.load(std::memory_order_acquire));
    EXPECT_TRUE(reroot_position_observed.load(std::memory_order_acquire));
    ASSERT_TRUE(game_client.frameState().active_coverage_epoch.has_value());
    EXPECT_GT(
        *game_client.frameState().active_coverage_epoch,
        wrapped_coverage_epoch.load(std::memory_order_acquire)
    );
    ASSERT_TRUE(game_client.frameState().active_coverage_center.has_value());
    EXPECT_NE(wrapped_coverage_center_x.load(std::memory_order_acquire), reroot_position.x);
    EXPECT_EQ(wrapped_coverage_center_y.load(std::memory_order_acquire), reroot_position.y);
    EXPECT_EQ(game_client.frameState().active_coverage_center->x_subcell, reroot_position.x_subcell);
    EXPECT_EQ(game_client.frameState().active_coverage_center->y_subcell, reroot_position.y_subcell);
    EXPECT_EQ(game_client.presentation().window(), reroot_window);
    EXPECT_EQ(
        game_client.presentation().window(),
        windowCenteredOnPosition(*game_client.frameState().active_coverage_center, MAP_SIDE, MAP_SIDE)
    );
    EXPECT_TRUE(allMapColumnsReady(game_client));
    EXPECT_TRUE(allMapColumnsInActiveCoverage(game_client));
    expectCanonicalMapPixels(game_client, *terrain_world);
}
