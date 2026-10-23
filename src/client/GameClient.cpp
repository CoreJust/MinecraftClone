#include <client/GameClient.hpp>

#include "TerrainLodSource.hpp"

#include <client/ClientAudio.hpp>
#include <client/FrameScheduler.hpp>
#include <client/PlayerPresentation.hpp>
#include <client/PreviewResidency.hpp>
#include <client/WorldLoading.hpp>
#include <shared/net/Message.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainLod.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldCoverage.hpp>
#include <generation/Generation.hpp>
#include <core/common/ControlFlow.hpp>
#include <core/common/SpanUtils.hpp>
#include <core/IO/Log.hpp>
#include <core/net/Address.hpp>
#include <core/net/Event.hpp>
#include <core/net/Peer.hpp>
#include <core/net/SendMode.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <ratio>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace client {

namespace {

constexpr std::chrono::seconds CONNECTION_ATTEMPT_TIMEOUT{ 30 };
constexpr std::chrono::milliseconds CONNECTION_RETRY_DELAY{ 250 };
constexpr std::chrono::milliseconds PLATFORM_SERVICE_INTERVAL{ 25 };
constexpr std::chrono::milliseconds NETWORK_POLL_BUDGET{ 1 };
constexpr uint8_t INITIAL_MATERIALIZED_CHUNK_CREDITS = shared::MATERIALIZED_CHUNK_DELIVERY_WINDOW;
constexpr uint32_t MAX_PENDING_MATERIALIZED_BATCHES = shared::MATERIALIZED_CHUNK_DELIVERY_WINDOW;
constexpr uint32_t MAX_PENDING_MATERIALIZED_CHUNKS =
    MAX_PENDING_MATERIALIZED_BATCHES * shared::MATERIALIZED_CHUNK_BATCH_CAPACITY;
constexpr int32_t CONNECTIVITY_GRID_RADIUS_CHUNKS = static_cast<int32_t>(
    shared::MAX_MATERIALIZED_INTEREST_RADIUS_BLOCKS / shared::Chunk::SIDE_LENGTH
) + 1;
constexpr uint32_t CONNECTIVITY_GRID_WIDTH = static_cast<uint32_t>(
    CONNECTIVITY_GRID_RADIUS_CHUNKS * 2 + 1
);
constexpr uint32_t CONNECTIVITY_GRID_SLOT_COUNT =
    CONNECTIVITY_GRID_WIDTH * CONNECTIVITY_GRID_WIDTH * CONNECTIVITY_GRID_WIDTH;
constexpr uint32_t NO_CONNECTIVITY_CANDIDATE = std::numeric_limits<uint32_t>::max();
static_assert(CONNECTIVITY_GRID_WIDTH == 13U);
static_assert(CONNECTIVITY_GRID_SLOT_COUNT < NO_CONNECTIVITY_CANDIDATE);

[[nodiscard]]
bool isValidPlayerPosition(shared::PlayerPosition const position) noexcept
{
    return position.x_subcell < shared::SUBCELLS_PER_CELL
        && position.y_subcell < shared::SUBCELLS_PER_CELL
        && position.z_subcell < shared::SUBCELLS_PER_CELL
        && shared::World::isFlightPositionInBounds(position);
}

[[nodiscard]]
bool samePlayerPosition(shared::PlayerPosition const left, shared::PlayerPosition const right) noexcept
{
    return left.x == right.x
        && left.y == right.y
        && left.z == right.z
        && left.x_subcell == right.x_subcell
        && left.y_subcell == right.y_subcell
        && left.z_subcell == right.z_subcell;
}

[[nodiscard]]
shared::ChunkCoordinate chunkForPlayerPosition(shared::PlayerPosition const position) noexcept
{
    return {
        .x = static_cast<int32_t>(shared::WorldBounds::wrapHorizontal(position.x) / shared::Chunk::SIDE_LENGTH),
        .y = static_cast<int32_t>(shared::WorldBounds::wrapHorizontal(position.y) / shared::Chunk::SIDE_LENGTH),
        .z = position.z / shared::Chunk::SIDE_LENGTH,
    };
}

[[nodiscard]]
bool areFaceAdjacent(shared::ChunkCoordinate const left, shared::ChunkCoordinate const right) noexcept
{
    constexpr int32_t HORIZONTAL_CHUNK_COUNT =
        static_cast<int32_t>(shared::WorldExtent::WIDTH / shared::Chunk::SIDE_LENGTH);
    int32_t dx = std::abs(left.x - right.x);
    int32_t dy = std::abs(left.y - right.y);
    dx = (std::min)(dx, HORIZONTAL_CHUNK_COUNT - dx);
    dy = (std::min)(dy, HORIZONTAL_CHUNK_COUNT - dy);
    int32_t const dz = std::abs(left.z - right.z);
    return dx + dy + dz == 1;
}

[[nodiscard]]
bool isChunkCoordinateZyxLess(
    shared::ChunkCoordinate const left,
    shared::ChunkCoordinate const right
) noexcept
{
    if (left.z != right.z) {
        return left.z < right.z;
    }
    if (left.y != right.y) {
        return left.y < right.y;
    }
    return left.x < right.x;
}

[[nodiscard]]
std::array<shared::ChunkCoordinate, 6U> faceNeighbors(
    shared::ChunkCoordinate const coordinate
) noexcept
{
    constexpr int32_t HORIZONTAL_CHUNK_COUNT =
        static_cast<int32_t>(shared::WorldExtent::WIDTH / shared::Chunk::SIDE_LENGTH);
    return {
        shared::ChunkCoordinate{ .x = (coordinate.x + HORIZONTAL_CHUNK_COUNT - 1) % HORIZONTAL_CHUNK_COUNT,
            .y = coordinate.y, .z = coordinate.z },
        shared::ChunkCoordinate{ .x = (coordinate.x + 1) % HORIZONTAL_CHUNK_COUNT,
            .y = coordinate.y, .z = coordinate.z },
        shared::ChunkCoordinate{ .x = coordinate.x,
            .y = (coordinate.y + HORIZONTAL_CHUNK_COUNT - 1) % HORIZONTAL_CHUNK_COUNT, .z = coordinate.z },
        shared::ChunkCoordinate{ .x = coordinate.x,
            .y = (coordinate.y + 1) % HORIZONTAL_CHUNK_COUNT, .z = coordinate.z },
        shared::ChunkCoordinate{ .x = coordinate.x, .y = coordinate.y, .z = coordinate.z - 1 },
        shared::ChunkCoordinate{ .x = coordinate.x, .y = coordinate.y, .z = coordinate.z + 1 },
    };
}

[[nodiscard]]
std::optional<uint32_t> connectivityGridSlot(
    shared::ChunkCoordinate const coordinate,
    shared::ChunkCoordinate const center
) noexcept
{
    if (!shared::WorldBounds::isValidChunk(coordinate)) {
        return std::nullopt;
    }

    constexpr int32_t HORIZONTAL_CHUNK_COUNT = static_cast<int32_t>(
        shared::WorldExtent::WIDTH / shared::Chunk::SIDE_LENGTH
    );
    int32_t offset_x = coordinate.x - center.x;
    int32_t offset_y = coordinate.y - center.y;
    if (offset_x > HORIZONTAL_CHUNK_COUNT / 2) {
        offset_x -= HORIZONTAL_CHUNK_COUNT;
    } else if (offset_x < -HORIZONTAL_CHUNK_COUNT / 2) {
        offset_x += HORIZONTAL_CHUNK_COUNT;
    }
    if (offset_y > HORIZONTAL_CHUNK_COUNT / 2) {
        offset_y -= HORIZONTAL_CHUNK_COUNT;
    } else if (offset_y < -HORIZONTAL_CHUNK_COUNT / 2) {
        offset_y += HORIZONTAL_CHUNK_COUNT;
    }
    int32_t const offset_z = coordinate.z - center.z;
    if (offset_x < -CONNECTIVITY_GRID_RADIUS_CHUNKS
        || offset_x > CONNECTIVITY_GRID_RADIUS_CHUNKS
        || offset_y < -CONNECTIVITY_GRID_RADIUS_CHUNKS
        || offset_y > CONNECTIVITY_GRID_RADIUS_CHUNKS
        || offset_z < -CONNECTIVITY_GRID_RADIUS_CHUNKS
        || offset_z > CONNECTIVITY_GRID_RADIUS_CHUNKS) {
        return std::nullopt;
    }

    uint32_t const local_x = static_cast<uint32_t>(offset_x + CONNECTIVITY_GRID_RADIUS_CHUNKS);
    uint32_t const local_y = static_cast<uint32_t>(offset_y + CONNECTIVITY_GRID_RADIUS_CHUNKS);
    uint32_t const local_z = static_cast<uint32_t>(offset_z + CONNECTIVITY_GRID_RADIUS_CHUNKS);
    return (local_z * CONNECTIVITY_GRID_WIDTH + local_y) * CONNECTIVITY_GRID_WIDTH + local_x;
}

[[nodiscard]]
bool nearCoverageRemainsConnected(
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    shared::PlayerPosition const old_center,
    shared::PlayerPosition const new_center
)
{
    std::vector<shared::ChunkCoordinate> const old_near = shared::requiredChunkKeys(
        old_center,
        client::WorldLoading::INITIAL_READY_SPHERE_RADIUS_BLOCKS
    );
    std::vector<shared::ChunkCoordinate> const new_near = shared::requiredChunkKeys(
        new_center,
        client::WorldLoading::INITIAL_READY_SPHERE_RADIUS_BLOCKS
    );
    if (old_near.empty() && new_near.empty()) {
        return true;
    }
    return std::ranges::any_of(new_near, [&old_near](shared::ChunkCoordinate const new_key) {
        return std::ranges::any_of(old_near, [new_key](shared::ChunkCoordinate const old_key) {
            return old_key == new_key || areFaceAdjacent(old_key, new_key);
        });
    });
}

[[nodiscard]]
bool isValidTerrainSession(shared::TerrainSessionIdentity const& session) noexcept
{
    if (session.join_epoch == 0U
        || !shared::isValidTerrainIdentity(session.terrain)
        || session.terrain.seed != session.configuration.seed
        || (session.configuration.resolution != 256U && session.configuration.resolution != 512U)) {
        return false;
    }
    try {
        return session.terrain.configuration_identity
            == generation::configurationIdentity(session.configuration);
    } catch (...) {
        return false;
    }
}

[[nodiscard]]
bool isHeightTileValidForWorld(
    shared::ServerHeightTileMessage const& message,
    uint64_t const world_revision,
    std::optional<shared::TerrainIdentity> const& world_identity
) noexcept
{
    if (message.key != shared::normalizeHeightTileKey(message.key)) {
        return false;
    }
    if (message.revision == 0U || message.token == 0U || world_revision == 0U
        || message.revision != world_revision) {
        return false;
    }
    if (!world_identity) {
        return false;
    }
    if (message.surface.coordinate != shared::ChunkCoordinate{
            .x = message.key.x,
            .y = message.key.y,
            .z = 0,
        }) {
        return false;
    }
    if (message.surface.identity != *world_identity) {
        return false;
    }
    if (message.surface.identity.revision != message.revision) {
        return false;
    }
    if (!shared::isValidSurfaceTile(message.surface)) {
        return false;
    }
    for (uint32_t index = 0U; index < message.heights.size(); ++index) {
        uint32_t const expected_height = static_cast<uint32_t>(
            message.surface.columns[index].highest_solid_z
        ) + 1U;
        if (message.heights[index] != expected_height || message.heights[index] > generation::MAX_HEIGHT + 1U) {
            return false;
        }
    }
    return true;
}

} // namespace

GameClient::GameClient(
    shared::WorldMode const mode,
    shared::WorldConfiguration const configuration,
    bool const wants_previews,
    std::unique_ptr<ClientAudioOutput> audio_output
)
    : core::Client{ 2 }
    , m_world{ mode, configuration }
    , m_predicted_world{ mode, configuration }
    , m_height_tile_residency{ { .generation = 1, .revision = 1 }, {} }
    , m_wants_previews{ wants_previews }
    , m_client_audio{ std::move(audio_output) }
#if defined(_WIN32)
    , m_world_loading{ WorldLoadingPlatform::Windows }
#else
    , m_world_loading{ WorldLoadingPlatform::Mac }
#endif
{
}

GameClient::~GameClient() = default;

WorldLoadingStatus GameClient::worldLoadingStatus() const
{
    return m_world_loading.status(std::chrono::steady_clock::now());
}

shared::SparseWorld const* GameClient::authoritativeWorld() const noexcept
{
    return m_authoritative_world.get();
}

std::optional<shared::TerrainIdentity> GameClient::activeTerrainIdentity() const
{
    if (!m_terrain_session) {
        return std::nullopt;
    }
    return m_terrain_session->terrain;
}

std::shared_ptr<shared::TerrainWorld const> GameClient::terrainLodWorld() const noexcept
{
    if (!m_terrain_session || !m_terrain_lod_source) {
        return nullptr;
    }
    return m_terrain_lod_source->worldFor(*m_terrain_session);
}

std::shared_ptr<shared::TerrainLodArtifact const> GameClient::terrainLodRoot() const noexcept
{
    if (!m_terrain_session || !m_terrain_lod_source) {
        return nullptr;
    }
    return m_terrain_lod_source->rootFor(*m_terrain_session);
}

std::optional<uint64_t> GameClient::activeCoverageEpoch() const noexcept
{
    return m_active_coverage_epoch;
}

std::optional<shared::PlayerPosition> GameClient::activeCoverageCenter() const noexcept
{
    return m_active_coverage_center;
}

bool GameClient::activeCoverageContains(shared::ChunkCoordinate const coordinate) const noexcept
{
    return m_active_coverage_chunks.contains(coordinate);
}

uint64_t GameClient::ChunkCoordinateHash::operator()(
    shared::ChunkCoordinate const coordinate
) const noexcept
{
    uint64_t hash = static_cast<uint32_t>(coordinate.x);
    hash ^= static_cast<uint64_t>(static_cast<uint32_t>(coordinate.y))
        + 0x9e37'79b9'7f4a'7c15ULL + (hash << 6U) + (hash >> 2U);
    hash ^= static_cast<uint64_t>(static_cast<uint32_t>(coordinate.z))
        + 0x9e37'79b9'7f4a'7c15ULL + (hash << 6U) + (hash >> 2U);
    return hash;
}

bool GameClient::beginLoadingRequest(LoadingTimePoint const accepted_at)
{
    generation::GenerationConfiguration configuration;
    configuration.seed = m_world.configuration().seed;
    WorldLoadingIdentity identity{
        .join_epoch = m_next_join_epoch,
        .configuration = configuration,
    };
    if (!m_world_loading.begin(identity, accepted_at)) {
        return false;
    }
    m_world_loading_identity = identity;
    if (m_terrain_lod_source) {
        m_terrain_lod_source->cancel();
    }
    ++m_next_join_epoch;
    if (m_next_join_epoch == 0U) {
        m_next_join_epoch = 1U;
    }
    m_terrain_session.reset();
    m_world.setCollisionWorld(nullptr);
    m_predicted_world.setCollisionWorld(nullptr);
    m_authoritative_world.reset();
    m_terrain_identity.reset();
    m_resident_height_tile_keys.clear();
    clearActiveCoverage();
    m_last_world_preparation_sequence = 0U;
    m_initial_chunk_credit_sent = false;
    m_start_message_sent = false;
    return true;
}

void GameClient::run(
    core::Address const server_address,
    char const ch,
    GameClientBenchmarkHooks const* const benchmark_hooks,
    std::optional<std::chrono::steady_clock::time_point> const request_accepted_at
) {
    LoadingTimePoint const request_started_at = request_accepted_at.value_or(
        std::chrono::steady_clock::now()
    );
    if (!beginLoadingRequest(request_started_at)) {
        CORE_ERROR("Could not initialize the loading state for this client request");
        return;
    }
    m_benchmark_hooks = benchmark_hooks;
    m_benchmark_bytes_sent = 0U;
    m_benchmark_bytes_received = 0U;
    m_local_character = ch;
    m_running = true;
    m_accepted = false;
    m_join_character_index = 0;
    if (m_world.mode() == shared::WorldMode::Flight) {
        auto const character = std::ranges::find(FLIGHT_CHARACTERS, ch);
        if (character != FLIGHT_CHARACTERS.end()) {
            m_join_character_index = static_cast<uint32_t>(character - FLIGHT_CHARACTERS.begin());
        }
    }
    FrameScheduler scheduler{ std::chrono::steady_clock::now(), shared::TICK };
    auto next_loading_render_at = request_started_at;
    auto const render_loading_if_due = [this, &next_loading_render_at] {
        auto const now = std::chrono::steady_clock::now();
        if (now >= next_loading_render_at) {
            render();
            next_loading_render_at = now + PLATFORM_SERVICE_INTERVAL;
        }
    };
    auto const benchmark_stopped = [benchmark_hooks] {
        return benchmark_hooks && (
            (benchmark_hooks->deadline && std::chrono::steady_clock::now() >= *benchmark_hooks->deadline)
            || (benchmark_hooks->should_stop && benchmark_hooks->should_stop())
        );
    };
    auto const service_connection = [&] {
        servicePlatformEvents();
        render_loading_if_due();
        return m_running && !benchmark_stopped()
            ? core::ControlFlow::Continue : core::ControlFlow::Break;
    };
    uint64_t input_ordinal = 0U;
    while (m_running && !benchmark_stopped()) {
        servicePlatformEvents();
        if (!m_running || benchmark_stopped()) {
            break;
        }
        if (!isConnected()) {
            if (!connect(server_address, CONNECTION_ATTEMPT_TIMEOUT, service_connection)) {
                CORE_INFO("Waiting to connect to server {}", server_address);
                auto const retry_deadline = std::chrono::steady_clock::now() + CONNECTION_RETRY_DELAY;
                while (service_connection() == core::ControlFlow::Continue
                    && std::chrono::steady_clock::now() < retry_deadline) {
                    std::this_thread::sleep_for((std::min)(
                        PLATFORM_SERVICE_INTERVAL,
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            retry_deadline - std::chrono::steady_clock::now()
                        )
                    ));
                }
                continue;
            }
            m_accepted = false;
            if (!sendJoinRequest()) {
                continue;
            }
        }
        while (!m_accepted && m_running && isConnected() && !benchmark_stopped()) {
            servicePlatformEvents();
            render_loading_if_due();
            if (!m_running || benchmark_stopped()) {
                break;
            }
            poll(PLATFORM_SERVICE_INTERVAL);
        }
        if (!m_running || benchmark_stopped()) {
            break;
        }
        if (!isConnected()) {
            continue;
        }
        auto const iteration_started_at = std::chrono::steady_clock::now();
        auto const loop_started_at = benchmark_hooks
            ? iteration_started_at
            : std::chrono::steady_clock::time_point{};
        auto const network_poll_started_at = loop_started_at;
        uint64_t network_event_count = 0U;
        uint32_t polled_event_count = 0U;
        auto const network_poll_deadline = iteration_started_at + NETWORK_POLL_BUDGET;
        while (m_running && !benchmark_stopped()
            && (polled_event_count = poll(std::chrono::milliseconds::zero())) > 0U) {
            network_event_count += polled_event_count;
            if (std::chrono::steady_clock::now() >= network_poll_deadline) {
                break;
            }
        }
        auto const network_poll_completed_at = benchmark_hooks
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        processPendingHeightTileDeliveries();
        processPendingMaterializedChunks(std::chrono::steady_clock::now());
        auto const height_tile_delivery_completed_at = benchmark_hooks
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        if (benchmark_stopped()) {
            break;
        }
        if (!m_running || !isConnected()) {
            continue;
        }
        std::chrono::steady_clock::time_point const now = std::chrono::steady_clock::now();
        bool input_sent = false;
        if (scheduler.simulationDue(now)) {
            std::optional<shared::Direction> direction;
            bool const benchmark_start_requested = benchmark_hooks
                && benchmark_hooks->start_input_override
                && benchmark_hooks->start_input_override(input_ordinal);
            if (benchmark_start_requested) {
                static_cast<void>(acceptLoadingStartInput(std::chrono::steady_clock::now()));
            } else if (worldLoadingStatus().state == WorldLoadingState::Playing) {
                if (benchmark_hooks && benchmark_hooks->input_override) {
                    direction = benchmark_hooks->input_override(input_ordinal);
                }
                if (!direction) {
                    direction = input();
                }
            }
            ++input_ordinal;
            if (worldLoadingStatus().state == WorldLoadingState::Playing && direction) {
                if (auto const predicted_input = predictInput(*direction, now)) {
                    input_sent = send(*predicted_input);
                    if (input_sent) {
                        onInputSubmitted(predicted_input->direction);
                    }
                    if (!input_sent) {
                        discardPredictedInput(predicted_input->sequence, now);
                    } else if (benchmark_hooks && benchmark_hooks->on_input_sent) {
                        benchmark_hooks->on_input_sent(
                            *predicted_input,
                            std::chrono::steady_clock::now(),
                            scheduler.lastSimulationBoundary()
                        );
                    }
                }
            }
        }
        flush();
        auto const render_started_at = benchmark_hooks
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        render();
        auto const render_completed_at = benchmark_hooks
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        if (!m_running || !isConnected()) {
            continue;
        }
        bool const presentation_succeeded = presentationSucceeded();
        if (benchmark_hooks && benchmark_hooks->on_loop) {
            benchmark_hooks->on_loop(GameClientLoopSample{
                .started_at = loop_started_at,
                .completed_at = render_completed_at,
                .network_poll_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    network_poll_completed_at - network_poll_started_at
                ),
                .height_tile_delivery_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    height_tile_delivery_completed_at - network_poll_completed_at
                ),
                .render_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    render_completed_at - render_started_at
                ),
                .network_event_count = network_event_count,
                .client_message_payload_bytes_sent = m_benchmark_bytes_sent,
                .client_message_payload_bytes_received = m_benchmark_bytes_received,
                .input_sent = input_sent,
                .presentation_succeeded = presentation_succeeded,
            });
        }
        if (!benchmark_hooks || !benchmark_hooks->is_uncapped_phase || !benchmark_hooks->is_uncapped_phase()) {
            auto const iteration_completed_at = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(scheduler.idleDelay(
                iteration_started_at,
                iteration_completed_at,
                presentation_succeeded
            ));
        }
    }
    m_client_audio.stop();
    m_benchmark_hooks = nullptr;
}

void GameClient::onDisconnected(core::DisconnectEvent const event) {
    CORE_INFO("Server disconnected: {}", event.peer.address());
    std::cout << "[SERVER DISCONNECTED] address " << fmt::format("{}", event.peer.address()) << '\n';
    m_accepted = false;
    resetConnectionState();
}

void GameClient::resetConnectionState()
{
    if (m_terrain_lod_source) {
        m_terrain_lod_source->cancel();
    }
    m_world = shared::World{ m_world.mode(), m_world.configuration() };
    m_predicted_world = shared::World{ m_world.mode(), m_world.configuration() };
    m_player_presentation = {};
    m_next_id = 0U;
    m_state_revisions.clear();
    m_pending_inputs.clear();
    m_last_predicted_direction.reset();
    m_forecast_player_position.reset();
    m_last_received_input_ack = 0U;
    m_last_received_position_revision = 0U;
    m_last_received_position_at = {};
    m_last_received_position_accepted = false;
    m_rejected_local_position_count = 0U;
    m_last_accepted_input_ack = 0U;
    m_last_accepted_position_revision = 0U;
    m_last_accepted_position_at = {};
    m_next_input_sequence = 1U;
    m_height_tile_residency = PreviewResidency{ { .generation = 0U, .revision = 0U } };
    m_height_tile_revision = { .generation = 0U, .revision = 0U };
    m_render_distance.reset();
    m_height_tile_credit_revision = 0U;
    m_pending_height_tile_deliveries.clear();
    m_pending_height_tile_delivery_tokens.clear();
    m_terrain_identity.reset();
    m_resident_height_tile_keys.clear();
    m_authoritative_world.reset();
    m_terrain_session.reset();
    clearActiveCoverage();
    m_last_world_preparation_sequence = 0U;
    m_initial_chunk_credit_sent = false;
    m_start_message_sent = false;
    if (m_world_loading_identity.join_epoch != 0U) {
        m_world_loading_identity.join_epoch = m_next_join_epoch;
        ++m_next_join_epoch;
        if (m_next_join_epoch == 0U) {
            m_next_join_epoch = 1U;
        }
        static_cast<void>(m_world_loading.restart(
            m_world_loading_identity,
            std::chrono::steady_clock::now()
        ));
    }
    onConnectionStateReset();
}

void GameClient::onReceived(core::ReceiveEvent event) {
    if (m_benchmark_hooks) {
        m_benchmark_bytes_received += event.data.size();
    }
    auto const diagnostic_receive_at = std::chrono::steady_clock::now();
    std::optional maybe_msg = shared::decodeMessage(event.data);
    if (!maybe_msg) {
        CORE_ERROR("Received a corrupted message");
        return;
    }

    shared::Message* msg_ptr = &*maybe_msg;
    if (event.channel_id == shared::HEIGHT_TILE_CHANNEL) {
        if (auto* authority = std::get_if<shared::ServerAuthoritativeTerrainBatchMessage>(msg_ptr)) {
            static_cast<void>(applyAuthoritativeTerrainBatch(*authority, std::chrono::steady_clock::now()));
        } else if (auto* chunk_batch = std::get_if<shared::ServerMaterializedChunkBatchMessage>(msg_ptr)) {
            if (m_benchmark_hooks && m_benchmark_hooks->on_materialized_chunk_batch_received) {
                m_benchmark_hooks->on_materialized_chunk_batch_received(*chunk_batch);
            }
            static_cast<void>(applyMaterializedChunkBatch(*chunk_batch, std::chrono::steady_clock::now()));
        } else if (auto* tile_descriptor = std::get_if<shared::ServerHeightTileDescriptorMessage>(msg_ptr)) {
            static_cast<void>(applyHeightTileDescriptor(*tile_descriptor));
        } else if (!m_render_distance) {
            CORE_ERROR("Received terrain before a valid server render-distance descriptor");
            return;
        } else if (auto* world_revision = std::get_if<shared::ServerWorldRevisionMessage>(msg_ptr)) {
            static_cast<void>(applyWorldRevision(*world_revision));
        } else if (auto* height_tile = std::get_if<shared::ServerHeightTileMessage>(msg_ptr)) {
            static_cast<void>(applyHeightTile(*height_tile));
        } else if (auto* height_tile_batch = std::get_if<shared::ServerHeightTileBatchMessage>(msg_ptr)) {
            static_cast<void>(queueHeightTileDelivery(std::move(*height_tile_batch)));
        } else if (auto* height_tile_removal = std::get_if<shared::ServerRemoveHeightTileMessage>(msg_ptr)) {
            static_cast<void>(applyHeightTileRemoval(*height_tile_removal));
        }
    } else if (event.channel_id != shared::GAME_CHANNEL) {
        CORE_ERROR("Received a game message on an unsupported channel {}", event.channel_id);
    } else if (auto* join_response = std::get_if<shared::JoinResponseMessage>(msg_ptr)) {
        if (join_response->join_epoch != m_world_loading_identity.join_epoch) {
            return;
        }
        bool const accepted = join_response->accepted;
        if (accepted) {
            bool const first_accept = !m_accepted;
            m_accepted = true;
            if (first_accept) {
                auto const audio_started_at = std::chrono::steady_clock::now();
                bool const audio_started = m_client_audio.start();
                auto const audio_completed_at = std::chrono::steady_clock::now();
                CORE_INFO("startup-boundary join-audio steady_ns={} start_duration_us={} started={}",
                    std::chrono::duration_cast<std::chrono::nanoseconds>(audio_started_at.time_since_epoch()).count(),
                    std::chrono::duration_cast<std::chrono::microseconds>(audio_completed_at - audio_started_at).count(),
                    audio_started);
                if (audio_started) {
                    static_cast<void>(m_client_audio.playJoinCue());
                } else {
                    CORE_ERROR("Client audio output could not start; continuing without audio");
                }
            }
        } else if (m_accepted) {
            m_running = false;
        } else {
            if (m_world.mode() != shared::WorldMode::Flight
                || m_join_character_index + 1U >= FLIGHT_CHARACTERS.size()) {
                static_cast<void>(m_world_loading.fail(
                    m_world_loading_identity,
                    "The server rejected the join request.",
                    std::chrono::steady_clock::now()
                ));
                m_running = false;
            } else {
                ++m_join_character_index;
                m_local_character = FLIGHT_CHARACTERS[m_join_character_index];
                if (!sendJoinRequest()) {
                    m_running = false;
                }
            }
        }
    } else if (auto* player_position = std::get_if<shared::ServerPlayerPositionMessage>(msg_ptr)) {
        if (applyServerPosition(*player_position) && m_benchmark_hooks) {
            if (auto const player = m_world.playerByCharacter(player_position->ch)) {
                if (m_benchmark_hooks->on_authoritative_player) {
                    m_benchmark_hooks->on_authoritative_player(*player);
                }
                if (m_benchmark_hooks->on_authoritative_state) {
                    m_benchmark_hooks->on_authoritative_state(
                        *player_position,
                        *player,
                        std::chrono::steady_clock::now()
                    );
                }
            }
        }
    } else if (auto* player_removal = std::get_if<shared::ServerRemovePlayerMessage>(msg_ptr)) {
        applyServerRemoval(player_removal->ch);
    } else if (auto* world_preparation = std::get_if<shared::ServerWorldPreparationMessage>(msg_ptr)) {
        auto const apply_started_at = std::chrono::steady_clock::now();
        bool const applied = applyWorldPreparation(*world_preparation, apply_started_at);
        auto const apply_completed_at = std::chrono::steady_clock::now();
        if (world_preparation->sequence <= 32U || world_preparation->final_atlas_complete) {
            auto const* snapshot = world_preparation->latest_snapshot.get();
            size_t const snapshot_payload_bytes = snapshot == nullptr ? 0U
                : (snapshot->elevation.size() + snapshot->temperature.size()
                    + snapshot->precipitation.size() + snapshot->water_level.size()) * sizeof(float)
                    + snapshot->ocean_mask.size() + snapshot->lake_mask.size() + snapshot->river_mask.size();
            CORE_INFO("startup-boundary preparation-client sequence={} receive_steady_ns={} packet_bytes={} "
                "snapshot_payload_bytes={} decode_us={} apply_us={} applied={} final={}",
                world_preparation->sequence,
                std::chrono::duration_cast<std::chrono::nanoseconds>(diagnostic_receive_at.time_since_epoch()).count(),
                event.data.size(), snapshot_payload_bytes,
                std::chrono::duration_cast<std::chrono::microseconds>(apply_started_at - diagnostic_receive_at).count(),
                std::chrono::duration_cast<std::chrono::microseconds>(apply_completed_at - apply_started_at).count(),
                applied, world_preparation->final_atlas_complete);
        }
    } else if (auto* chunk_coverage = std::get_if<shared::ServerChunkCoverageMessage>(msg_ptr)) {
        static_cast<void>(applyChunkCoverage(*chunk_coverage, std::chrono::steady_clock::now()));
    } else if (auto* authority = std::get_if<shared::ServerAuthoritativeTerrainBatchMessage>(msg_ptr)) {
        static_cast<void>(applyAuthoritativeTerrainBatch(*authority, std::chrono::steady_clock::now()));
    } else if (auto* chunk_batch = std::get_if<shared::ServerMaterializedChunkBatchMessage>(msg_ptr)) {
        if (m_benchmark_hooks && m_benchmark_hooks->on_materialized_chunk_batch_received) {
            m_benchmark_hooks->on_materialized_chunk_batch_received(*chunk_batch);
        }
        static_cast<void>(applyMaterializedChunkBatch(*chunk_batch, std::chrono::steady_clock::now()));
    } else if (auto* start_ack = std::get_if<shared::ServerStartWorldAckMessage>(msg_ptr)) {
        static_cast<void>(applyStartWorldAck(*start_ack, std::chrono::steady_clock::now()));
    } else {
        CORE_ERROR("Received a message unsupported by the client {}", msg_ptr->index());
    }
}

bool GameClient::validateSession(shared::TerrainSessionIdentity const& session) const noexcept
{
    return m_accepted
        && isValidTerrainSession(session)
        && session.join_epoch == m_world_loading_identity.join_epoch
        && session.configuration.seed == m_world.configuration().seed
        && (!m_terrain_session || *m_terrain_session == session);
}

bool GameClient::applyWorldPreparation(
    shared::ServerWorldPreparationMessage const& message,
    LoadingTimePoint const received_at
)
{
    if (!validateSession(message.session)
        || message.sequence == 0U
        || message.sequence <= m_last_world_preparation_sequence
        || message.work_completed > message.work_total
        || static_cast<uint8_t>(message.stage) > static_cast<uint8_t>(generation::GenerationStage::Complete)) {
        return false;
    }

    if (!m_terrain_session) {
        if (m_terrain_identity && *m_terrain_identity != message.session.terrain) {
            return false;
        }
        if (m_world_loading_identity.configuration != message.session.configuration) {
            if (!m_world_loading.bindConfiguration(
                    m_world_loading_identity,
                    message.session.configuration,
                    received_at
                )) {
                return false;
            }
            m_world_loading_identity.configuration = message.session.configuration;
        }
        try {
            m_authoritative_world = std::make_unique<shared::SparseWorld>(
                shared::SparseWorldOptions{
                    .seed = message.session.terrain.seed,
                    .max_resident_chunks = shared::MAX_CHUNK_KEYS_PER_MATERIALIZED_INTEREST_UPPER_BOUND,
                    .revision = message.session.terrain.revision,
                    .max_authoritative_columns = shared::MAX_AUTHORITATIVE_COLUMNS_PER_PLAYER,
                },
                shared::SparseWorldGenerationMode::MaterializedOnly
            );
        } catch (std::exception const& error) {
            static_cast<void>(m_world_loading.fail(
                m_world_loading_identity,
                std::string{ "Could not create remote materialized world storage: " } + error.what(),
                received_at
            ));
            return false;
        }
        m_world.setCollisionWorld(m_authoritative_world.get());
        m_predicted_world.setCollisionWorld(m_authoritative_world.get());
        if (!m_authoritative_world->setTerrainIdentity(message.session.terrain)) {
            static_cast<void>(m_world_loading.fail(
                m_world_loading_identity,
                "Could not bind the authoritative terrain identity to client chunk storage.",
                received_at
            ));
            return false;
        }
        m_terrain_identity = message.session.terrain;
        m_terrain_session = message.session;
        if (message.failure.empty()) {
            try {
                if (!m_terrain_lod_source) {
                    m_terrain_lod_source = std::make_unique<TerrainLodSource>();
                }
                auto const request_started_at = std::chrono::steady_clock::now();
                bool const requested = m_terrain_lod_source->request(message.session);
                auto const request_completed_at = std::chrono::steady_clock::now();
                CORE_INFO("startup-boundary request-l0 sequence={} steady_ns={} duration_us={} requested={}",
                    message.sequence,
                    std::chrono::duration_cast<std::chrono::nanoseconds>(request_started_at.time_since_epoch()).count(),
                    std::chrono::duration_cast<std::chrono::microseconds>(request_completed_at - request_started_at).count(),
                    requested);
                if (!requested) {
                    CORE_ERROR("Could not schedule the client terrain LOD for the active session");
                }
            } catch (std::exception const& error) {
                CORE_ERROR("Could not start the client terrain LOD source: {}", error.what());
            }
        }
        for (shared::ChunkCoordinate const coordinate : m_resident_height_tile_keys) {
            publishResidentSurfaceMetadata(coordinate);
        }
    }

    if (!m_world_loading.publishServerCreationElapsed(
            m_world_loading_identity,
            message.server_elapsed_microseconds,
            received_at
        )) {
        return false;
    }
    generation::GenerationProgress const progress{
        .stage = message.stage,
        .completed = message.work_completed,
        .total = message.work_total,
        .snapshot = message.latest_snapshot,
    };
    if (!m_world_loading.publishGenerationProgress(m_world_loading_identity, progress, received_at)) {
        return false;
    }
    m_last_world_preparation_sequence = message.sequence;

    if (!message.failure.empty()) {
        if (m_terrain_lod_source) {
            m_terrain_lod_source->cancel();
        }
        static_cast<void>(m_world_loading.fail(m_world_loading_identity, message.failure, received_at));
        return true;
    }
    if (message.final_atlas_complete && !m_world_loading.status(received_at).atlas_ready
        && !m_world_loading.publishAtlasCompletion(
            m_world_loading_identity,
            message.session.terrain,
            received_at
        )) {
        return false;
    }
    if (message.refined_safe_spawn) {
        WorldLoadingStatus const status = m_world_loading.status(received_at);
        if (status.safe_spawn_position) {
            if (!samePlayerPosition(*status.safe_spawn_position, *message.refined_safe_spawn)) {
                return false;
            }
        } else if (!m_world_loading.publishRefinedSpawn(
                       m_world_loading_identity,
                       *message.refined_safe_spawn,
                       isValidPlayerPosition(*message.refined_safe_spawn)
                           ? SpawnValidation::Safe : SpawnValidation::Unsafe,
                       received_at
                   )) {
            return false;
        }
    }
    return true;
}

bool GameClient::applyChunkCoverage(
    shared::ServerChunkCoverageMessage const& message,
    LoadingTimePoint const received_at
)
{
    if (!m_terrain_session
        || !validateSession(message.session)
        || message.coverage_epoch == 0U
        || message.actual_radius_blocks != shared::ACTUAL_TERRAIN_RADIUS_BLOCKS
        || message.visual_lod_radius_blocks != shared::FULL_WORLD_VISUAL_LOD_RADIUS_BLOCKS
        || !isValidPlayerPosition(message.center)
        || !m_authoritative_world) {
        return false;
    }
    WorldLoadingStatus const status = m_world_loading.status(received_at);
    if (!status.safe_spawn_position) {
        return false;
    }
    bool const new_epoch = !m_active_coverage_epoch
        || message.coverage_epoch > *m_active_coverage_epoch;
    if (new_epoch && !m_active_coverage_epoch
        && (message.coverage_epoch != 1U
            || !samePlayerPosition(*status.safe_spawn_position, message.center))) {
        return false;
    }
    if (m_active_coverage_epoch && message.coverage_epoch < *m_active_coverage_epoch) {
        return false;
    }
    if (m_active_coverage_epoch && message.coverage_epoch == *m_active_coverage_epoch) {
        if (!m_active_coverage_center) {
            return false;
        }
        if (samePlayerPosition(*m_active_coverage_center, message.center)) {
            if (!m_initial_chunk_credit_sent) {
                m_initial_chunk_credit_sent = sendChunkDeliveryAck(
                    message.coverage_epoch,
                    0U,
                    INITIAL_MATERIALIZED_CHUNK_CREDITS
                );
            }
            return m_initial_chunk_credit_sent;
        }
        if (!nearCoverageRemainsConnected(*m_active_coverage_center, message.center)) {
            return false;
        }
    }

    std::vector<shared::ChunkCoordinate> required_keys = shared::requiredChunkKeys(
        message.center,
        shared::NEAR_TERRAIN_RADIUS_BLOCKS
    );
    std::vector<shared::ChunkCoordinate> interest_keys = shared::materializedInterestChunkKeys(message.center);
    if (required_keys.size() > shared::MAX_CHUNK_KEYS_PER_NEAR_SPHERE_UPPER_BOUND
        || interest_keys.size() > shared::MAX_CHUNK_KEYS_PER_MATERIALIZED_INTEREST_UPPER_BOUND
        || !m_authoritative_world->setProtectedChunks(interest_keys)) {
        static_cast<void>(m_world_loading.fail(
            m_world_loading_identity,
            "The authoritative materialized-interest chunk set exceeds the client residency limit.",
            received_at
        ));
        return false;
    }
    if (new_epoch) {
        if (m_active_coverage_epoch.has_value()) {
            for (auto const& [delivery_token, batch] : m_pending_materialized_batches) {
                std::vector<shared::ChunkCoordinate> rejected = batch.not_resident_keys;
                rejected.insert(rejected.end(), batch.unresolved_coordinates.begin(), batch.unresolved_coordinates.end());
                if (!sendChunkDeliveryAck(
                    m_active_coverage_epoch.value_or(0U), delivery_token, 1U, std::move(rejected)
                )) {
                    return false;
                }
            }
        }
        for (shared::ChunkCoordinate const coordinate : m_connected_coverage_chunks) {
            if (!m_world_loading.evictChunk(m_world_loading_identity, coordinate, received_at)) {
                static_cast<void>(m_world_loading.fail(
                    m_world_loading_identity,
                    "A prior coverage epoch could not be removed from loading readiness.",
                    received_at
                ));
                return false;
            }
        }
        for (shared::ChunkCoordinate const coordinate : m_connected_authoritative_columns) {
            static_cast<void>(m_world_loading.evictAuthoritativeColumn(
                m_world_loading_identity, coordinate, received_at
            ));
        }
        clearActiveCoverage();
        m_initial_chunk_credit_sent = false;
        m_coverage_bfs_root = message.center;
        m_coverage_root_pending = true;
    } else {
        if (!m_active_coverage_center.has_value()) {
            return false;
        }
        std::vector<shared::ChunkCoordinate> const old_near_keys = shared::requiredChunkKeys(
            m_active_coverage_center.value(),
            WorldLoading::INITIAL_READY_SPHERE_RADIUS_BLOCKS
        );
        std::vector<shared::ChunkCoordinate> const new_near_keys = shared::requiredChunkKeys(
            message.center,
            WorldLoading::INITIAL_READY_SPHERE_RADIUS_BLOCKS
        );
        shared::ChunkCoordinate const coverage_center = chunkForPlayerPosition(message.center);
        std::vector<uint8_t> interest_slots(CONNECTIVITY_GRID_SLOT_COUNT, 0U);
        for (shared::ChunkCoordinate const key : interest_keys) {
            std::optional<uint32_t> const slot = connectivityGridSlot(key, coverage_center);
            if (!slot) {
                static_cast<void>(m_world_loading.fail(
                    m_world_loading_identity,
                    "Materialized-interest keys exceed the bounded local connectivity grid.",
                    received_at
                ));
                return false;
            }
            interest_slots[*slot] = 1U;
        }
        uint32_t const candidate_limit = static_cast<uint32_t>(
            interest_keys.size() < shared::MAX_CHUNK_KEYS_PER_MATERIALIZED_INTEREST_UPPER_BOUND
                ? interest_keys.size()
                : shared::MAX_CHUNK_KEYS_PER_MATERIALIZED_INTEREST_UPPER_BOUND
        );
        std::vector<shared::ChunkCoordinate> candidate_nodes;
        candidate_nodes.reserve(candidate_limit);
        std::vector<uint32_t> candidate_indices(CONNECTIVITY_GRID_SLOT_COUNT, NO_CONNECTIVITY_CANDIDATE);
        std::vector<uint8_t> visited(CONNECTIVITY_GRID_SLOT_COUNT, 0U);
        for (shared::ChunkCoordinate const key : m_connected_coverage_chunks) {
            std::optional<uint32_t> const slot = connectivityGridSlot(key, coverage_center);
            if (!slot || interest_slots[*slot] == 0U
                || m_authoritative_world->residentChunk(key) == nullptr) {
                continue;
            }
            if (candidate_nodes.size() >= candidate_limit) {
                static_cast<void>(m_world_loading.fail(
                    m_world_loading_identity,
                    "Connected coverage exceeds the bounded materialized-interest index.",
                    received_at
                ));
                return false;
            }
            uint32_t const index = static_cast<uint32_t>(candidate_nodes.size());
            candidate_nodes.push_back(key);
            candidate_indices[*slot] = index;
        }

        std::vector<uint32_t> connected_frontier;
        connected_frontier.reserve(candidate_nodes.size());
        for (shared::ChunkCoordinate const key : old_near_keys) {
            if (!std::binary_search(new_near_keys.begin(), new_near_keys.end(), key, isChunkCoordinateZyxLess)) {
                continue;
            }
            std::optional<uint32_t> const slot = connectivityGridSlot(key, coverage_center);
            if (!slot) {
                continue;
            }
            uint32_t const candidate_index = candidate_indices[*slot];
            if (candidate_index != NO_CONNECTIVITY_CANDIDATE && visited[*slot] == 0U) {
                visited[*slot] = 1U;
                connected_frontier.push_back(candidate_index);
            }
        }
        while (!connected_frontier.empty()) {
            uint32_t const current_index = connected_frontier.back();
            connected_frontier.pop_back();
            for (shared::ChunkCoordinate const neighbor : faceNeighbors(candidate_nodes[current_index])) {
                if (!shared::WorldBounds::isValidChunk(neighbor)) {
                    continue;
                }
                std::optional<uint32_t> const slot = connectivityGridSlot(neighbor, coverage_center);
                if (!slot) {
                    continue;
                }
                uint32_t const candidate_index = candidate_indices[*slot];
                if (candidate_index != NO_CONNECTIVITY_CANDIDATE && visited[*slot] == 0U) {
                    visited[*slot] = 1U;
                    connected_frontier.push_back(candidate_index);
                }
            }
        }

        std::vector<shared::ChunkCoordinate> removed_connected_keys;
        removed_connected_keys.reserve(m_connected_coverage_chunks.size() - candidate_nodes.size());
        for (shared::ChunkCoordinate const key : m_connected_coverage_chunks) {
            std::optional<uint32_t> const slot = connectivityGridSlot(key, coverage_center);
            if (slot && candidate_indices[*slot] != NO_CONNECTIVITY_CANDIDATE && visited[*slot] != 0U) {
                continue;
            }
            if (!m_world_loading.evictChunk(m_world_loading_identity, key, received_at)) {
                static_cast<void>(m_world_loading.fail(
                    m_world_loading_identity,
                    "Pruned connected coverage could not be removed from loading readiness.",
                    received_at
                ));
                return false;
            }
            removed_connected_keys.push_back(key);
        }
        for (shared::ChunkCoordinate const key : removed_connected_keys) {
            m_connected_coverage_chunks.erase(key);
            m_active_coverage_chunks.erase(key);
        }
        if (m_connected_coverage_chunks.empty()) {
            m_coverage_bfs_root = message.center;
            m_coverage_root_pending = true;
        } else {
            m_coverage_bfs_root.reset();
            m_coverage_root_pending = false;
        }
    }
    if (status.current_player_position
        && !samePlayerPosition(*status.current_player_position, message.center)
        && !m_world_loading.updatePlayerPosition(m_world_loading_identity, message.center, received_at)) {
        return false;
    }
    std::vector<shared::ChunkCoordinate> const profile_interest = shared::requiredTerrainColumns(
        message.center, shared::AUTHORITATIVE_PREFETCH_RADIUS_BLOCKS
    );
    if (!m_authoritative_world->setProtectedAuthoritativeColumns(profile_interest)) {
        return false;
    }
    m_authoritative_interest_columns = ChunkKeySet{ profile_interest.begin(), profile_interest.end() };
    auto const profile_required = shared::requiredTerrainColumns(message.center);
    m_required_authoritative_columns = ChunkKeySet{ profile_required.begin(), profile_required.end() };
    for (shared::ChunkCoordinate const key : m_authoritative_world->availableAuthoritativeColumns()) {
        if (!m_authoritative_interest_columns.contains(key)) {
            static_cast<void>(m_authoritative_world->removeAuthoritativeSurface(key));
            m_connected_authoritative_columns.erase(key);
            static_cast<void>(m_world_loading.evictAuthoritativeColumn(m_world_loading_identity, key, received_at));
        }
    }
    m_active_coverage_epoch = message.coverage_epoch;
    m_active_coverage_center = message.center;
    if (new_epoch) {
        m_active_coverage_required_set.clear();
        m_active_coverage_required_set.reserve(required_keys.size());
        m_active_coverage_required_set.insert(required_keys.begin(), required_keys.end());
        m_active_materialized_interest_set.clear();
        m_active_materialized_interest_set.reserve(interest_keys.size());
        m_active_materialized_interest_set.insert(interest_keys.begin(), interest_keys.end());
    } else {
        auto previous_key = m_active_coverage_required_keys.begin();
        auto next_key = required_keys.begin();
        while (previous_key != m_active_coverage_required_keys.end()
            && next_key != required_keys.end()) {
            if (isChunkCoordinateZyxLess(*previous_key, *next_key)) {
                m_active_coverage_required_set.erase(*previous_key);
                m_active_coverage_chunks.erase(*previous_key);
                ++previous_key;
            } else if (isChunkCoordinateZyxLess(*next_key, *previous_key)) {
                m_active_coverage_required_set.insert(*next_key);
                if (m_connected_coverage_chunks.contains(*next_key)
                    && m_authoritative_world->residentChunk(*next_key) != nullptr) {
                    m_active_coverage_chunks.insert(*next_key);
                }
                ++next_key;
            } else {
                ++previous_key;
                ++next_key;
            }
        }
        while (previous_key != m_active_coverage_required_keys.end()) {
            m_active_coverage_required_set.erase(*previous_key);
            m_active_coverage_chunks.erase(*previous_key);
            ++previous_key;
        }
        while (next_key != required_keys.end()) {
            m_active_coverage_required_set.insert(*next_key);
            if (m_connected_coverage_chunks.contains(*next_key)
                && m_authoritative_world->residentChunk(*next_key) != nullptr) {
                m_active_coverage_chunks.insert(*next_key);
            }
            ++next_key;
        }

        for (auto interest_key = m_active_materialized_interest_set.begin();
            interest_key != m_active_materialized_interest_set.end();) {
            if (!std::binary_search(
                    interest_keys.begin(),
                    interest_keys.end(),
                    *interest_key,
                    isChunkCoordinateZyxLess
                )) {
                interest_key = m_active_materialized_interest_set.erase(interest_key);
            } else {
                ++interest_key;
            }
        }
        m_active_materialized_interest_set.reserve(interest_keys.size());
        for (shared::ChunkCoordinate const key : interest_keys) {
            m_active_materialized_interest_set.insert(key);
        }
    }
    m_active_coverage_required_keys = std::move(required_keys);
    if (new_epoch) {
        m_initial_chunk_credit_sent = sendChunkDeliveryAck(
            message.coverage_epoch,
            0U,
            INITIAL_MATERIALIZED_CHUNK_CREDITS
        );
    }
    processPendingMaterializedChunks(received_at);
    return new_epoch ? m_initial_chunk_credit_sent : true;
}

uint32_t GameClient::requiredAuthoritativeColumnCount() const noexcept
{
    return static_cast<uint32_t>(m_required_authoritative_columns.size());
}

uint32_t GameClient::residentAuthoritativeColumnCount() const noexcept
{
    return static_cast<uint32_t>(std::ranges::count_if(m_required_authoritative_columns, [this](auto const key) {
        return m_connected_authoritative_columns.contains(key) && m_authoritative_world != nullptr
            && m_authoritative_world->hasAuthoritativeSurface(key);
    }));
}

bool GameClient::applyAuthoritativeTerrainBatch(
    shared::ServerAuthoritativeTerrainBatchMessage const& message,
    LoadingTimePoint const received_at
)
{
    if (!m_terrain_session || !validateSession(message.session)
        || !m_active_coverage_epoch || message.coverage_epoch == 0U || !m_active_coverage_center
        || !m_authoritative_world || message.delivery_token == 0U
        || message.tiles.empty() || message.tiles.size() > shared::AUTHORITATIVE_TERRAIN_BATCH_CAPACITY) {
        return false;
    }
    if (message.coverage_epoch < *m_active_coverage_epoch) {
        std::vector<shared::ChunkCoordinate> rejected;
        rejected.reserve(message.tiles.size());
        for (auto const& tile : message.tiles) {
            rejected.push_back(tile.coordinate);
        }
        static_cast<void>(send(shared::ClientAuthoritativeTerrainAckMessage{
            .session = *m_terrain_session,
            .coverage_epoch = message.coverage_epoch,
            .delivery_token = message.delivery_token,
            .not_resident_keys = std::move(rejected),
        }));
        return false;
    }
    if (message.coverage_epoch != *m_active_coverage_epoch
        || message.delivery_token <= m_last_authoritative_delivery_token) {
        return false;
    }
    shared::ChunkCoordinate root = chunkForPlayerPosition(*m_active_coverage_center);
    root.z = 0;
    std::vector<shared::ChunkCoordinate> rejected;
    for (shared::AuthoritativeTerrainTile const& tile : message.tiles) {
        bool const connected = tile.coordinate == root || m_connected_authoritative_columns.contains(tile.coordinate)
            || std::ranges::any_of(std::array<shared::ChunkCoordinate, 4>{
                shared::ChunkCoordinate{ (tile.coordinate.x + 4'095) % 4'096, tile.coordinate.y, 0 },
                shared::ChunkCoordinate{ (tile.coordinate.x + 1) % 4'096, tile.coordinate.y, 0 },
                shared::ChunkCoordinate{ tile.coordinate.x, (tile.coordinate.y + 4'095) % 4'096, 0 },
                shared::ChunkCoordinate{ tile.coordinate.x, (tile.coordinate.y + 1) % 4'096, 0 },
            }, [this](auto const neighbor) { return m_connected_authoritative_columns.contains(neighbor); });
        if (tile.identity != message.session.terrain || !m_authoritative_interest_columns.contains(tile.coordinate)
            || !connected || !m_authoritative_world->publishAuthoritativeSurface(tile)) {
            rejected.push_back(tile.coordinate);
            continue;
        }
        m_connected_authoritative_columns.insert(tile.coordinate);
        if (!m_world_loading.publishAuthoritativeColumn(m_world_loading_identity, tile.coordinate, received_at)) {
            rejected.push_back(tile.coordinate);
        }
    }
    m_last_authoritative_delivery_token = message.delivery_token;
    return send(shared::ClientAuthoritativeTerrainAckMessage{
        .session = *m_terrain_session, .coverage_epoch = message.coverage_epoch,
        .delivery_token = message.delivery_token, .not_resident_keys = std::move(rejected),
    });
}

bool GameClient::applyMaterializedChunkBatch(
    shared::ServerMaterializedChunkBatchMessage const& message,
    LoadingTimePoint const received_at
)
{
    if (!m_terrain_session
        || !validateSession(message.session)
        || !m_active_coverage_epoch.has_value()
        || message.coverage_epoch == 0U
        || message.delivery_token == 0U
        || message.chunks.empty()
        || message.chunks.size() > shared::MATERIALIZED_CHUNK_BATCH_CAPACITY
        || !m_authoritative_world) {
        return false;
    }
    uint64_t const active_coverage_epoch = m_active_coverage_epoch.value();
    if (message.coverage_epoch < active_coverage_epoch) {
        std::vector<shared::ChunkCoordinate> rejected;
        rejected.reserve(message.chunks.size());
        for (auto const& chunk : message.chunks) {
            rejected.push_back(chunk.coordinate);
        }
        static_cast<void>(send(shared::ClientChunkDeliveryAckMessage{
            .session = *m_terrain_session,
            .coverage_epoch = message.coverage_epoch,
            .delivery_token = message.delivery_token,
            .credits = 1U,
            .not_resident_keys = std::move(rejected),
        }));
        return false;
    }
    if (message.coverage_epoch != active_coverage_epoch
        || m_pending_materialized_batches.size() >= MAX_PENDING_MATERIALIZED_BATCHES
        || m_pending_materialized_batches.contains(message.delivery_token)) {
        return false;
    }

    PendingMaterializedBatch batch{
        .delivery_token = message.delivery_token,
        .unresolved_coordinates = {},
        .not_resident_keys = {},
    };
    batch.unresolved_coordinates.reserve(message.chunks.size());
    batch.not_resident_keys.reserve(message.chunks.size());
    ChunkKeySet batch_coordinates;
    batch_coordinates.reserve(message.chunks.size());
    m_pending_materialized_batches.emplace(message.delivery_token, std::move(batch));
    for (shared::MaterializedChunkPayload const& payload : message.chunks) {
        PendingMaterializedBatch& pending_batch = m_pending_materialized_batches.at(message.delivery_token);
        if (!batch_coordinates.insert(payload.coordinate).second) {
            continue;
        }
        std::optional<shared::Chunk> chunk = shared::materializeChunkPayload(payload);
        if (!chunk || !isActiveMaterializedInterestKey(payload.coordinate)) {
            pending_batch.not_resident_keys.push_back(payload.coordinate);
            continue;
        }
        if (m_active_coverage_chunks.contains(payload.coordinate)
            && m_authoritative_world->residentChunk(payload.coordinate) != nullptr) {
            continue;
        }
        if (m_pending_materialized_chunks.size() >= MAX_PENDING_MATERIALIZED_CHUNKS
            || !m_pending_materialized_keys.insert(payload.coordinate).second) {
            pending_batch.not_resident_keys.push_back(payload.coordinate);
            continue;
        }
        pending_batch.unresolved_coordinates.push_back(payload.coordinate);
        m_pending_materialized_chunks.push_back({
            .chunk = *chunk,
            .delivery_token = message.delivery_token,
        });
    }
    processPendingMaterializedChunks(received_at);
    auto const completed_batch = m_pending_materialized_batches.find(message.delivery_token);
    if (completed_batch != m_pending_materialized_batches.end()
        && completed_batch->second.unresolved_coordinates.empty()) {
        std::vector<shared::ChunkCoordinate> const not_resident = completed_batch->second.not_resident_keys;
        if (sendChunkDeliveryAck(active_coverage_epoch, message.delivery_token, 1U, not_resident)) {
            m_pending_materialized_batches.erase(completed_batch);
            return true;
        }
        return false;
    }
    return true;
}

bool GameClient::applyStartWorldAck(
    shared::ServerStartWorldAckMessage const& message,
    LoadingTimePoint const acknowledged_at
)
{
    if (!m_terrain_session || !validateSession(message.session)) {
        return false;
    }
    bool const applied = m_world_loading.acknowledgeStart(
        m_world_loading_identity,
        message.accepted,
        message.failure,
        acknowledged_at
    );
    if (applied) {
        m_start_message_sent = false;
    }
    return applied;
}

void GameClient::markLoadingPromptRendered(LoadingTimePoint const rendered_at)
{
    static_cast<void>(m_world_loading.markReadyPromptFrameRendered(m_world_loading_identity, rendered_at));
}

LoadingInputResult GameClient::acceptLoadingStartInput(LoadingTimePoint const accepted_at)
{
    LoadingInputResult const result = m_world_loading.acceptStartInput(m_world_loading_identity, accepted_at);
    if (result != LoadingInputResult::Started) {
        return result;
    }
    if (!m_accepted || !m_terrain_session || m_start_message_sent
        || !send(shared::ClientStartWorldMessage{ .session = *m_terrain_session })) {
        static_cast<void>(m_world_loading.fail(
            m_world_loading_identity,
            "Could not send the world start request to the server.",
            accepted_at
        ));
        return LoadingInputResult::Consumed;
    }
    m_start_message_sent = true;
    return LoadingInputResult::Started;
}

bool GameClient::sendChunkDeliveryAck(
    uint64_t const coverage_epoch,
    uint64_t const delivery_token,
    uint8_t const credits,
    std::vector<shared::ChunkCoordinate> not_resident_keys
)
{
    return m_terrain_session && m_active_coverage_epoch == coverage_epoch
        && send(shared::ClientChunkDeliveryAckMessage{
            .session = *m_terrain_session,
            .coverage_epoch = coverage_epoch,
            .delivery_token = delivery_token,
            .credits = credits,
            .not_resident_keys = std::move(not_resident_keys),
        });
}

void GameClient::processPendingMaterializedChunks(LoadingTimePoint const received_at)
{
    bool changed = true;
    while (changed && m_active_coverage_epoch && m_authoritative_world) {
        changed = false;
        for (auto iterator = m_pending_materialized_chunks.begin();
             iterator != m_pending_materialized_chunks.end();) {
            shared::ChunkCoordinate const coordinate = iterator->chunk.coordinate();
            if (!isActiveMaterializedInterestKey(coordinate)) {
                uint64_t const delivery_token = iterator->delivery_token;
                iterator = m_pending_materialized_chunks.erase(iterator);
                resolvePendingMaterializedChunk(delivery_token, coordinate, true, received_at);
                changed = true;
                continue;
            }
            if (!connectedToActiveCoverage(coordinate)) {
                ++iterator;
                continue;
            }

            bool admitted = true;
            shared::Chunk const* const resident = m_authoritative_world->residentChunk(coordinate);
            if (resident != nullptr) {
                admitted = resident->contentIdentity().content_hash
                    == iterator->chunk.contentIdentity().content_hash;
            } else {
                std::optional<shared::ChunkCoordinate> evicted_coordinate;
                admitted = m_authoritative_world->publishMaterializedChunk(
                    iterator->chunk,
                    m_terrain_session->terrain.revision,
                    m_terrain_session->terrain.seed,
                    &evicted_coordinate
                );
                if (evicted_coordinate) {
                    m_active_coverage_chunks.erase(*evicted_coordinate);
                    m_connected_coverage_chunks.erase(*evicted_coordinate);
                    if (!m_world_loading.evictChunk(m_world_loading_identity, *evicted_coordinate, received_at)) {
                        static_cast<void>(m_world_loading.fail(
                            m_world_loading_identity,
                            "Client chunk residency changed outside the loading evidence timeline.",
                            received_at
                        ));
                        admitted = false;
                    }
                }
            }
            if (admitted) {
                shared::Chunk const& admitted_chunk = resident != nullptr
                    ? *resident : *m_authoritative_world->residentChunk(coordinate);
                bool recorded = false;
                if (m_benchmark_hooks && m_benchmark_hooks->on_materialized_chunk_recorded) {
                    WorldLoadingStatus const before = m_world_loading.status(received_at);
                    recorded = m_world_loading.publishMaterializedChunk(
                        m_world_loading_identity,
                        admitted_chunk,
                        received_at
                    );
                    if (recorded) {
                        WorldLoadingStatus const after = m_world_loading.status(received_at);
                        m_benchmark_hooks->on_materialized_chunk_recorded(
                            *m_active_coverage_epoch,
                            iterator->delivery_token,
                            coordinate,
                            before,
                            after
                        );
                    }
                } else {
                    recorded = m_world_loading.publishMaterializedChunk(
                        m_world_loading_identity,
                        admitted_chunk,
                        received_at
                    );
                }
                if (!recorded) {
                    static_cast<void>(m_world_loading.fail(
                        m_world_loading_identity,
                        "A validated chunk could not be recorded by the loading state.",
                        received_at
                    ));
                    admitted = false;
                }
            }
            if (admitted) {
                m_connected_coverage_chunks.insert(coordinate);
                if (m_coverage_bfs_root
                    && coordinate == chunkForPlayerPosition(*m_coverage_bfs_root)) {
                    m_coverage_root_pending = false;
                }
                if (isActiveCoverageKey(coordinate)) {
                    m_active_coverage_chunks.insert(coordinate);
                }
            }
            uint64_t const delivery_token = iterator->delivery_token;
            iterator = m_pending_materialized_chunks.erase(iterator);
            resolvePendingMaterializedChunk(delivery_token, coordinate, !admitted, received_at);
            changed = true;
        }
    }

    for (auto iterator = m_pending_materialized_batches.begin(); iterator != m_pending_materialized_batches.end();) {
        if (iterator->second.unresolved_coordinates.empty()) {
            if (sendChunkDeliveryAck(
                    *m_active_coverage_epoch,
                    iterator->first,
                    1U,
                    iterator->second.not_resident_keys
                )) {
                iterator = m_pending_materialized_batches.erase(iterator);
                continue;
            }
        }
        ++iterator;
    }
}

void GameClient::resolvePendingMaterializedChunk(
    uint64_t const delivery_token,
    shared::ChunkCoordinate const coordinate,
    bool const not_resident,
    LoadingTimePoint const received_at
)
{
    m_pending_materialized_keys.erase(coordinate);
    auto const batch = m_pending_materialized_batches.find(delivery_token);
    if (batch == m_pending_materialized_batches.end()) {
        return;
    }
    auto const unresolved = std::find(
        batch->second.unresolved_coordinates.begin(),
        batch->second.unresolved_coordinates.end(),
        coordinate
    );
    if (unresolved == batch->second.unresolved_coordinates.end()) {
        return;
    }
    batch->second.unresolved_coordinates.erase(unresolved);
    if (not_resident
        && std::find(batch->second.not_resident_keys.begin(), batch->second.not_resident_keys.end(), coordinate)
            == batch->second.not_resident_keys.end()) {
        batch->second.not_resident_keys.push_back(coordinate);
    }
    if (batch->second.unresolved_coordinates.empty() && m_active_coverage_epoch
        && sendChunkDeliveryAck(
            *m_active_coverage_epoch,
            delivery_token,
            1U,
            batch->second.not_resident_keys
        )) {
        m_pending_materialized_batches.erase(batch);
    }
    static_cast<void>(received_at);
}

bool GameClient::connectedToActiveCoverage(shared::ChunkCoordinate const coordinate) const noexcept
{
    if (!m_active_coverage_center) {
        return false;
    }
    if (m_coverage_root_pending && m_coverage_bfs_root
        && coordinate == chunkForPlayerPosition(*m_coverage_bfs_root)) {
        return true;
    }
    if (m_connected_coverage_chunks.contains(coordinate)) {
        return true;
    }
    return std::ranges::any_of(faceNeighbors(coordinate), [this](shared::ChunkCoordinate const neighbor) {
        return shared::WorldBounds::isValidChunk(neighbor) && m_connected_coverage_chunks.contains(neighbor);
    });
}

bool GameClient::isActiveCoverageKey(shared::ChunkCoordinate const coordinate) const noexcept
{
    return m_active_coverage_required_set.contains(coordinate);
}

bool GameClient::isActiveMaterializedInterestKey(shared::ChunkCoordinate const coordinate) const noexcept
{
    return m_active_materialized_interest_set.contains(coordinate);
}

void GameClient::clearActiveCoverage() noexcept
{
    m_active_coverage_epoch.reset();
    m_active_coverage_center.reset();
    m_coverage_bfs_root.reset();
    m_active_coverage_required_keys.clear();
    m_required_authoritative_columns.clear();
    m_authoritative_interest_columns.clear();
    m_connected_authoritative_columns.clear();
    m_last_authoritative_delivery_token = 0U;
    m_active_coverage_required_set.clear();
    m_active_materialized_interest_set.clear();
    m_active_coverage_chunks.clear();
    m_connected_coverage_chunks.clear();
    m_coverage_root_pending = false;
    m_pending_materialized_keys.clear();
    m_pending_materialized_batches.clear();
    m_pending_materialized_chunks.clear();
}

void GameClient::removeResidentSurfaceMetadata(shared::ChunkCoordinate const coordinate)
{
    m_resident_height_tile_keys.erase(coordinate);
    if (m_authoritative_world) {
        static_cast<void>(m_authoritative_world->removeSurfaceMetadata(coordinate));
    }
}

void GameClient::clearResidentSurfaceMetadata()
{
    if (m_authoritative_world) {
        for (shared::ChunkCoordinate const coordinate : m_resident_height_tile_keys) {
            static_cast<void>(m_authoritative_world->removeSurfaceMetadata(coordinate));
        }
    }
    m_resident_height_tile_keys.clear();
}

void GameClient::publishResidentSurfaceMetadata(shared::ChunkCoordinate const coordinate)
{
    if (!m_authoritative_world || !m_terrain_session || coordinate.z != 0) {
        return;
    }
    HeightTileHandle const tile = m_height_tile_residency.resident({ .x = coordinate.x, .y = coordinate.y });
    if (tile && tile->surface().identity == m_terrain_session->terrain) {
        static_cast<void>(m_authoritative_world->publishSurfaceMetadata(tile->surface()));
    }
}

bool GameClient::sendJoinRequest()
{
    return send(shared::JoinRequestMessage{
        .ch = m_local_character,
        .mode = m_world.mode(),
        .configuration = m_world.configuration(),
        .wants_previews = m_wants_previews && m_world.mode() == shared::WorldMode::Flight,
        .join_epoch = m_world_loading_identity.join_epoch,
    });
}

void GameClient::applyServerRemoval(char const character)
{
    if (auto const player = m_world.playerByCharacter(character)) {
        m_player_presentation.remove(character);
        m_world.despawnPlayer(player->id);
        m_state_revisions.erase(character);
        rebuildPrediction();
    }
}

std::optional<shared::ClientInputMessage> GameClient::predictInput(
    shared::Direction const direction,
    std::chrono::steady_clock::time_point const predicted_at
)
{
    if (m_pending_inputs.size() == MAX_PENDING_INPUTS) {
        return std::nullopt;
    }
    auto const player = m_predicted_world.playerByCharacter(m_local_character);
    if (!player) {
        return std::nullopt;
    }
    shared::ClientInputMessage const input{
        .direction = direction,
        .sequence = m_next_input_sequence++,
    };
    m_pending_inputs.push_back(input);
    m_last_predicted_direction = direction;
    m_last_predicted_direction->cycle_movement_capabilities = false;
    static_cast<void>(m_predicted_world.movePlayer(player->id, direction));
    refreshMovementForecast();
    updatePredictedPresentation(predicted_at, PlayerPresentationUpdateKind::Prediction);
    return input;
}

void GameClient::discardPredictedInput(
    uint32_t const sequence,
    std::chrono::steady_clock::time_point const discarded_at
)
{
    auto const found = std::find_if(m_pending_inputs.begin(), m_pending_inputs.end(), [sequence](
        shared::ClientInputMessage const& input
    ) {
        return input.sequence == sequence;
    });
    if (found != m_pending_inputs.end()) {
        if (std::next(found) == m_pending_inputs.end()) {
            m_last_predicted_direction.reset();
            if (found != m_pending_inputs.begin()) {
                m_last_predicted_direction = std::prev(found)->direction;
                m_last_predicted_direction->cycle_movement_capabilities = false;
            }
        }
        m_pending_inputs.erase(found);
        rebuildPrediction();
        updatePredictedPresentation(discarded_at, PlayerPresentationUpdateKind::Reconciliation);
    }
}

bool GameClient::applyServerPosition(
    shared::ServerPlayerPositionMessage const& message,
    std::chrono::steady_clock::time_point const received_at
)
{
    bool const local = message.ch == m_local_character;
    if (local) {
        m_last_received_input_ack = message.acknowledged_input_sequence;
        m_last_received_position_revision = message.state_revision;
        m_last_received_position_at = received_at;
        m_last_received_position_accepted = false;
    }
    auto const known_revision = m_state_revisions.find(message.ch);
    if (known_revision != m_state_revisions.end()
        && !shared::isNewerSequence(message.state_revision, known_revision->second)) {
        if (local) {
            ++m_rejected_local_position_count;
        }
        return false;
    }
    auto const player = m_world.playerByCharacter(message.ch);
    if (!m_world.applyReplicatedPlayerState({
            .id = player ? player->id : m_next_id,
            .x = message.x,
            .y = message.y,
            .z = message.z,
            .x_subcell = message.x_subcell,
            .y_subcell = message.y_subcell,
            .z_subcell = message.z_subcell,
            .ch = message.ch,
            .palette_index = message.palette_index,
            .movement_capabilities = message.movement_capabilities,
            .vertical_velocity_subcells = message.vertical_velocity_subcells,
        })) {
        if (local) {
            ++m_rejected_local_position_count;
        }
        return false;
    }
    if (!player) {
        ++m_next_id;
    }
    if (local) {
        m_last_received_position_accepted = true;
        m_last_accepted_input_ack = message.acknowledged_input_sequence;
        m_last_accepted_position_revision = message.state_revision;
        m_last_accepted_position_at = received_at;
    }
    m_state_revisions.insert_or_assign(message.ch, message.state_revision);
    if (message.ch == m_local_character) {
        while (!m_pending_inputs.empty()
            && !shared::isNewerSequence(
                m_pending_inputs.front().sequence,
                message.acknowledged_input_sequence
            )) {
            m_pending_inputs.pop_front();
        }
        rebuildPrediction();
        updatePredictedPresentation(received_at, PlayerPresentationUpdateKind::Reconciliation);
        if (m_terrain_session && m_authoritative_world) {
            shared::PlayerPosition const authoritative_position{
                .x = message.x,
                .y = message.y,
                .z = message.z,
                .x_subcell = message.x_subcell,
                .y_subcell = message.y_subcell,
                .z_subcell = message.z_subcell,
            };
            WorldLoadingStatus const loading_status = m_world_loading.status(received_at);
            if (loading_status.safe_spawn_position
                && isValidPlayerPosition(authoritative_position)
                && !m_world_loading.updatePlayerPosition(
                    m_world_loading_identity,
                    authoritative_position,
                    received_at
                )) {
                static_cast<void>(m_world_loading.fail(
                    m_world_loading_identity,
                    "An authoritative player update could not be applied to loading coverage.",
                    received_at
                ));
            }
        }
    } else {
        rebuildPrediction();
        updatePredictedPresentation(received_at, PlayerPresentationUpdateKind::Reconciliation);
        if (auto const updated_player = m_world.playerByCharacter(message.ch)) {
            m_player_presentation.update(*updated_player, received_at);
        }
    }
    return true;
}

std::optional<shared::Player> GameClient::predictedLocalPlayer() const noexcept
{
    return m_predicted_world.playerByCharacter(m_local_character);
}

std::optional<shared::PlayerPosition> GameClient::forecastedLocalPlayerPosition() const noexcept
{
    return m_forecast_player_position;
}

void GameClient::refreshMovementForecast()
{
    auto const player = predictedLocalPlayer();
    m_forecast_player_position.reset();
    if (!player || !m_last_predicted_direction) {
        return;
    }
    m_forecast_player_position = m_predicted_world.forecastFlightPlayerPosition(
        player->id, *m_last_predicted_direction);
}

std::optional<PlayerPresentationPosition> GameClient::predictedLocalPresentation(
    std::chrono::steady_clock::time_point const now
) const noexcept
{
    return m_player_presentation.sample(m_local_character, now);
}

void GameClient::rebuildPrediction()
{
    m_predicted_world = m_world;
    if (auto const player = m_predicted_world.playerByCharacter(m_local_character)) {
        for (shared::ClientInputMessage const& input : m_pending_inputs) {
            static_cast<void>(m_predicted_world.movePlayer(player->id, input.direction));
        }
    }
    refreshMovementForecast();
}

void GameClient::updatePredictedPresentation(
    std::chrono::steady_clock::time_point const updated_at,
    PlayerPresentationUpdateKind const update_kind
)
{
    if (auto const player = m_predicted_world.playerByCharacter(m_local_character)) {
        m_player_presentation.update(*player, updated_at, update_kind);
    }
}

bool GameClient::send(shared::Message const& message)
{
    std::vector const message_bytes = shared::encodeMessage(message);
    if (!core::Client::send(message_bytes, shared::GAME_CHANNEL, core::SendMode{ core::SendMode::Reliable })) {
        CORE_ERROR("Failed to send a message");
        return false;
    }
    if (m_benchmark_hooks) {
        m_benchmark_bytes_sent += message_bytes.size();
    }
    return true;
}

bool GameClient::applyHeightTileDescriptor(shared::ServerHeightTileDescriptorMessage const& message)
{
    std::optional<uint32_t> const radius = shared::heightTileInterestRadiusForCount(message.max_height_tiles);
    if (message.configuration != m_world.configuration()) {
        return false;
    }
    if (message.world_revision == 0U || message.world_revision < m_height_tile_revision.generation || !radius
        || message.world_identity.revision != message.world_revision
        || message.world_identity.seed != message.configuration.seed
        || !shared::isValidTerrainIdentity(message.world_identity)
        || message.max_height_tile_bytes != shared::HEIGHT_TILE_PAYLOAD_BYTES) {
        return false;
    }
    if (m_render_distance && m_render_distance != radius) {
        return false;
    }
    if (m_terrain_identity && message.world_revision == m_height_tile_revision.generation
        && message.world_identity != *m_terrain_identity) {
        return false;
    }
    if (!m_render_distance) {
        uint32_t const capacity = message.max_height_tiles + PreviewResidencyLimits::HYSTERESIS_TILES;
        std::string const resident_configuration_identity = message.world_identity.configuration_identity;
        uint64_t const resident_bytes_per_tile = static_cast<uint64_t>(sizeof(HeightTile))
            + static_cast<uint64_t>(resident_configuration_identity.capacity()) + 1U;
        m_height_tile_residency = PreviewResidency{m_height_tile_revision, {
            .max_resident_tiles = capacity,
            .max_resident_bytes = static_cast<uint64_t>(capacity) * resident_bytes_per_tile,
        }};
        m_render_distance = radius;
    }
    if (message.world_revision != m_height_tile_revision.generation) {
        clearResidentSurfaceMetadata();
        m_height_tile_revision = {
            .generation = message.world_revision,
            .revision = message.world_revision,
        };
        m_height_tile_residency.advanceRevision(m_height_tile_revision);
        m_pending_height_tile_deliveries.clear();
        m_pending_height_tile_delivery_tokens.clear();
    }
    m_terrain_identity = message.world_identity;
    if (m_height_tile_credit_revision == message.world_revision) {
        return true;
    }
    m_height_tile_credit_revision = message.world_revision;
    return grantHeightTileCredit(0U, shared::HEIGHT_TILE_DELIVERY_WINDOW);
}

bool GameClient::applyWorldRevision(shared::ServerWorldRevisionMessage const& message)
{
    if (message.world_revision == 0U || message.world_revision < m_height_tile_revision.generation) {
        return false;
    }
    if (message.world_revision != m_height_tile_revision.generation) {
        clearResidentSurfaceMetadata();
        m_height_tile_revision = {
            .generation = message.world_revision,
            .revision = message.world_revision,
        };
        m_height_tile_residency.advanceRevision(m_height_tile_revision);
        m_pending_height_tile_deliveries.clear();
        m_pending_height_tile_delivery_tokens.clear();
        if (m_terrain_identity) {
            m_terrain_identity->revision = message.world_revision;
        }
    }
    if (m_height_tile_credit_revision == message.world_revision) {
        return true;
    }
    m_height_tile_credit_revision = message.world_revision;
    return grantHeightTileCredit(0U, shared::HEIGHT_TILE_DELIVERY_WINDOW);
}

bool GameClient::grantHeightTileCredit(
    uint64_t const delivery_token,
    uint8_t const credits,
    std::vector<shared::HeightTileKey> not_resident_keys
)
{
    return send(shared::ClientHeightTileCreditMessage{
        .world_revision = m_height_tile_revision.generation,
        .delivery_token = delivery_token,
        .credits = credits,
        .not_resident_keys = std::move(not_resident_keys),
    });
}

bool GameClient::queueHeightTileDelivery(shared::ServerHeightTileBatchMessage message)
{
    if (message.delivery_token == 0U
        || m_pending_height_tile_deliveries.size() >= shared::HEIGHT_TILE_DELIVERY_WINDOW
        || !m_pending_height_tile_delivery_tokens.insert(message.delivery_token).second) {
        return false;
    }
    m_pending_height_tile_deliveries.push_back(std::move(message));
    return true;
}

void GameClient::processPendingHeightTileDeliveries()
{
    uint32_t constexpr MAX_PENDING_HANDOFF_CHANGES = shared::HEIGHT_TILE_BATCH_CAPACITY
        * shared::HEIGHT_TILE_DELIVERY_WINDOW;
    while (!m_pending_height_tile_deliveries.empty()) {
        shared::ServerHeightTileBatchMessage const& delivery = m_pending_height_tile_deliveries.front();
        uint32_t const operation_count = static_cast<uint32_t>(
            delivery.tiles.size() + delivery.removals.size()
        );
        if (m_height_tile_residency.pendingChangeCount() + operation_count > MAX_PENDING_HANDOFF_CHANGES) {
            return;
        }
        std::vector<shared::HeightTileKey> not_resident_keys = applyHeightTileBatch(delivery);
        uint64_t const delivery_token = delivery.delivery_token;
        m_pending_height_tile_delivery_tokens.erase(delivery_token);
        m_pending_height_tile_deliveries.pop_front();
        if (!grantHeightTileCredit(delivery_token, 1U, std::move(not_resident_keys))) {
            m_running = false;
            return;
        }
        flush();
    }
}

bool GameClient::applyHeightTile(shared::ServerHeightTileMessage const& message)
{
    std::optional<shared::HeightTileKey> evicted_key;
    return applyHeightTileAndTrackEviction(message, evicted_key);
}

bool GameClient::applyHeightTileAndTrackEviction(
    shared::ServerHeightTileMessage const& message,
    std::optional<shared::HeightTileKey>& evicted_key
)
{
    evicted_key.reset();
    if (!isHeightTileValidForWorld(message, m_height_tile_revision.generation, m_terrain_identity)) {
        return false;
    }
    HeightTileReplacementResult const result = m_height_tile_residency.accept(
        message.key,
        { .generation = m_height_tile_revision.generation, .revision = message.revision },
        message.token,
        message.heights,
        message.surface
    );
    evicted_key = result.evicted_key;
    if (evicted_key) {
        removeResidentSurfaceMetadata({ .x = evicted_key->x, .y = evicted_key->y, .z = 0 });
    }
    if (result.replacement != HeightTileReplacement::Published) {
        return false;
    }

    shared::ChunkCoordinate const coordinate{ .x = message.key.x, .y = message.key.y, .z = 0 };
    m_resident_height_tile_keys.insert(coordinate);
    publishResidentSurfaceMetadata(coordinate);
    return true;
}

std::vector<shared::HeightTileKey> GameClient::applyHeightTileBatch(
    shared::ServerHeightTileBatchMessage const& message
)
{
    auto const append_if_missing = [this](
        std::vector<shared::HeightTileKey>& keys,
        shared::HeightTileKey const key
    ) {
        if (!m_height_tile_residency.resident(key)
            && std::ranges::find(keys, key) == keys.end()) {
            keys.push_back(key);
        }
    };
    auto const sort_keys = [](std::vector<shared::HeightTileKey>& keys) {
        std::ranges::sort(keys, [](
            shared::HeightTileKey const first,
            shared::HeightTileKey const second
        ) {
            return first.x < second.x || (first.x == second.x && first.y < second.y);
        });
    };

    auto const invalid_tile = std::ranges::find_if(message.tiles, [this](shared::ServerHeightTileMessage const& tile) {
        return !isHeightTileValidForWorld(tile, m_height_tile_revision.generation, m_terrain_identity);
    });
    if (invalid_tile != message.tiles.end()) {
        std::vector<shared::HeightTileKey> not_resident_keys;
        not_resident_keys.reserve(message.tiles.size());
        for (shared::ServerHeightTileMessage const& tile : message.tiles) {
            if (tile.key == shared::normalizeHeightTileKey(tile.key)
                && tile.revision != 0U && tile.token != 0U
                && m_height_tile_revision.generation != 0U) {
                append_if_missing(not_resident_keys, tile.key);
            }
        }
        sort_keys(not_resident_keys);
        return not_resident_keys;
    }

    for (shared::ServerRemoveHeightTileMessage const& removal : message.removals) {
        static_cast<void>(applyHeightTileRemoval(removal));
    }

    std::vector<shared::HeightTileKey> evicted_keys;
    evicted_keys.reserve(message.tiles.size());
    for (shared::ServerHeightTileMessage const& tile : message.tiles) {
        std::optional<shared::HeightTileKey> evicted_key;
        static_cast<void>(applyHeightTileAndTrackEviction(tile, evicted_key));
        if (evicted_key) {
            evicted_keys.push_back(*evicted_key);
        }
    }

    std::vector<shared::HeightTileKey> not_resident_keys;
    not_resident_keys.reserve(message.tiles.size() + evicted_keys.size());
    for (shared::HeightTileKey const key : evicted_keys) {
        append_if_missing(not_resident_keys, key);
    }
    for (shared::ServerHeightTileMessage const& tile : message.tiles) {
        if (tile.key == shared::normalizeHeightTileKey(tile.key)
            && tile.revision != 0U && tile.token != 0U
            && m_height_tile_revision.generation != 0U) {
            append_if_missing(not_resident_keys, tile.key);
        }
    }
    sort_keys(not_resident_keys);
    return not_resident_keys;
}

bool GameClient::applyHeightTileRemoval(shared::ServerRemoveHeightTileMessage const& message)
{
    if (message.key != shared::normalizeHeightTileKey(message.key) || message.revision == 0U
        || message.token == 0U || m_height_tile_revision.generation == 0U) {
        return false;
    }
    bool const removed = m_height_tile_residency.evict(
        message.key,
        { .generation = m_height_tile_revision.generation, .revision = message.revision },
        message.token
    );
    if (removed) {
        removeResidentSurfaceMetadata({ .x = message.key.x, .y = message.key.y, .z = 0 });
    }
    return removed;
}

} // namespace client
