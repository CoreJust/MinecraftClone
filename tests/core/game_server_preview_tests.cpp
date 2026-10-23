#include <server/GameServer.hpp>
#include <server/WorldPreparation.hpp>
#include <shared/net/Message.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/HeightTileInterest.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldCoverage.hpp>
#include <generation/Generation.hpp>
#include <core/common/Defer.hpp>
#include <core/IO/Log.hpp>
#include <core/net/Address.hpp>
#include <core/net/Client.hpp>
#include <core/net/Event.hpp>
#include <core/net/SendMode.hpp>

#include <enet/enet.h>
#include <enet/protocol.h>
#if !defined(_WIN32)
#include <enet/unix.h>
#else
#include <enet/win32.h>
#endif
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace {

std::shared_ptr<shared::TerrainWorld const> preparedTerrainWorld()
{
    static std::shared_ptr<shared::TerrainWorld const> const terrain_world = [] {
        generation::GenerationConfiguration configuration;
        configuration.seed = shared::World::canonicalConfiguration().seed;
        configuration.epochs = 1U;
        configuration.erosion_steps = 0U;
        configuration.initial_climate_iterations = 1U;
        configuration.climate_iterations = 1U;
        configuration.currents = false;
        configuration.coastal_elaboration = false;
        std::shared_ptr<generation::GlobalAtlas const> const atlas = generation::prepareAtlas(configuration);
        return std::make_shared<shared::TerrainWorld>(
            atlas,
            shared::World::canonicalConfiguration().algorithm_version
        );
    }();
    return terrain_world;
}

class PreviewClient final : public core::Client {
public:
    static constexpr uint32_t MAX_TRACKED_INPUTS = 512U;

    explicit PreviewClient(
        bool const acknowledges_deliveries = true,
        bool const starts_deliveries = true,
        char const tracked_character = '@',
        bool const retains_messages = true
    )
        : core::Client{2}
        , m_acknowledges_deliveries{acknowledges_deliveries}
        , m_starts_deliveries{starts_deliveries}
        , m_tracked_character{tracked_character}
        , m_retains_messages{retains_messages}
    { }

    std::vector<shared::Message> messages;
    std::vector<shared::HeightTileKey> received_height_tile_keys;
    uint32_t received_height_tiles = 0U;
    uint32_t received_delivery_batches = 0U;
    uint32_t received_height_tile_descriptors = 0U;
    uint32_t descriptor_max_height_tiles = 0U;
    uint32_t descriptor_max_height_tile_bytes = 0U;
    uint32_t received_removals = 0U;
    uint32_t invalid_height_tiles = 0U;
    uint32_t sent_delivery_credit_messages = 0U;
    uint32_t last_acknowledged_input = 0U;
    int32_t last_player_x = 0;
    std::optional<shared::ServerPlayerPositionMessage> last_player_position;
    std::optional<bool> join_accepted;
    uint64_t acknowledgement_latency_samples = 0U;
    uint64_t acknowledgement_latency_total_ns = 0U;
    uint64_t acknowledgement_latency_max_ns = 0U;

    void trackInputSent(uint32_t const sequence)
    {
        if (sequence < MAX_TRACKED_INPUTS) {
            m_input_sent_at[sequence] = std::chrono::steady_clock::now();
        }
    }

    void setAcknowledgesDeliveries(bool const acknowledges_deliveries) noexcept
    {
        m_acknowledges_deliveries = acknowledges_deliveries;
    }

    void reportFirstDeliveryTileAsEvicted() noexcept
    {
        m_report_first_delivery_tile_as_evicted = true;
    }

    bool acknowledgeDelivery(
        uint64_t const delivery_token,
        std::vector<shared::HeightTileKey> not_resident_keys = {}
    )
    {
        return send(shared::encodeMessage(shared::ClientHeightTileCreditMessage{
            .world_revision = 1U,
            .delivery_token = delivery_token,
            .credits = 1U,
            .not_resident_keys = std::move(not_resident_keys),
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable});
    }

private:
    void onDisconnected(core::DisconnectEvent const) override { }

    void recordHeightTile(shared::ServerHeightTileMessage const& tile)
    {
        ++received_height_tiles;
        if (!m_retains_messages) {
            received_height_tile_keys.push_back(tile.key);
        }
        if (tile.heights.size() != shared::HEIGHT_TILE_SAMPLE_COUNT
            || tile.token == 0U
            || tile.revision != 1U) {
            ++invalid_height_tiles;
        }
    }

    void onReceived(core::ReceiveEvent event) override
    {
        auto const message = shared::decodeMessage(event.data);
        if (!message) {
            return;
        }
        if (m_retains_messages) {
            messages.push_back(*message);
        }
        if (auto const* const join_response = std::get_if<shared::JoinResponseMessage>(&*message)) {
            join_accepted = join_response->accepted;
        } else if (auto const* const tile_message = std::get_if<shared::ServerHeightTileMessage>(&*message)) {
            recordHeightTile(*tile_message);
        } else if (auto const* const batch = std::get_if<shared::ServerHeightTileBatchMessage>(&*message)) {
            ++received_delivery_batches;
            received_removals += static_cast<uint32_t>(batch->removals.size());
            for (shared::ServerHeightTileMessage const& batch_tile : batch->tiles) {
                recordHeightTile(batch_tile);
            }
        } else if (std::holds_alternative<shared::ServerRemoveHeightTileMessage>(*message)) {
            ++received_removals;
        } else if (std::holds_alternative<shared::ServerHeightTileDescriptorMessage>(*message)) {
            ++received_height_tile_descriptors;
            auto const& descriptor = std::get<shared::ServerHeightTileDescriptorMessage>(*message);
            descriptor_max_height_tiles = descriptor.max_height_tiles;
            descriptor_max_height_tile_bytes = descriptor.max_height_tile_bytes;
        }
        if (auto const* const descriptor = std::get_if<shared::ServerHeightTileDescriptorMessage>(&*message);
            descriptor != nullptr && m_starts_deliveries) {
            if (send(shared::encodeMessage(shared::ClientHeightTileCreditMessage{
                .world_revision = descriptor->world_revision,
                .delivery_token = 0U,
                .credits = shared::HEIGHT_TILE_DELIVERY_WINDOW,
                .not_resident_keys = {},
            }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable})) {
                ++sent_delivery_credit_messages;
            }
        } else if (auto const* const batch = std::get_if<shared::ServerHeightTileBatchMessage>(&*message);
            batch != nullptr && m_acknowledges_deliveries) {
            std::vector<shared::HeightTileKey> not_resident_keys;
            if (m_report_first_delivery_tile_as_evicted && !batch->tiles.empty()) {
                not_resident_keys.push_back(batch->tiles.front().key);
                m_report_first_delivery_tile_as_evicted = false;
            }
            if (send(shared::encodeMessage(shared::ClientHeightTileCreditMessage{
                .world_revision = 1U,
                .delivery_token = batch->delivery_token,
                .credits = 1U,
                .not_resident_keys = std::move(not_resident_keys),
            }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable})) {
                ++sent_delivery_credit_messages;
            }
        }
        if (auto const* position = std::get_if<shared::ServerPlayerPositionMessage>(&*message);
            position != nullptr && position->ch == m_tracked_character) {
            last_player_position = *position;
            uint32_t const previous_acknowledged_input = last_acknowledged_input;
            last_acknowledged_input = position->acknowledged_input_sequence;
            last_player_x = position->x;
            if (position->acknowledged_input_sequence > previous_acknowledged_input
                && position->acknowledged_input_sequence < MAX_TRACKED_INPUTS) {
                auto const sent_at = m_input_sent_at[position->acknowledged_input_sequence];
                if (sent_at.time_since_epoch().count() != 0) {
                    auto const latency = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - sent_at
                    );
                    uint64_t const latency_ns = static_cast<uint64_t>(latency.count());
                    ++acknowledgement_latency_samples;
                    acknowledgement_latency_total_ns += latency_ns;
                    acknowledgement_latency_max_ns = std::max(acknowledgement_latency_max_ns, latency_ns);
                }
            }
        }
    }

    bool m_acknowledges_deliveries;
    bool m_starts_deliveries;
    bool m_report_first_delivery_tile_as_evicted = false;
    char m_tracked_character;
    bool m_retains_messages;
    std::array<std::chrono::steady_clock::time_point, MAX_TRACKED_INPUTS> m_input_sent_at{};
};

std::array<shared::ChunkCoordinate, 6U> chunkNeighbors(shared::ChunkCoordinate const coordinate) noexcept
{
    static constexpr int32_t CHUNK_COUNT_XY = static_cast<int32_t>(
        shared::WorldExtent::WIDTH / shared::Chunk::SIDE_LENGTH
    );
    auto const wrap = [](int32_t const value) noexcept {
        return (value % CHUNK_COUNT_XY + CHUNK_COUNT_XY) % CHUNK_COUNT_XY;
    };
    return {
        shared::ChunkCoordinate{ .x = wrap(coordinate.x + 1), .y = coordinate.y, .z = coordinate.z },
        shared::ChunkCoordinate{ .x = wrap(coordinate.x - 1), .y = coordinate.y, .z = coordinate.z },
        shared::ChunkCoordinate{ .x = coordinate.x, .y = wrap(coordinate.y + 1), .z = coordinate.z },
        shared::ChunkCoordinate{ .x = coordinate.x, .y = wrap(coordinate.y - 1), .z = coordinate.z },
        shared::ChunkCoordinate{ .x = coordinate.x, .y = coordinate.y, .z = coordinate.z + 1 },
        shared::ChunkCoordinate{ .x = coordinate.x, .y = coordinate.y, .z = coordinate.z - 1 },
    };
}

struct ChunkCoordinateHash final {
    size_t operator()(shared::ChunkCoordinate const coordinate) const noexcept
    {
        uint64_t hash = 14'695'981'039'346'656'037ULL;
        for (int32_t const component : {coordinate.x, coordinate.y, coordinate.z}) {
            hash ^= static_cast<uint32_t>(component);
            hash *= 1'099'511'628'211ULL;
        }
        return static_cast<size_t>(hash);
    }
};

struct ChunkComponentInput final {
    std::span<shared::ChunkCoordinate const> candidates;
    std::span<shared::ChunkCoordinate const> roots;
};

std::vector<shared::ChunkCoordinate> rootedChunkComponent(ChunkComponentInput const input)
{
    std::unordered_set<shared::ChunkCoordinate, ChunkCoordinateHash> const candidate_set{
        input.candidates.begin(), input.candidates.end()
    };
    std::unordered_set<shared::ChunkCoordinate, ChunkCoordinateHash> connected;
    connected.reserve(candidate_set.size());
    std::vector<shared::ChunkCoordinate> frontier;
    frontier.reserve(candidate_set.size());
    for (shared::ChunkCoordinate const root : input.roots) {
        if (candidate_set.contains(root) && connected.insert(root).second) {
            frontier.push_back(root);
        }
    }
    for (size_t head = 0U; head < frontier.size(); ++head) {
        for (shared::ChunkCoordinate const neighbor : chunkNeighbors(frontier[head])) {
            if (candidate_set.contains(neighbor) && connected.insert(neighbor).second) {
                frontier.push_back(neighbor);
            }
        }
    }
    return frontier;
}

class PreparedWorldClient final : public core::Client {
public:
    struct HeldMaterializedDelivery {
        shared::ClientChunkDeliveryAckMessage acknowledgement;
        std::vector<shared::ChunkCoordinate> accepted_coordinates;
    };

    PreparedWorldClient()
        : core::Client{2}
    { }

    std::vector<shared::Message> messages;
    std::vector<shared::ChunkCoordinate> received_authoritative_columns;
    std::unordered_set<shared::ChunkCoordinate, ChunkCoordinateHash> received_authoritative_column_keys;
    std::vector<shared::ChunkCoordinate> required_authoritative_columns;
    bool acknowledge_authoritative_batches = false;
    bool acknowledge_preview_batches = true;
    std::vector<shared::ChunkCoordinate> received_chunks;
    std::vector<shared::ServerStartWorldAckMessage> start_acknowledgements;
    std::optional<bool> join_accepted;
    std::optional<shared::TerrainSessionIdentity> session;
    std::optional<shared::PlayerPosition> coverage_center;
    std::vector<shared::ChunkCoordinate> required_near_chunks;
    std::vector<shared::ChunkCoordinate> required_chunks;
    std::vector<shared::ChunkCoordinate> required_interest_chunks;
    std::vector<shared::ChunkCoordinate> connected_chunks;
    std::vector<shared::ChunkCoordinate> received_prefetched_chunks;
    std::vector<shared::ChunkCoordinate> rejected_disconnected_chunks;
    std::vector<shared::ChunkCoordinate> acknowledged_chunks;
    std::vector<HeldMaterializedDelivery> held_near_deliveries;
    bool withhold_final_near_delivery_acknowledgement = false;
    bool received_current_delivery_while_near_acknowledgements_pending = false;
    uint32_t near_chunks_received_at_first_current_delivery = 0U;
    uint32_t near_chunks_acknowledged_at_first_current_delivery = 0U;
    bool received_prefetch_with_current_backlog_batch = false;
    std::vector<shared::ServerHeightTileMessage> received_height_tiles;
    std::function<void(shared::ServerHeightTileMessage const&, bool, bool)> on_preview_tile;
    uint64_t coverage_epoch = 0U;
    uint64_t preview_world_revision = 0U;
    std::optional<shared::TerrainIdentity> preview_world_identity;
    bool final_atlas_complete = false;
    bool coverage_root_pending = true;
    bool acknowledge_materialized_batches = true;
    bool sent_initial_credit = false;
    bool sent_early_start = false;
    bool sent_ready_start = false;
    std::optional<std::chrono::steady_clock::time_point> first_near_ready_at;
    uint32_t acknowledged_chunks_at_first_near_ready = 0U;
    bool wants_previews = false;

    [[nodiscard]]
    bool requestWorldStart()
    {
        return sendStart();
    }

    [[nodiscard]]
    bool releaseHeldNearDeliveryAcknowledgements()
    {
        for (HeldMaterializedDelivery const& delivery : held_near_deliveries) {
            if (!send(shared::encodeMessage(delivery.acknowledgement),
                shared::GAME_CHANNEL,
                core::SendMode{core::SendMode::Reliable})) {
                return false;
            }
            for (shared::ChunkCoordinate const coordinate : delivery.accepted_coordinates) {
                if (std::ranges::find(
                    delivery.acknowledgement.not_resident_keys,
                    coordinate
                ) == delivery.acknowledgement.not_resident_keys.end()
                    && std::ranges::find(acknowledged_chunks, coordinate) == acknowledged_chunks.end()) {
                    acknowledged_chunks.push_back(coordinate);
                }
            }
        }
        held_near_deliveries.clear();
        return true;
    }

private:
    void onDisconnected(core::DisconnectEvent const) override { }

    [[nodiscard]]
    bool sendInitialCredit()
    {
        if (!session) {
            return false;
        }
        return send(shared::encodeMessage(shared::ClientChunkDeliveryAckMessage{
            .session = *session,
            .coverage_epoch = coverage_epoch,
            .delivery_token = 0U,
            .credits = shared::MATERIALIZED_CHUNK_DELIVERY_WINDOW,
            .not_resident_keys = {},
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable});
    }

    [[nodiscard]]
    bool sendStart()
    {
        if (!session) {
            return false;
        }
        return send(shared::encodeMessage(shared::ClientStartWorldMessage{
            .session = *session,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable});
    }

    [[nodiscard]]
    bool sendInitialPreviewCredit()
    {
        return send(shared::encodeMessage(shared::ClientHeightTileCreditMessage{
            .world_revision = preview_world_revision,
            .delivery_token = 0U,
            .credits = shared::HEIGHT_TILE_DELIVERY_WINDOW,
            .not_resident_keys = {},
        }), shared::HEIGHT_TILE_CHANNEL, core::SendMode{core::SendMode::Reliable});
    }

    [[nodiscard]]
    bool acknowledgePreviewBatch(shared::ServerHeightTileBatchMessage const& batch)
    {
        return send(shared::encodeMessage(shared::ClientHeightTileCreditMessage{
            .world_revision = preview_world_revision,
            .delivery_token = batch.delivery_token,
            .credits = 1U,
            .not_resident_keys = {},
        }), shared::HEIGHT_TILE_CHANNEL, core::SendMode{core::SendMode::Reliable});
    }

    [[nodiscard]]
    bool receivedEveryNearChunk() const
    {
        return std::ranges::all_of(required_near_chunks, [this](shared::ChunkCoordinate const coordinate) {
            return std::ranges::find(connected_chunks, coordinate) != connected_chunks.end();
        });
    }

    [[nodiscard]]
    bool isConnectedChunk(shared::ChunkCoordinate const coordinate) const
    {
        if (!coverage_center.has_value()) {
            return false;
        }
        if (coverage_root_pending) {
            return coordinate == shared::WorldBounds::chunkCoordinate({
                .x = static_cast<int64_t>(coverage_center->x),
                .y = static_cast<int64_t>(coverage_center->y),
                .z = static_cast<int64_t>(coverage_center->z),
            });
        }
        auto const neighbors = chunkNeighbors(coordinate);
        return std::ranges::any_of(neighbors, [this](shared::ChunkCoordinate const neighbor) {
            return std::ranges::find(connected_chunks, neighbor) != connected_chunks.end();
        });
    }

    void onReceived(core::ReceiveEvent event) override
    {
        auto const message = shared::decodeMessage(event.data);
        if (!message) {
            return;
        }
        messages.push_back(*message);
        if (auto const* const response = std::get_if<shared::JoinResponseMessage>(&*message)) {
            join_accepted = response->accepted;
        } else if (auto const* const preparation =
                std::get_if<shared::ServerWorldPreparationMessage>(&*message)) {
            session = preparation->session;
            final_atlas_complete = final_atlas_complete || preparation->final_atlas_complete;
        } else if (auto const* const coverage = std::get_if<shared::ServerChunkCoverageMessage>(&*message)) {
            session = coverage->session;
            bool const new_epoch = coverage_epoch != coverage->coverage_epoch;
            std::vector<shared::ChunkCoordinate> const next_near = shared::requiredChunkKeys(
                coverage->center,
                shared::NEAR_TERRAIN_RADIUS_BLOCKS
            );
            std::vector<shared::ChunkCoordinate> const next_interest =
                shared::materializedInterestChunkKeys(coverage->center);
            if (new_epoch) {
                connected_chunks.clear();
                acknowledged_chunks.clear();
                coverage_root_pending = true;
                sent_initial_credit = false;
            } else if (coverage_center.has_value()
                && (coverage_center->x != coverage->center.x
                    || coverage_center->y != coverage->center.y
                    || coverage_center->z != coverage->center.z
                    || coverage_center->x_subcell != coverage->center.x_subcell
                    || coverage_center->y_subcell != coverage->center.y_subcell
                    || coverage_center->z_subcell != coverage->center.z_subcell)) {
                std::unordered_set<shared::ChunkCoordinate, ChunkCoordinateHash> const old_near{
                    required_near_chunks.begin(), required_near_chunks.end()
                };
                std::unordered_set<shared::ChunkCoordinate, ChunkCoordinateHash> const new_near{
                    next_near.begin(), next_near.end()
                };
                std::unordered_set<shared::ChunkCoordinate, ChunkCoordinateHash> const new_interest{
                    next_interest.begin(), next_interest.end()
                };
                std::vector<shared::ChunkCoordinate> retained;
                retained.reserve(connected_chunks.size());
                std::vector<shared::ChunkCoordinate> seeds;
                seeds.reserve(connected_chunks.size());
                for (shared::ChunkCoordinate const coordinate : connected_chunks) {
                    if (!new_interest.contains(coordinate)) {
                        continue;
                    }
                    retained.push_back(coordinate);
                    if (old_near.contains(coordinate) && new_near.contains(coordinate)) {
                        seeds.push_back(coordinate);
                    }
                }
                connected_chunks = rootedChunkComponent({ .candidates = retained, .roots = seeds });
                std::vector<shared::ChunkCoordinate> retained_acknowledgements;
                retained_acknowledgements.reserve(acknowledged_chunks.size());
                for (shared::ChunkCoordinate const coordinate : acknowledged_chunks) {
                    if (new_interest.contains(coordinate)) {
                        retained_acknowledgements.push_back(coordinate);
                    }
                }
                acknowledged_chunks = rootedChunkComponent({
                    .candidates = retained_acknowledgements,
                    .roots = seeds,
                });
                coverage_root_pending = connected_chunks.empty();
            }
            coverage_epoch = coverage->coverage_epoch;
            coverage_center = coverage->center;
            required_authoritative_columns = shared::requiredTerrainColumns(coverage->center);
            required_near_chunks = next_near;
            required_chunks = shared::requiredChunkKeys(
                coverage->center,
                shared::NEAR_TERRAIN_RADIUS_BLOCKS
            );
            required_interest_chunks = shared::materializedInterestChunkKeys(coverage->center);
            if (!sent_initial_credit) {
                sent_initial_credit = sendInitialCredit();
                sent_early_start = sent_initial_credit && sendStart();
            }
        } else if (auto const* const descriptor =
                std::get_if<shared::ServerHeightTileDescriptorMessage>(&*message)) {
            preview_world_revision = descriptor->world_revision;
            preview_world_identity = descriptor->world_identity;
            if (wants_previews) {
                static_cast<void>(sendInitialPreviewCredit());
            }
        } else if (auto const* const batch =
                std::get_if<shared::ServerMaterializedChunkBatchMessage>(&*message)) {
            std::vector<shared::ChunkCoordinate> not_resident_keys;
            not_resident_keys.reserve(batch->chunks.size());
            std::vector<shared::ChunkCoordinate> accepted_coordinates;
            accepted_coordinates.reserve(batch->chunks.size());
            for (shared::MaterializedChunkPayload const& chunk : batch->chunks) {
                bool const in_interest = std::ranges::find(
                    required_interest_chunks,
                    chunk.coordinate
                ) != required_interest_chunks.end();
                if (!in_interest || !isConnectedChunk(chunk.coordinate)) {
                    not_resident_keys.push_back(chunk.coordinate);
                    rejected_disconnected_chunks.push_back(chunk.coordinate);
                    continue;
                }
                accepted_coordinates.push_back(chunk.coordinate);
                received_chunks.push_back(chunk.coordinate);
                connected_chunks.push_back(chunk.coordinate);
                if (coverage_root_pending) {
                    coverage_root_pending = false;
                }
                if (std::ranges::find(required_chunks, chunk.coordinate) == required_chunks.end()) {
                    received_prefetched_chunks.push_back(chunk.coordinate);
                }
            }
            bool const batch_contains_current = std::ranges::any_of(
                accepted_coordinates,
                [this](shared::ChunkCoordinate const coordinate) {
                    return std::ranges::find(required_chunks, coordinate) != required_chunks.end();
                }
            );
            bool const batch_contains_prefetch = std::ranges::any_of(
                accepted_coordinates,
                [this](shared::ChunkCoordinate const coordinate) {
                    return std::ranges::find(required_chunks, coordinate) == required_chunks.end();
                }
            );
            bool const current_backlog_remains = std::ranges::any_of(
                required_chunks,
                [this](shared::ChunkCoordinate const coordinate) {
                    return std::ranges::find(acknowledged_chunks, coordinate) == acknowledged_chunks.end();
                }
            );
            received_prefetch_with_current_backlog_batch = received_prefetch_with_current_backlog_batch
                || (batch_contains_current && batch_contains_prefetch && current_backlog_remains);
            bool const batch_contains_near = std::ranges::any_of(
                accepted_coordinates,
                [this](shared::ChunkCoordinate const coordinate) {
                    return std::ranges::find(required_near_chunks, coordinate) != required_near_chunks.end();
                }
            );
            bool const batch_contains_current_outside_near = std::ranges::any_of(
                accepted_coordinates,
                [this](shared::ChunkCoordinate const coordinate) {
                    bool const in_interest = std::ranges::find(required_interest_chunks, coordinate)
                        != required_interest_chunks.end();
                    bool const in_near = std::ranges::find(required_near_chunks, coordinate)
                        != required_near_chunks.end();
                    return in_interest && !in_near;
                }
            );
            bool const near_acknowledgements_complete = std::ranges::all_of(
                required_near_chunks,
                [this](shared::ChunkCoordinate const coordinate) {
                    return std::ranges::find(acknowledged_chunks, coordinate) != acknowledged_chunks.end();
                }
            );
            if (batch_contains_current_outside_near
                && !near_acknowledgements_complete
                && !received_current_delivery_while_near_acknowledgements_pending) {
                received_current_delivery_while_near_acknowledgements_pending = true;
                near_chunks_received_at_first_current_delivery = static_cast<uint32_t>(std::ranges::count_if(
                    required_near_chunks,
                    [this](shared::ChunkCoordinate const coordinate) {
                        return std::ranges::find(connected_chunks, coordinate) != connected_chunks.end();
                    }
                ));
                near_chunks_acknowledged_at_first_current_delivery = static_cast<uint32_t>(std::ranges::count_if(
                    required_near_chunks,
                    [this](shared::ChunkCoordinate const coordinate) {
                        return std::ranges::find(acknowledged_chunks, coordinate) != acknowledged_chunks.end();
                    }
                ));
            }
            shared::ClientChunkDeliveryAckMessage acknowledgement{
                .session = batch->session,
                .coverage_epoch = batch->coverage_epoch,
                .delivery_token = batch->delivery_token,
                .credits = 1U,
                .not_resident_keys = std::move(not_resident_keys),
            };
            bool acknowledged = false;
            if (acknowledge_materialized_batches && withhold_final_near_delivery_acknowledgement
                && batch_contains_near && receivedEveryNearChunk()) {
                withhold_final_near_delivery_acknowledgement = false;
                held_near_deliveries.push_back(HeldMaterializedDelivery{
                    .acknowledgement = std::move(acknowledgement),
                    .accepted_coordinates = std::move(accepted_coordinates),
                });
            } else if (acknowledge_materialized_batches) {
                acknowledged = send(shared::encodeMessage(acknowledgement), shared::GAME_CHANNEL,
                    core::SendMode{core::SendMode::Reliable});
                if (acknowledged) {
                    for (shared::ChunkCoordinate const coordinate : accepted_coordinates) {
                        if (std::ranges::find(acknowledged_chunks, coordinate) == acknowledged_chunks.end()) {
                            acknowledged_chunks.push_back(coordinate);
                        }
                    }
                }
            }
            if (acknowledged && !sent_ready_start && receivedEveryNearChunk()) {
                sent_ready_start = sendStart();
                if (sent_ready_start) {
                    first_near_ready_at = std::chrono::steady_clock::now();
                    acknowledged_chunks_at_first_near_ready = static_cast<uint32_t>(acknowledged_chunks.size());
                }
            }
        } else if (auto const* const authority =
                std::get_if<shared::ServerAuthoritativeTerrainBatchMessage>(&*message)) {
            for (auto const& tile : authority->tiles) {
                if (received_authoritative_column_keys.insert(tile.coordinate).second) {
                    received_authoritative_columns.push_back(tile.coordinate);
                }
            }
            if (acknowledge_authoritative_batches) {
                static_cast<void>(send(shared::encodeMessage(shared::ClientAuthoritativeTerrainAckMessage{
                    .session = authority->session, .coverage_epoch = authority->coverage_epoch,
                    .delivery_token = authority->delivery_token,
                    .not_resident_keys = {},
                }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
            }
        } else if (auto const* const tile = std::get_if<shared::ServerHeightTileMessage>(&*message)) {
            received_height_tiles.push_back(*tile);
            recordPreviewTile(*tile);
        } else if (auto const* const height_tile_batch =
                std::get_if<shared::ServerHeightTileBatchMessage>(&*message)) {
            for (shared::ServerHeightTileMessage const& batch_tile : height_tile_batch->tiles) {
                received_height_tiles.push_back(batch_tile);
                recordPreviewTile(batch_tile);
            }
            if (acknowledge_preview_batches) {
                static_cast<void>(acknowledgePreviewBatch(*height_tile_batch));
            }
        } else if (auto const* const acknowledgement =
                std::get_if<shared::ServerStartWorldAckMessage>(&*message)) {
            start_acknowledgements.push_back(*acknowledgement);
        }
    }

    void recordPreviewTile(shared::ServerHeightTileMessage const& tile)
    {
        bool const valid = preview_world_identity.has_value()
            && tile.revision == preview_world_revision
            && tile.token != 0U
            && tile.key.x == tile.surface.coordinate.x
            && tile.key.y == tile.surface.coordinate.y
            && tile.surface.coordinate.z == 0
            && tile.surface.identity == *preview_world_identity
            && shared::isValidSurfaceTile(tile.surface)
            && tile.heights.size() == shared::HEIGHT_TILE_SAMPLE_COUNT
            && std::ranges::equal(tile.heights, tile.surface.columns, [](uint16_t const height,
                shared::TerrainColumn const& column) {
                return height == column.highest_solid_z + 1U;
            });
        if (!on_preview_tile) {
            return;
        }
        on_preview_tile(
            tile,
            valid,
            std::ranges::any_of(required_near_chunks, [&tile](shared::ChunkCoordinate const coordinate) {
                return coordinate.x == tile.key.x && coordinate.y == tile.key.y;
            })
        );
    }
};

uint32_t heightTileCount(std::vector<shared::Message> const& messages)
{
    uint32_t count = 0U;
    for (shared::Message const& message : messages) {
        if (std::holds_alternative<shared::ServerHeightTileMessage>(message)) {
            ++count;
        } else if (auto const* const batch = std::get_if<shared::ServerHeightTileBatchMessage>(&message)) {
            count += static_cast<uint32_t>(batch->tiles.size());
        }
    }
    return count;
}

uint32_t removalCount(std::span<shared::Message const> const messages)
{
    uint32_t count = 0U;
    for (shared::Message const& message : messages) {
        if (std::holds_alternative<shared::ServerRemoveHeightTileMessage>(message)) {
            ++count;
        } else if (auto const* const batch = std::get_if<shared::ServerHeightTileBatchMessage>(&message)) {
            count += static_cast<uint32_t>(batch->removals.size());
        }
    }
    return count;
}

uint32_t deliveryBatchCount(std::vector<shared::Message> const& messages)
{
    return static_cast<uint32_t>(std::ranges::count_if(messages, [](shared::Message const& message) {
        return std::holds_alternative<shared::ServerHeightTileBatchMessage>(message);
    }));
}

std::vector<shared::HeightTileKey> heightTileKeys(std::vector<shared::Message> const& messages)
{
    std::vector<shared::HeightTileKey> keys;
    for (shared::Message const& message : messages) {
        if (auto const* const tile = std::get_if<shared::ServerHeightTileMessage>(&message)) {
            keys.push_back(tile->key);
        } else if (auto const* const batch = std::get_if<shared::ServerHeightTileBatchMessage>(&message)) {
            for (shared::ServerHeightTileMessage const& batch_tile : batch->tiles) {
                keys.push_back(batch_tile.key);
            }
        }
    }
    return keys;
}

std::vector<shared::HeightTileKey> heightTileKeysFrom(
    std::vector<shared::Message> const& messages,
    size_t const first_message
)
{
    std::vector<shared::Message> suffix;
    suffix.reserve(messages.size() - std::min(first_message, messages.size()));
    for (size_t index = std::min(first_message, messages.size()); index < messages.size(); ++index) {
        suffix.push_back(messages[index]);
    }
    return heightTileKeys(suffix);
}

std::vector<shared::ServerHeightTileBatchMessage> deliveryBatchesFrom(
    std::vector<shared::Message> const& messages,
    size_t const first_message
)
{
    std::vector<shared::ServerHeightTileBatchMessage> batches;
    for (size_t index = std::min(first_message, messages.size()); index < messages.size(); ++index) {
        if (auto const* const batch = std::get_if<shared::ServerHeightTileBatchMessage>(&messages[index])) {
            batches.push_back(*batch);
        }
    }
    return batches;
}

bool waitForStalledDeliveryWindow(PreviewClient& client, uint32_t const first_message = 0U)
{
    static constexpr std::chrono::seconds TIMEOUT{ 2 };
    static constexpr std::chrono::milliseconds STARTUP_DURATION{ 500 };
    static constexpr std::chrono::milliseconds QUIET_DURATION{ 100 };
    static constexpr std::chrono::milliseconds POLL_INTERVAL{ 1 };
    auto const started_at = std::chrono::steady_clock::now();
    auto last_delivery_at = started_at;
    uint32_t previous_count = 0U;
    while (std::chrono::steady_clock::now() - started_at < TIMEOUT) {
        client.poll(POLL_INTERVAL);
        uint32_t const count = static_cast<uint32_t>(std::ranges::count_if(
            std::span<shared::Message const>{ client.messages }.subspan(first_message),
            [](shared::Message const& message) {
                return std::holds_alternative<shared::ServerHeightTileBatchMessage>(message);
            }
        ));
        auto const now = std::chrono::steady_clock::now();
        if (count != previous_count) {
            previous_count = count;
            last_delivery_at = now;
        }
        if (count > 0U && now - started_at >= STARTUP_DURATION && now - last_delivery_at >= QUIET_DURATION) {
            return true;
        }
    }
    return false;
}

void expectBoundedDeliveryWindow(std::vector<shared::ServerHeightTileBatchMessage> const& batches)
{
    static constexpr uint32_t MAXIMUM_BATCHES = 8U;
    static constexpr uint32_t MAXIMUM_PACKET_BYTES = 10'118U;
    static constexpr uint32_t MAXIMUM_WINDOW_BYTES = 34'356U;
    EXPECT_GE(batches.size(), 1U);
    EXPECT_LE(batches.size(), MAXIMUM_BATCHES);
    uint64_t encoded_bytes = 0U;
    for (shared::ServerHeightTileBatchMessage const& batch : batches) {
        uint64_t const packet_bytes = shared::encodeMessage(batch).size();
        EXPECT_LE(packet_bytes, MAXIMUM_PACKET_BYTES);
        encoded_bytes += packet_bytes;
    }
    EXPECT_LE(encoded_bytes, MAXIMUM_WINDOW_BYTES);
}

bool meetsHeightTileRemovalProgress(
    uint32_t const removals,
    uint32_t const additions,
    uint64_t const encoded_bytes,
    uint64_t const available_bytes
) {
    static constexpr uint32_t REMOVAL_BYTES = 24U;
    return (removals > 0U && removals >= additions) || encoded_bytes + REMOVAL_BYTES > available_bytes;
}

uint64_t unacknowledgedHeightTileBytes(
    std::vector<shared::ServerHeightTileBatchMessage> const& batches,
    std::vector<uint64_t> const& acknowledged_tokens
) {
    uint64_t bytes = 0U;
    for (shared::ServerHeightTileBatchMessage const& batch : batches) {
        if (std::ranges::find(acknowledged_tokens, batch.delivery_token) == acknowledged_tokens.end()) {
            bytes += shared::encodeMessage(batch).size();
        }
    }
    return bytes;
}

bool meetsHeightTileRemovalQuota(
    uint32_t const removals,
    uint64_t const encoded_bytes,
    uint64_t const available_bytes
) {
    static constexpr uint32_t MINIMUM_REMOVALS = shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY / 2U;
    static constexpr uint32_t REMOVAL_BYTES = 24U;
    return removals >= MINIMUM_REMOVALS || encoded_bytes + REMOVAL_BYTES > available_bytes;
}

void expectAcknowledgedDeliveryWindow(
    std::vector<shared::ServerHeightTileBatchMessage> batches,
    std::vector<uint64_t> const& acknowledged_tokens
)
{
    static constexpr uint32_t INITIAL_CREDITS = 8U;
    EXPECT_LE(batches.size(), INITIAL_CREDITS + acknowledged_tokens.size());
    std::erase_if(batches, [&](shared::ServerHeightTileBatchMessage const& batch) {
        return std::ranges::find(acknowledged_tokens, batch.delivery_token) != acknowledged_tokens.end();
    });
    expectBoundedDeliveryWindow(batches);
}

} // namespace

TEST(GameServerPreviewTest, CanonicalTerrainServesMatchingCollisionAndPreviewSurface)
{
    static constexpr std::chrono::seconds TIMEOUT{15};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr int32_t TARGET_X = 32'880;
    static constexpr int32_t TARGET_Y = 32'768;
    static constexpr uint32_t STABLE_GROUNDED_INPUTS = 5U;
    static constexpr shared::HeightTileKey TARGET_TILE{.x = 2'055, .y = 2'048};

    std::shared_ptr<shared::TerrainWorld const> const terrain_world = preparedTerrainWorld();
    auto const expected_surface_storage = std::unique_ptr<shared::SurfaceTile const>{
        new shared::SurfaceTile{terrain_world->sampleSurface({
            .x = TARGET_TILE.x,
            .y = TARGET_TILE.y,
            .z = 0,
        })}
    };
    shared::SurfaceTile const& expected_surface = *expected_surface_storage;
    uint16_t const expected_height = static_cast<uint16_t>(expected_surface.columns.front().highest_solid_z + 1U);
    auto const server_storage = std::unique_ptr<server::GameServer>{new server::GameServer{
        0,
        {{.character = '@', .x = TARGET_X, .y = TARGET_Y, .z = expected_height}},
        shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(),
        shared::HEIGHT_TILE_INTEREST_RADIUS,
        terrain_world,
    }};
    server::GameServer& server = *server_storage;
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested] { server.run(stop_requested); }};
    auto const client_storage = std::make_unique<PreviewClient>();
    PreviewClient& client = *client_storage;
    auto const encode_message = [](auto const& message) { return shared::encodeMessage(message); };
    bool const connected = client.connect(core::Address::localhost(server.port()), TIMEOUT);
    bool const joined = connected && client.send(encode_message(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable});
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    std::optional<shared::ServerPlayerPositionMessage> position;
    std::optional<uint16_t> preview_height;
    auto const preview_surface_storage = std::make_unique<std::optional<shared::SurfaceTile>>();
    std::optional<shared::SurfaceTile>& preview_surface = *preview_surface_storage;
    std::optional<shared::TerrainIdentity> descriptor_identity;
    uint32_t inspected_messages = 0U;
    uint32_t sequence = 1U;
    uint32_t stable_grounded_inputs = 0U;
    uint8_t last_cycle_source = 255U;
    bool saw_collision_enabled_flight = false;
    bool sent_all_inputs = true;
    while (joined && sent_all_inputs
        && (stable_grounded_inputs < STABLE_GROUNDED_INPUTS || !preview_height.has_value())
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
        while (inspected_messages < client.messages.size()) {
            shared::Message const& message = client.messages[inspected_messages++];
            if (auto const* const update = std::get_if<shared::ServerPlayerPositionMessage>(&message)) {
                position = *update;
                saw_collision_enabled_flight |= update->movement_capabilities.bits == 1U;
                if (update->movement_capabilities.bits == 0U && update->vertical_velocity_subcells == 0) {
                    if (stable_grounded_inputs < STABLE_GROUNDED_INPUTS) {
                        ++stable_grounded_inputs;
                    }
                } else {
                    stable_grounded_inputs = 0U;
                }
            }
            auto const inspect_tile = [&preview_height, &preview_surface](shared::ServerHeightTileMessage const& tile) {
                if (tile.key == TARGET_TILE) {
                    preview_height = tile.heights[0U];
                    preview_surface = tile.surface;
                }
            };
            if (auto const* const tile = std::get_if<shared::ServerHeightTileMessage>(&message)) {
                inspect_tile(*tile);
            } else if (auto const* const batch = std::get_if<shared::ServerHeightTileBatchMessage>(&message)) {
                for (shared::ServerHeightTileMessage const& batch_tile : batch->tiles) {
                    inspect_tile(batch_tile);
                }
            } else if (auto const* const descriptor = std::get_if<shared::ServerHeightTileDescriptorMessage>(&message)) {
                descriptor_identity = descriptor->world_identity;
            }
        }
        if (!position.has_value() || position->acknowledged_input_sequence + 1U != sequence) {
            continue;
        }
        uint8_t const capability_bits = position->movement_capabilities.bits;
        bool const cycle = capability_bits != 0U && last_cycle_source != capability_bits;
        if (cycle) {
            last_cycle_source = capability_bits;
        }
        sent_all_inputs = client.send(encode_message(shared::ClientInputMessage{
            .direction = {.x = 0U, .y = 0U, .cycle_movement_capabilities = cycle}, .sequence = sequence++,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable});
    }
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    ASSERT_TRUE(connected);
    ASSERT_TRUE(joined);
    ASSERT_TRUE(sent_all_inputs);
    ASSERT_TRUE(saw_collision_enabled_flight);
    ASSERT_TRUE(preview_height.has_value());
    ASSERT_TRUE(preview_surface.has_value());
    ASSERT_TRUE(descriptor_identity.has_value());
    EXPECT_EQ(*descriptor_identity, terrain_world->identity());
    EXPECT_EQ(preview_surface->identity, terrain_world->identity());
    EXPECT_EQ(preview_surface->coordinate, expected_surface.coordinate);
    EXPECT_EQ(preview_surface->columns.front().highest_solid_z, expected_surface.columns.front().highest_solid_z);
    EXPECT_EQ(preview_surface->columns.front().top_material, expected_surface.columns.front().top_material);
    EXPECT_EQ(*preview_height, expected_height);
    EXPECT_EQ(*preview_height, preview_surface->columns.front().highest_solid_z + 1U);
    ASSERT_TRUE(position.has_value());
    ASSERT_EQ(stable_grounded_inputs, STABLE_GROUNDED_INPUTS);
    EXPECT_EQ(position->movement_capabilities.bits, 0U);
    EXPECT_EQ(position->x, TARGET_X);
    EXPECT_EQ(position->y, TARGET_Y);
    EXPECT_EQ(position->z, *preview_height);
    EXPECT_EQ(position->z_subcell, 0U);
    EXPECT_EQ(position->vertical_velocity_subcells, 0);
}

TEST(GameServerPreviewTest, PreparedDefaultSpawnWaitsForAsynchronousTerrainMaterialization)
{
    static constexpr std::chrono::seconds TIMEOUT{20};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    shared::WorldConfiguration const world_configuration = shared::World::canonicalConfiguration();
    generation::GenerationConfiguration generation_configuration;
    generation_configuration.seed = world_configuration.seed;
    generation_configuration.epochs = 1U;
    generation_configuration.erosion_steps = 0U;
    generation_configuration.initial_climate_iterations = 1U;
    generation_configuration.climate_iterations = 1U;
    generation_configuration.currents = false;
    generation_configuration.coastal_elaboration = false;
    auto const preparation = std::make_shared<server::WorldPreparation>(
        generation_configuration,
        world_configuration.algorithm_version
    );

    auto const preparation_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (preparation->latest().state == server::WorldPreparationState::Preparing
        && std::chrono::steady_clock::now() < preparation_deadline) {
        std::this_thread::sleep_for(POLL_INTERVAL);
    }
    ASSERT_EQ(preparation->latest().state, server::WorldPreparationState::Ready);

    server::GameServer game_server{
        0U,
        {},
        shared::WorldMode::Flight,
        world_configuration,
        shared::HEIGHT_TILE_INTEREST_RADIUS,
        nullptr,
        preparation,
    };
    PreviewClient client{false, false};
    std::atomic_bool connection_complete{false};
    std::thread connection_pump{[&game_server, &connection_complete] {
        while (!connection_complete.load(std::memory_order_acquire)) {
            static_cast<void>(game_server.tick(POLL_INTERVAL));
        }
    }};
    bool const connected = client.connect(core::Address::localhost(game_server.port()), TIMEOUT);
    connection_complete.store(true, std::memory_order_release);
    connection_pump.join();
    ASSERT_TRUE(connected);
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@',
        .mode = shared::WorldMode::Flight,
        .configuration = world_configuration,
        .wants_previews = false,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));

    auto const join_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!client.join_accepted.has_value() && std::chrono::steady_clock::now() < join_deadline) {
        static_cast<void>(game_server.tick(POLL_INTERVAL));
        static_cast<void>(client.poll(POLL_INTERVAL));
    }
    ASSERT_TRUE(client.join_accepted.has_value());
    ASSERT_TRUE(*client.join_accepted);
    EXPECT_FALSE(client.last_player_position.has_value());

    auto const spawn_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!client.last_player_position.has_value() && std::chrono::steady_clock::now() < spawn_deadline) {
        static_cast<void>(game_server.tick(POLL_INTERVAL));
        static_cast<void>(client.poll(POLL_INTERVAL));
    }
    ASSERT_TRUE(client.last_player_position.has_value());
    std::shared_ptr<shared::TerrainWorld const> const terrain_world = preparation->readyWorld();
    ASSERT_NE(terrain_world, nullptr);
    shared::ServerPlayerPositionMessage const spawn = *client.last_player_position;
    shared::SurfaceTile const surface = terrain_world->sampleSurface({
        .x = spawn.x / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH),
        .y = spawn.y / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH),
        .z = 0,
    });
    uint32_t const local_x = static_cast<uint32_t>(spawn.x) % shared::Chunk::SIDE_LENGTH;
    uint32_t const local_y = static_cast<uint32_t>(spawn.y) % shared::Chunk::SIDE_LENGTH;
    shared::TerrainColumn const& column = surface.columns[
        local_y * shared::Chunk::SIDE_LENGTH + local_x
    ];
    EXPECT_EQ(spawn.z, static_cast<int32_t>(column.highest_solid_z) + 1);
    EXPECT_EQ(spawn.z_subcell, 0U);
    EXPECT_EQ(spawn.vertical_velocity_subcells, 0);
}

TEST(GameServerPreviewTest, RejectsPreviewJoinWithoutPreparedTerrainContext)
{
    static constexpr auto TIMEOUT = std::chrono::seconds{2};
    server::GameServer server{0, {}, shared::WorldMode::Flight};
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested] { server.run(stop_requested); }};
    defer {
        stop_requested.store(true, std::memory_order_relaxed);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };

    PreviewClient client;
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@',
        .mode = shared::WorldMode::Flight,
        .wants_previews = true,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!client.join_accepted.has_value() && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }

    ASSERT_TRUE(client.join_accepted.has_value());
    EXPECT_FALSE(*client.join_accepted);
    EXPECT_EQ(client.received_height_tile_descriptors, 0U);
    EXPECT_EQ(client.received_height_tiles, 0U);
}

TEST(GameServerPreviewTest, PreparedWorldStreamsConnectedNearChunksBeforeFarCoverage)
{
    static constexpr std::chrono::seconds TIMEOUT{45};
    static constexpr std::chrono::seconds MOVING_NEAR_READY_TIMEOUT{10};
    static constexpr std::chrono::seconds MOVING_FAR_PROGRESS_TIMEOUT{10};
    static constexpr uint64_t JOIN_EPOCH = 17U;
    static constexpr uint32_t MAX_CHUNK_CANDIDATES_PER_PUMP = 1'024U;
    static constexpr uint32_t MAX_MATERIALIZATION_CANDIDATES_PER_REFILL = 256U;
    static constexpr uint32_t MAX_WORLD_GENERATION_DISPATCH_PER_CALL = 48U;
    static constexpr uint32_t NEAR_RADIUS_BLOCKS = shared::NEAR_TERRAIN_RADIUS_BLOCKS;
    static constexpr uint32_t NORMAL_SPEED_MOVEMENT_INPUT_COUNT = 16U;
    static constexpr uint32_t SAME_EPOCH_MOVEMENT_INPUT_COUNT = NORMAL_SPEED_MOVEMENT_INPUT_COUNT + 1U;
    shared::WorldConfiguration const world_configuration = shared::World::canonicalConfiguration();
    generation::GenerationConfiguration generation_configuration;
    generation_configuration.seed = world_configuration.seed;
    generation_configuration.epochs = 1U;
    generation_configuration.erosion_steps = 0U;
    generation_configuration.initial_climate_iterations = 1U;
    generation_configuration.climate_iterations = 1U;
    generation_configuration.currents = false;
    generation_configuration.coastal_elaboration = false;
    auto const preparation_started = server::WorldPreparation::Clock::now();
    auto const preparation = std::make_shared<server::WorldPreparation>(
        generation_configuration,
        world_configuration.algorithm_version,
        preparation_started
    );
    std::atomic<uint64_t> materialization_enqueued_total{0U};
    std::atomic<uint32_t> materialization_outstanding{0U};
    std::atomic<uint64_t> materialization_max_enqueued{0U};
    std::atomic<uint32_t> materialization_max_outstanding{0U};
    std::atomic<uint64_t> max_candidates_per_delivery_call{0U};
    std::atomic_bool candidate_scan_budget_exceeded{false};
    std::atomic<uint64_t> forecast_materialization_candidates_examined{0U};
    std::atomic_bool materialization_candidate_scan_budget_exceeded{false};
    std::atomic_bool world_generation_dispatch_budget_exceeded{false};
    std::atomic<uint64_t> delivered_chunks_total{0U};
    std::atomic<uint32_t> acknowledged_required_chunks{0U};
    std::atomic<uint32_t> observed_required_full_chunks{0U};
    server::GameServer::BenchmarkHooks benchmark_hooks{
        .on_tick = {},
        .on_preview_buffered = {},
        .on_preview_metrics = [&materialization_enqueued_total, &materialization_outstanding,
            &materialization_max_enqueued, &materialization_max_outstanding,
            &max_candidates_per_delivery_call, &candidate_scan_budget_exceeded,
            &forecast_materialization_candidates_examined,
            &materialization_candidate_scan_budget_exceeded, &world_generation_dispatch_budget_exceeded,
            &delivered_chunks_total, &acknowledged_required_chunks, &observed_required_full_chunks](
            core::ClientId,
            server::GameServer::PreviewStreamMetrics const metrics
        ) {
            auto const& jobs = metrics.world_generation_jobs;
            uint32_t const outstanding = jobs.pending_jobs + jobs.executor_queued_jobs + jobs.running_jobs
                + jobs.completed_uncollected_jobs;
            materialization_enqueued_total.store(jobs.enqueued_total, std::memory_order_relaxed);
            materialization_outstanding.store(outstanding, std::memory_order_relaxed);
            if (jobs.enqueued_total > materialization_max_enqueued.load(std::memory_order_relaxed)) {
                materialization_max_enqueued.store(jobs.enqueued_total, std::memory_order_relaxed);
            }
            if (outstanding > materialization_max_outstanding.load(std::memory_order_relaxed)) {
                materialization_max_outstanding.store(outstanding, std::memory_order_relaxed);
            }
            delivered_chunks_total.fetch_add(
                metrics.chunk_delivery_chunks_sent,
                std::memory_order_relaxed
            );
            acknowledged_required_chunks.store(metrics.acknowledged_chunk_keys, std::memory_order_relaxed);
            observed_required_full_chunks.store(metrics.required_full_chunk_keys, std::memory_order_relaxed);
            forecast_materialization_candidates_examined.fetch_add(
                metrics.forecast_materialization_candidates_examined,
                std::memory_order_relaxed
            );
            if (metrics.queue_refill.calls > 0U
                && metrics.materialization_candidate_keys_examined
                    > metrics.queue_refill.calls * MAX_MATERIALIZATION_CANDIDATES_PER_REFILL) {
                materialization_candidate_scan_budget_exceeded.store(true, std::memory_order_relaxed);
            }
            if (metrics.worker_dispatch.calls > 0U
                && metrics.world_generation_jobs_dispatched
                    > metrics.worker_dispatch.calls * MAX_WORLD_GENERATION_DISPATCH_PER_CALL) {
                world_generation_dispatch_budget_exceeded.store(true, std::memory_order_relaxed);
            }
            if (metrics.delivery.calls > 0U) {
                uint64_t const candidate_budget = static_cast<uint64_t>(metrics.delivery.calls)
                    * MAX_CHUNK_CANDIDATES_PER_PUMP;
                if (metrics.chunk_delivery_candidate_keys_examined > candidate_budget) {
                    candidate_scan_budget_exceeded.store(true, std::memory_order_relaxed);
                }
                uint64_t const candidates_per_call = (
                    metrics.chunk_delivery_candidate_keys_examined + metrics.delivery.calls - 1U
                ) / metrics.delivery.calls;
                if (candidates_per_call > max_candidates_per_delivery_call.load(std::memory_order_relaxed)) {
                    max_candidates_per_delivery_call.store(
                        candidates_per_call,
                        std::memory_order_relaxed
                    );
                }
            }
        },
        .before_height_tile_completion = {},
    };
    server::GameServer game_server{
        0U,
        {},
        shared::WorldMode::Flight,
        world_configuration,
        shared::HEIGHT_TILE_INTEREST_RADIUS,
        nullptr,
        preparation,
    };
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&game_server, &stop_requested, &benchmark_hooks] {
        game_server.run(stop_requested, &benchmark_hooks);
    }};
    defer {
        stop_requested.store(true, std::memory_order_relaxed);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };

    PreparedWorldClient client;
    client.wants_previews = true;
    uint64_t materialization_enqueued_at_far_preview_overlap = 0U;
    uint32_t materialization_outstanding_at_far_preview_overlap = 0U;
    uint32_t valid_preview_tiles = 0U;
    uint32_t projected_full_preview_tiles = 0U;
    uint32_t projected_far_preview_tiles = 0U;
    uint64_t materialization_max_enqueued_at_projected_far = 0U;
    uint32_t materialization_max_outstanding_at_projected_far = 0U;
    client.on_preview_tile = [&client, &materialization_enqueued_total, &materialization_outstanding,
        &materialization_enqueued_at_far_preview_overlap, &materialization_outstanding_at_far_preview_overlap,
        &valid_preview_tiles, &projected_full_preview_tiles, &projected_far_preview_tiles,
        &materialization_max_enqueued_at_projected_far, &materialization_max_outstanding_at_projected_far](
            shared::ServerHeightTileMessage const& tile,
            bool const valid,
            bool const is_near
        ) {
        bool const in_full_sphere_projection = std::ranges::any_of(
            client.required_interest_chunks,
            [&tile](shared::ChunkCoordinate const coordinate) {
                return coordinate.x == tile.key.x && coordinate.y == tile.key.y;
            }
        );
        uint64_t const enqueued = materialization_enqueued_total.load(std::memory_order_relaxed);
        uint32_t const outstanding = materialization_outstanding.load(std::memory_order_relaxed);
        if (valid) {
            ++valid_preview_tiles;
        }
        if (valid && in_full_sphere_projection) {
            ++projected_full_preview_tiles;
        }
        if (valid && in_full_sphere_projection && !is_near) {
            ++projected_far_preview_tiles;
            materialization_max_enqueued_at_projected_far = std::max(
                materialization_max_enqueued_at_projected_far,
                enqueued
            );
            materialization_max_outstanding_at_projected_far = std::max(
                materialization_max_outstanding_at_projected_far,
                outstanding
            );
        }
        if (valid && in_full_sphere_projection && !is_near && outstanding > 0U
            && materialization_outstanding_at_far_preview_overlap == 0U) {
            materialization_enqueued_at_far_preview_overlap = enqueued;
            materialization_outstanding_at_far_preview_overlap = outstanding;
        }
    };
    bool const connected = client.connect(core::Address::localhost(game_server.port()), TIMEOUT);
    bool const joined = connected && client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@',
        .mode = shared::WorldMode::Flight,
        .configuration = world_configuration,
        .wants_previews = true,
        .join_epoch = JOIN_EPOCH,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable});
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    auto has_start_result = [&client](bool const accepted) {
        return std::ranges::any_of(client.start_acknowledgements, [accepted](
            shared::ServerStartWorldAckMessage const& acknowledgement
        ) {
            return acknowledgement.accepted == accepted;
        });
    };
    auto const has_far_chunk = [&client] {
        return std::ranges::any_of(client.received_chunks, [&client](
            shared::ChunkCoordinate const coordinate
        ) {
            return std::ranges::find(client.required_near_chunks, coordinate)
                == client.required_near_chunks.end();
        });
    };
    while (std::chrono::steady_clock::now() < deadline
        && (!client.final_atlas_complete || !has_start_result(true) || !has_far_chunk()
            || materialization_outstanding_at_far_preview_overlap == 0U)) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }

    EXPECT_TRUE(connected);
    ASSERT_TRUE(joined);
    ASSERT_TRUE(client.join_accepted.has_value());
    ASSERT_TRUE(*client.join_accepted);
    ASSERT_TRUE(client.final_atlas_complete);
    ASSERT_TRUE(client.session.has_value());
    EXPECT_EQ(client.session->join_epoch, JOIN_EPOCH);
    EXPECT_EQ(client.session->terrain.seed, world_configuration.seed);
    EXPECT_EQ(client.coverage_epoch, 1U);
    EXPECT_FALSE(client.required_near_chunks.empty());
    EXPECT_TRUE(client.sent_initial_credit);
    EXPECT_TRUE(client.sent_early_start);
    EXPECT_TRUE(client.sent_ready_start);
    EXPECT_FALSE(client.received_height_tiles.empty());
    EXPECT_GT(projected_far_preview_tiles, 0U)
        << "valid preview tiles=" << valid_preview_tiles
        << ", projected into 80-block interest XY set=" << projected_full_preview_tiles
        << ", projected outside 48-block XY set=" << projected_far_preview_tiles
        << ", maximum world-generation jobs enqueued at projected-far preview="
        << materialization_max_enqueued_at_projected_far
        << ", maximum outstanding at projected-far preview="
        << materialization_max_outstanding_at_projected_far
        << ", maximum world-generation jobs enqueued overall="
        << materialization_max_enqueued.load(std::memory_order_relaxed)
        << ", maximum outstanding overall="
        << materialization_max_outstanding.load(std::memory_order_relaxed);
    EXPECT_GT(materialization_max_enqueued.load(std::memory_order_relaxed), client.required_near_chunks.size())
        << "maximum world-generation enqueues="
        << materialization_max_enqueued.load(std::memory_order_relaxed)
        << ", required near chunks=" << client.required_near_chunks.size();
    EXPECT_GT(materialization_outstanding_at_far_preview_overlap, 0U)
        << "world-generation enqueued at overlapping projected-far preview="
        << materialization_enqueued_at_far_preview_overlap
        << ", projected-far preview count=" << projected_far_preview_tiles;
    EXPECT_FALSE(client.received_prefetch_with_current_backlog_batch)
        << "Prefetch must not share a delivery while actual-sphere acknowledgements remain outstanding.";
    EXPECT_TRUE(has_start_result(false));
    ASSERT_TRUE(has_start_result(true));
    ASSERT_FALSE(client.received_chunks.empty());

    size_t first_far_chunk_index = client.received_chunks.size();
    std::vector<shared::ChunkCoordinate> unique_near_before_far;
    for (size_t index = 0U; index < client.received_chunks.size(); ++index) {
        shared::ChunkCoordinate const coordinate = client.received_chunks[index];
        if (std::ranges::find(client.required_near_chunks, coordinate) == client.required_near_chunks.end()) {
            first_far_chunk_index = index;
            break;
        }
        if (std::ranges::find(unique_near_before_far, coordinate) == unique_near_before_far.end()) {
            unique_near_before_far.push_back(coordinate);
        }
    }
    ASSERT_LT(first_far_chunk_index, client.received_chunks.size());
    EXPECT_EQ(unique_near_before_far.size(), client.required_near_chunks.size());

    auto const first_far_batch = std::ranges::find_if(client.messages, [&client](shared::Message const& message) {
        auto const* const batch = std::get_if<shared::ServerMaterializedChunkBatchMessage>(&message);
        return batch != nullptr && std::ranges::any_of(batch->chunks, [&client](
            shared::MaterializedChunkPayload const& chunk
        ) {
            return std::ranges::find(client.required_near_chunks, chunk.coordinate)
                == client.required_near_chunks.end();
        });
    });
    ASSERT_NE(first_far_batch, client.messages.end());
    std::vector<shared::Message>::size_type const first_far_batch_index = static_cast<
        std::vector<shared::Message>::size_type
    >(std::distance(client.messages.begin(), first_far_batch));
    EXPECT_TRUE(client.rejected_disconnected_chunks.empty())
        << "The initial server stream sent a chunk that was outside interest or disconnected.";

    std::vector<shared::ChunkCoordinate> const required_chunks_before_movement = client.required_chunks;
    for (uint32_t sequence = 1U; sequence <= NORMAL_SPEED_MOVEMENT_INPUT_COUNT; ++sequence) {
        ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {
                .x = 127U,
                .y = 0U,
                .z = 0U,
                .accelerated = false,
                .speedup = 30U,
                .view_x = 127,
            },
            .sequence = sequence,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    }
    auto const movement_acknowledged = [&client] {
        return std::ranges::any_of(client.messages, [](shared::Message const& message) {
            auto const* const position = std::get_if<shared::ServerPlayerPositionMessage>(&message);
            return position != nullptr
                && position->acknowledged_input_sequence >= NORMAL_SPEED_MOVEMENT_INPUT_COUNT;
        });
    };
    auto const movement_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!movement_acknowledged() && std::chrono::steady_clock::now() < movement_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    ASSERT_TRUE(movement_acknowledged());

    auto const has_coverage_center_advanced = [&client] {
        std::optional<shared::PlayerPosition> first_coverage_center;
        for (shared::Message const& message : client.messages) {
            auto const* const coverage = std::get_if<shared::ServerChunkCoverageMessage>(&message);
            if (coverage == nullptr) {
                continue;
            }
            if (!first_coverage_center.has_value()) {
                first_coverage_center = coverage->center;
                continue;
            }
            if (coverage->center.x != first_coverage_center->x
                || coverage->center.y != first_coverage_center->y
                || coverage->center.z != first_coverage_center->z
                || coverage->center.x_subcell != first_coverage_center->x_subcell
                || coverage->center.y_subcell != first_coverage_center->y_subcell
                || coverage->center.z_subcell != first_coverage_center->z_subcell) {
                return true;
            }
        }
        return false;
    };
    auto const coverage_update_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!has_coverage_center_advanced() && std::chrono::steady_clock::now() < coverage_update_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    ASSERT_TRUE(has_coverage_center_advanced());
    ASSERT_EQ(client.coverage_epoch, 1U);
    EXPECT_NE(client.required_chunks, required_chunks_before_movement);

    auto const normal_coverage_matches_latest_position = [&client] {
        std::optional<shared::PlayerPosition> latest_position;
        std::optional<shared::PlayerPosition> latest_coverage_center;
        for (shared::Message const& message : client.messages) {
            if (auto const* const position = std::get_if<shared::ServerPlayerPositionMessage>(&message);
                position != nullptr
                && position->acknowledged_input_sequence >= NORMAL_SPEED_MOVEMENT_INPUT_COUNT) {
                latest_position = shared::PlayerPosition{
                    .x = position->x,
                    .y = position->y,
                    .z = position->z,
                    .x_subcell = position->x_subcell,
                    .y_subcell = position->y_subcell,
                    .z_subcell = position->z_subcell,
                };
            } else if (auto const* const coverage = std::get_if<shared::ServerChunkCoverageMessage>(&message)) {
                latest_coverage_center = coverage->center;
            }
        }
        return latest_position.has_value()
            && latest_coverage_center.has_value()
            && latest_position->x == latest_coverage_center->x
            && latest_position->y == latest_coverage_center->y
            && latest_position->z == latest_coverage_center->z
            && latest_position->x_subcell == latest_coverage_center->x_subcell
            && latest_position->y_subcell == latest_coverage_center->y_subcell
            && latest_position->z_subcell == latest_coverage_center->z_subcell;
    };
    auto const normal_coverage_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!normal_coverage_matches_latest_position()
        && std::chrono::steady_clock::now() < normal_coverage_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    ASSERT_TRUE(normal_coverage_matches_latest_position());

    std::optional<shared::ServerPlayerPositionMessage> latest_normal_position;
    for (shared::Message const& message : client.messages) {
        auto const* const position = std::get_if<shared::ServerPlayerPositionMessage>(&message);
        if (position != nullptr
            && position->acknowledged_input_sequence >= NORMAL_SPEED_MOVEMENT_INPUT_COUNT) {
            latest_normal_position = *position;
        }
    }
    ASSERT_TRUE(latest_normal_position.has_value());
    shared::PlayerPosition const normal_center{
        .x = latest_normal_position->x,
        .y = latest_normal_position->y,
        .z = latest_normal_position->z,
        .x_subcell = latest_normal_position->x_subcell,
        .y_subcell = latest_normal_position->y_subcell,
        .z_subcell = latest_normal_position->z_subcell,
    };
    ASSERT_TRUE(client.coverage_center.has_value());
    EXPECT_EQ(client.coverage_center->x, normal_center.x);
    EXPECT_EQ(client.coverage_center->y, normal_center.y);
    EXPECT_EQ(client.coverage_center->z, normal_center.z);

    shared::World forecast_world{shared::WorldMode::Flight, world_configuration};
    shared::PlayerId const forecast_player_id = 1U;
    forecast_world.spawnPlayer(forecast_player_id, '@', normal_center);
    ASSERT_TRUE(forecast_world.setPlayerMovementCapabilities(
        forecast_player_id,
        latest_normal_position->movement_capabilities
    ));
    shared::Direction const first_accelerated_input{
        .x = 127U,
        .y = 0U,
        .z = 0U,
        .accelerated = true,
        .speedup = 30U,
        .view_x = 127,
    };
    std::optional<shared::PlayerPosition> const first_accelerated_center =
        forecast_world.forecastFlightPlayerPosition(forecast_player_id, first_accelerated_input);
    ASSERT_TRUE(first_accelerated_center.has_value());
    std::vector<shared::ChunkCoordinate> const first_accelerated_keys = shared::requiredChunkKeys(
        *first_accelerated_center,
        shared::NEAR_TERRAIN_RADIUS_BLOCKS
    );
    std::unordered_set<shared::ChunkCoordinate, ChunkCoordinateHash> first_accelerated_lead_keys;
    for (shared::ChunkCoordinate const coordinate : first_accelerated_keys) {
        if (std::ranges::find(client.required_interest_chunks, coordinate) != client.required_interest_chunks.end()
            && std::ranges::find(client.required_chunks, coordinate) == client.required_chunks.end()) {
            first_accelerated_lead_keys.insert(coordinate);
        }
    }
    ASSERT_FALSE(first_accelerated_lead_keys.empty());
    auto const has_acknowledged_first_accelerated_lead = [&client, &first_accelerated_lead_keys] {
        return std::ranges::any_of(client.acknowledged_chunks, [&first_accelerated_lead_keys](
            shared::ChunkCoordinate const coordinate
        ) {
            return first_accelerated_lead_keys.contains(coordinate);
        });
    };
    auto const lead_acknowledged_deadline = std::chrono::steady_clock::now() + MOVING_FAR_PROGRESS_TIMEOUT;
    while (!has_acknowledged_first_accelerated_lead()
        && std::chrono::steady_clock::now() < lead_acknowledged_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    ASSERT_TRUE(has_acknowledged_first_accelerated_lead())
        << "The 30-speedup lead for the queued normal heading was not resident before the first accelerated step.";
    std::vector<shared::ChunkCoordinate> acknowledged_first_accelerated_lead;
    for (shared::ChunkCoordinate const coordinate : client.acknowledged_chunks) {
        if (first_accelerated_lead_keys.contains(coordinate)) {
            acknowledged_first_accelerated_lead.push_back(coordinate);
        }
    }
    ASSERT_FALSE(acknowledged_first_accelerated_lead.empty());
    std::vector<shared::ChunkCoordinate> const required_chunks_before_acceleration = client.required_chunks;
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = first_accelerated_input,
        .sequence = SAME_EPOCH_MOVEMENT_INPUT_COUNT,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    auto const accelerated_movement_acknowledged = [&client] {
        return std::ranges::any_of(client.messages, [](shared::Message const& message) {
            auto const* const position = std::get_if<shared::ServerPlayerPositionMessage>(&message);
            return position != nullptr
                && position->acknowledged_input_sequence >= SAME_EPOCH_MOVEMENT_INPUT_COUNT;
        });
    };
    auto const accelerated_movement_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!accelerated_movement_acknowledged()
        && std::chrono::steady_clock::now() < accelerated_movement_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    ASSERT_TRUE(accelerated_movement_acknowledged());
    auto const coverage_matches_accelerated_position = [&client, &normal_center] {
        auto const same_position = [](shared::PlayerPosition const left, shared::PlayerPosition const right) {
            return left.x == right.x
                && left.y == right.y
                && left.z == right.z
                && left.x_subcell == right.x_subcell
                && left.y_subcell == right.y_subcell
                && left.z_subcell == right.z_subcell;
        };
        std::optional<shared::PlayerPosition> latest_position;
        for (shared::Message const& message : client.messages) {
            auto const* const position = std::get_if<shared::ServerPlayerPositionMessage>(&message);
            if (position != nullptr
                && position->acknowledged_input_sequence >= SAME_EPOCH_MOVEMENT_INPUT_COUNT) {
                latest_position = shared::PlayerPosition{
                    .x = position->x,
                    .y = position->y,
                    .z = position->z,
                    .x_subcell = position->x_subcell,
                    .y_subcell = position->y_subcell,
                    .z_subcell = position->z_subcell,
                };
            }
        }
        return latest_position.has_value()
            && client.coverage_center.has_value()
            && !same_position(*client.coverage_center, normal_center)
            && same_position(*client.coverage_center, *latest_position);
    };
    auto const accelerated_coverage_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!coverage_matches_accelerated_position()
        && std::chrono::steady_clock::now() < accelerated_coverage_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    ASSERT_TRUE(coverage_matches_accelerated_position());
    EXPECT_NE(client.required_chunks, required_chunks_before_acceleration);
    EXPECT_TRUE(std::ranges::any_of(acknowledged_first_accelerated_lead, [&client](
        shared::ChunkCoordinate const coordinate
    ) {
        return std::ranges::find(client.required_chunks, coordinate) != client.required_chunks.end();
    })) << "At least one pre-acknowledged 30-speedup lead chunk should promote into current dense48 coverage.";
    EXPECT_GT(forecast_materialization_candidates_examined.load(std::memory_order_relaxed), 0U);

    auto const received_new_current_chunk = [&client, &required_chunks_before_acceleration] {
        return std::ranges::any_of(client.received_chunks, [&client, &required_chunks_before_acceleration](
            shared::ChunkCoordinate const coordinate
        ) {
            return std::ranges::find(client.required_chunks, coordinate) != client.required_chunks.end()
                && std::ranges::find(required_chunks_before_acceleration, coordinate)
                    == required_chunks_before_acceleration.end();
        });
    };
    auto const more_chunks_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!received_new_current_chunk()
        && std::chrono::steady_clock::now() < more_chunks_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    ASSERT_TRUE(received_new_current_chunk())
        << "After same-epoch movement, a newly required 48-block chunk must be accepted; "
        << "halo-only delivery cannot advance current readiness.";
    EXPECT_TRUE(client.rejected_disconnected_chunks.empty())
        << "Same-epoch coverage movement must preserve rooted, face-connected chunk delivery.";

    std::optional<shared::PlayerPosition> first_coverage_center;
    bool coverage_center_advanced = false;
    bool coverage_epoch_stayed_stable = true;
    for (shared::Message const& message : client.messages) {
        auto const* const coverage = std::get_if<shared::ServerChunkCoverageMessage>(&message);
        if (coverage == nullptr) {
            continue;
        }
        coverage_epoch_stayed_stable = coverage_epoch_stayed_stable && coverage->coverage_epoch == 1U;
        if (!first_coverage_center.has_value()) {
            first_coverage_center = coverage->center;
            continue;
        }
        coverage_center_advanced = coverage_center_advanced
            || coverage->center.x != first_coverage_center->x
            || coverage->center.y != first_coverage_center->y
            || coverage->center.z != first_coverage_center->z
            || coverage->center.x_subcell != first_coverage_center->x_subcell
            || coverage->center.y_subcell != first_coverage_center->y_subcell
            || coverage->center.z_subcell != first_coverage_center->z_subcell;
    }
    EXPECT_TRUE(coverage_center_advanced);
    EXPECT_TRUE(coverage_epoch_stayed_stable);

    auto const current_coverage_matches_latest_position = [&client] {
        std::optional<shared::PlayerPosition> latest_position;
        std::optional<shared::PlayerPosition> latest_coverage_center;
        for (shared::Message const& message : client.messages) {
            if (auto const* const position = std::get_if<shared::ServerPlayerPositionMessage>(&message);
                position != nullptr
                && position->acknowledged_input_sequence >= SAME_EPOCH_MOVEMENT_INPUT_COUNT) {
                latest_position = shared::PlayerPosition{
                    .x = position->x,
                    .y = position->y,
                    .z = position->z,
                    .x_subcell = position->x_subcell,
                    .y_subcell = position->y_subcell,
                    .z_subcell = position->z_subcell,
                };
            } else if (auto const* const coverage = std::get_if<shared::ServerChunkCoverageMessage>(&message)) {
                latest_coverage_center = coverage->center;
            }
        }
        return latest_position.has_value()
            && latest_coverage_center.has_value()
            && latest_position->x == latest_coverage_center->x
            && latest_position->y == latest_coverage_center->y
            && latest_position->z == latest_coverage_center->z
            && latest_position->x_subcell == latest_coverage_center->x_subcell
            && latest_position->y_subcell == latest_coverage_center->y_subcell
            && latest_position->z_subcell == latest_coverage_center->z_subcell;
    };
    auto const current_near_is_resident = [&client] {
        return std::ranges::all_of(client.required_near_chunks, [&client](
            shared::ChunkCoordinate const coordinate
        ) {
            return std::ranges::find(client.connected_chunks, coordinate) != client.connected_chunks.end();
        });
    };
    auto const moving_near_deadline = std::chrono::steady_clock::now() + MOVING_NEAR_READY_TIMEOUT;
    while ((!current_coverage_matches_latest_position() || !current_near_is_resident())
        && std::chrono::steady_clock::now() < moving_near_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    EXPECT_TRUE(current_coverage_matches_latest_position());
    EXPECT_TRUE(current_near_is_resident())
        << "The latest same-epoch 48-block sphere did not become resident after continuous movement.";
    std::optional<shared::PlayerPosition> initial_coverage_center;
    std::unordered_set<shared::ChunkCoordinate, ChunkCoordinateHash> received_before_update;
    bool near_available_after_far_progress = false;
    for (std::vector<shared::Message>::size_type index = 0U; index < client.messages.size(); ++index) {
        shared::Message const& message = client.messages[index];
        if (auto const* const batch = std::get_if<shared::ServerMaterializedChunkBatchMessage>(&message)) {
            for (shared::MaterializedChunkPayload const& chunk : batch->chunks) {
                received_before_update.insert(chunk.coordinate);
            }
            continue;
        }
        auto const* const coverage = std::get_if<shared::ServerChunkCoverageMessage>(&message);
        if (coverage == nullptr) {
            continue;
        }
        if (!initial_coverage_center.has_value()) {
            initial_coverage_center = coverage->center;
            continue;
        }
        bool const center_advanced = coverage->center.x != initial_coverage_center->x
            || coverage->center.y != initial_coverage_center->y
            || coverage->center.z != initial_coverage_center->z
            || coverage->center.x_subcell != initial_coverage_center->x_subcell
            || coverage->center.y_subcell != initial_coverage_center->y_subcell
            || coverage->center.z_subcell != initial_coverage_center->z_subcell;
        if (!center_advanced || coverage->coverage_epoch != 1U || index <= first_far_batch_index) {
            continue;
        }
        std::vector<shared::ChunkCoordinate> const near_keys = shared::requiredChunkKeys(
            coverage->center,
            NEAR_RADIUS_BLOCKS
        );
        std::unordered_set<shared::ChunkCoordinate, ChunkCoordinateHash> missing_near_keys;
        for (shared::ChunkCoordinate const coordinate : near_keys) {
            if (!received_before_update.contains(coordinate)) {
                missing_near_keys.insert(coordinate);
            }
        }
        if (missing_near_keys.empty()) {
            near_available_after_far_progress = true;
            break;
        }
        for (std::vector<shared::Message>::size_type following = index + 1U;
             following < client.messages.size(); ++following) {
            shared::Message const& following_message = client.messages[following];
            if (auto const* const next_coverage = std::get_if<shared::ServerChunkCoverageMessage>(
                    &following_message);
                next_coverage != nullptr) {
                if (next_coverage->coverage_epoch != coverage->coverage_epoch
                    || shared::requiredChunkKeys(next_coverage->center, NEAR_RADIUS_BLOCKS) != near_keys) {
                    break;
                }
                continue;
            }
            auto const* const batch = std::get_if<shared::ServerMaterializedChunkBatchMessage>(&following_message);
            if (batch != nullptr && batch->coverage_epoch == coverage->coverage_epoch
                && std::ranges::any_of(batch->chunks, [&missing_near_keys](
                    shared::MaterializedChunkPayload const& chunk
                ) {
                    return missing_near_keys.contains(chunk.coordinate);
                })) {
                near_available_after_far_progress = true;
                break;
            }
        }
        if (near_available_after_far_progress) {
            break;
        }
    }
    EXPECT_TRUE(near_available_after_far_progress)
        << "After prefetch begins, a same-epoch center update must retain prefetched actual keys "
        << "or deliver missing actual keys before the required set changes again.";
    auto const has_current_prefetched_chunk = [&client] {
        for (uint32_t index = 0U;
             index < static_cast<uint32_t>(client.received_chunks.size());
             ++index) {
            shared::ChunkCoordinate const coordinate = client.received_chunks[index];
            if (std::ranges::find(client.required_near_chunks, coordinate) == client.required_near_chunks.end()
                && std::ranges::find(client.required_interest_chunks, coordinate)
                    != client.required_interest_chunks.end()) {
                return true;
            }
        }
        return false;
    };
    auto const moving_far_deadline = std::chrono::steady_clock::now() + MOVING_FAR_PROGRESS_TIMEOUT;
    while (!has_current_prefetched_chunk() && std::chrono::steady_clock::now() < moving_far_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    EXPECT_TRUE(has_current_prefetched_chunk())
        << "The same-epoch 80-block interest must contain delivered prefetch outside the ready actual sphere.";
    EXPECT_FALSE(candidate_scan_budget_exceeded.load(std::memory_order_relaxed));
    EXPECT_GT(max_candidates_per_delivery_call.load(std::memory_order_relaxed), 0U);
    EXPECT_LE(max_candidates_per_delivery_call.load(std::memory_order_relaxed),
        MAX_CHUNK_CANDIDATES_PER_PUMP);
    EXPECT_FALSE(materialization_candidate_scan_budget_exceeded.load(std::memory_order_relaxed));
    EXPECT_FALSE(world_generation_dispatch_budget_exceeded.load(std::memory_order_relaxed));
    EXPECT_GT(delivered_chunks_total.load(std::memory_order_relaxed), 0U);

    client.acknowledge_materialized_batches = false;
    auto const expected_current_acknowledged = [&client] {
        std::unordered_set<shared::ChunkCoordinate, ChunkCoordinateHash> acknowledged{
            client.acknowledged_chunks.begin(), client.acknowledged_chunks.end()
        };
        return static_cast<uint32_t>(std::ranges::count_if(
            acknowledged,
            [&client](shared::ChunkCoordinate const coordinate) {
                return std::ranges::find(client.required_chunks, coordinate) != client.required_chunks.end();
            }
        ));
    };
    auto const metrics_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while ((acknowledged_required_chunks.load(std::memory_order_relaxed) != expected_current_acknowledged()
            || observed_required_full_chunks.load(std::memory_order_relaxed) != client.required_chunks.size())
        && std::chrono::steady_clock::now() < metrics_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    EXPECT_EQ(observed_required_full_chunks.load(std::memory_order_relaxed), client.required_chunks.size());
    EXPECT_EQ(acknowledged_required_chunks.load(std::memory_order_relaxed), expected_current_acknowledged())
        << "Server ACK metrics must count only ACKed coordinates in the current 48-block sphere.";
}

TEST(GameServerPreviewTest, ActualSphereAcknowledgementsGateStartAndPrefetch)
{
    static constexpr std::chrono::seconds TIMEOUT{20};
    static constexpr uint64_t JOIN_EPOCH = 29U;
    shared::WorldConfiguration const world_configuration = shared::World::canonicalConfiguration();
    generation::GenerationConfiguration generation_configuration;
    generation_configuration.seed = world_configuration.seed;
    generation_configuration.epochs = 1U;
    generation_configuration.erosion_steps = 0U;
    generation_configuration.initial_climate_iterations = 1U;
    generation_configuration.climate_iterations = 1U;
    generation_configuration.currents = false;
    generation_configuration.coastal_elaboration = false;
    auto const preparation_started = server::WorldPreparation::Clock::now();
    auto const preparation = std::make_shared<server::WorldPreparation>(
        generation_configuration,
        world_configuration.algorithm_version,
        preparation_started
    );
    server::GameServer game_server{
        0U,
        {},
        shared::WorldMode::Flight,
        world_configuration,
        shared::HEIGHT_TILE_INTEREST_RADIUS,
        nullptr,
        preparation,
    };
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&game_server, &stop_requested] {
        game_server.run(stop_requested);
    }};
    defer {
        stop_requested.store(true, std::memory_order_relaxed);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };

    PreparedWorldClient client;
    client.withhold_final_near_delivery_acknowledgement = true;
    ASSERT_TRUE(client.connect(core::Address::localhost(game_server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@',
        .mode = shared::WorldMode::Flight,
        .configuration = world_configuration,
        .wants_previews = false,
        .join_epoch = JOIN_EPOCH,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));

    auto const all_near_payloads_received = [&client] {
        return !client.required_near_chunks.empty()
            && std::ranges::all_of(client.required_near_chunks, [&client](
                shared::ChunkCoordinate const coordinate
            ) {
                return std::ranges::find(client.connected_chunks, coordinate) != client.connected_chunks.end();
            });
    };
    auto const near_delivery_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!all_near_payloads_received() && std::chrono::steady_clock::now() < near_delivery_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }

    ASSERT_TRUE(client.join_accepted.has_value());
    ASSERT_TRUE(*client.join_accepted);
    ASSERT_TRUE(client.session.has_value());
    ASSERT_TRUE(client.final_atlas_complete);
    ASSERT_FALSE(client.required_near_chunks.empty());
    ASSERT_FALSE(client.required_chunks.empty());
    ASSERT_TRUE(all_near_payloads_received())
        << "Near payloads did not all reach the client while one near ACK was withheld.";
    ASSERT_FALSE(client.held_near_deliveries.empty());
    auto const pipeline_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
    while (std::chrono::steady_clock::now() < pipeline_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    EXPECT_FALSE(client.received_current_delivery_while_near_acknowledgements_pending)
        << "Interest prefetch must not pipeline while actual-sphere ACKs remain withheld.";
    EXPECT_FALSE(client.held_near_deliveries.empty());
    EXPECT_TRUE(client.received_prefetched_chunks.empty())
        << "Prefetch must remain withheld while near readiness is not ACK-complete.";
    EXPECT_TRUE(client.rejected_disconnected_chunks.empty());

    uint32_t const start_acknowledgements_before_retry = static_cast<uint32_t>(std::ranges::count_if(
        client.start_acknowledgements,
        [](shared::ServerStartWorldAckMessage const& acknowledgement) {
            return !acknowledgement.accepted;
        }
    ));
    ASSERT_TRUE(client.requestWorldStart());
    auto const start_rejection_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::ranges::count_if(client.start_acknowledgements,
        [](shared::ServerStartWorldAckMessage const& acknowledgement) {
            return !acknowledgement.accepted;
        }) == start_acknowledgements_before_retry
        && std::chrono::steady_clock::now() < start_rejection_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    EXPECT_FALSE(std::ranges::any_of(client.start_acknowledgements,
        [](shared::ServerStartWorldAckMessage const& acknowledgement) {
            return acknowledgement.accepted;
        })) << "Near payload receipt alone must not satisfy the server's readiness gate.";

    ASSERT_TRUE(client.releaseHeldNearDeliveryAcknowledgements());
    ASSERT_TRUE(client.requestWorldStart());
    auto const ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!std::ranges::any_of(client.start_acknowledgements,
        [](shared::ServerStartWorldAckMessage const& acknowledgement) {
            return acknowledgement.accepted;
        }) && std::chrono::steady_clock::now() < ready_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    EXPECT_TRUE(std::ranges::any_of(client.start_acknowledgements,
        [](shared::ServerStartWorldAckMessage const& acknowledgement) {
            return acknowledgement.accepted;
        }));
    auto const prefetch_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.received_prefetched_chunks.empty() && std::chrono::steady_clock::now() < prefetch_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    EXPECT_FALSE(client.received_prefetched_chunks.empty())
        << "Acknowledging the actual sphere must resume the bounded interest prefetch.";
}

TEST(GameServerPreviewTest, PreparedWorldMaterializationAdmissionIsFairAcrossPlayers)
{
    static constexpr std::chrono::seconds TIMEOUT{30};
    static constexpr size_t PLAYER_COUNT = 4U;
    static constexpr std::chrono::seconds MAX_NEAR_READY_SPREAD{12};
    static constexpr std::array<char, PLAYER_COUNT> CHARACTERS{'@', '#', '$', '%'};
    shared::WorldConfiguration const world_configuration = shared::World::canonicalConfiguration();
    std::shared_ptr<shared::TerrainWorld const> const terrain_world = preparedTerrainWorld();
    std::vector<server::GameServer::SpawnPoint> spawn_points;
    spawn_points.reserve(PLAYER_COUNT);
    for (size_t index = 0U; index < PLAYER_COUNT; ++index) {
        int32_t const x = shared::World::FLIGHT_SPAWN.x + 1 + static_cast<int32_t>(index) * 256;
        int32_t const y = shared::World::FLIGHT_SPAWN.y + 1;
        shared::SurfaceTile const surface = terrain_world->sampleSurface({
            .x = x / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH),
            .y = y / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH),
            .z = 0,
        });
        uint32_t const local_x = static_cast<uint32_t>(x) % shared::Chunk::SIDE_LENGTH;
        uint32_t const local_y = static_cast<uint32_t>(y) % shared::Chunk::SIDE_LENGTH;
        shared::TerrainColumn const& column = surface.columns[
            local_y * shared::Chunk::SIDE_LENGTH + local_x
        ];
        spawn_points.push_back({
            .character = CHARACTERS[index],
            .x = x,
            .y = y,
            .z = static_cast<int32_t>(column.highest_solid_z) + 1,
        });
    }
    generation::GenerationConfiguration generation_configuration;
    generation_configuration.seed = world_configuration.seed;
    generation_configuration.epochs = 1U;
    generation_configuration.erosion_steps = 0U;
    generation_configuration.initial_climate_iterations = 1U;
    generation_configuration.climate_iterations = 1U;
    generation_configuration.currents = false;
    generation_configuration.coastal_elaboration = false;
    auto const preparation = std::make_shared<server::WorldPreparation>(
        generation_configuration,
        world_configuration.algorithm_version
    );

    server::GameServer game_server{
        0U,
        std::move(spawn_points),
        shared::WorldMode::Flight,
        world_configuration,
        shared::HEIGHT_TILE_INTEREST_RADIUS,
        nullptr,
        preparation,
    };
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&game_server, &stop_requested] {
        game_server.run(stop_requested);
    }};
    defer {
        stop_requested.store(true, std::memory_order_relaxed);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };

    std::array<std::unique_ptr<PreparedWorldClient>, PLAYER_COUNT> clients;
    for (size_t index = 0U; index < PLAYER_COUNT; ++index) {
        clients[index] = std::make_unique<PreparedWorldClient>();
        ASSERT_TRUE(clients[index]->connect(core::Address::localhost(game_server.port()), TIMEOUT));
    }
    for (size_t index = 0U; index < PLAYER_COUNT; ++index) {
        ASSERT_TRUE(clients[index]->send(shared::encodeMessage(shared::JoinRequestMessage{
            .ch = CHARACTERS[index],
            .mode = shared::WorldMode::Flight,
            .configuration = world_configuration,
            .wants_previews = false,
            .join_epoch = static_cast<uint64_t>(index + 1U),
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    }

    auto const all_players_near_ready = [&clients] {
        return std::ranges::all_of(clients, [](auto const& client) {
            bool const accepted_start = std::ranges::any_of(
                client->start_acknowledgements,
                [](shared::ServerStartWorldAckMessage const& acknowledgement) {
                    return acknowledgement.accepted;
                }
            );
            return client->join_accepted == std::optional<bool>{true}
                && client->first_near_ready_at.has_value()
                && accepted_start;
        });
    };
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!all_players_near_ready() && std::chrono::steady_clock::now() < deadline) {
        for (auto const& client : clients) {
            static_cast<void>(client->poll(std::chrono::milliseconds{1}));
        }
    }
    EXPECT_TRUE(all_players_near_ready()) << "Each accepted player must reach an ACKed connected 48-block sphere.";
    for (size_t index = 0U; index < PLAYER_COUNT; ++index) {
        ASSERT_TRUE(clients[index]->join_accepted.has_value());
        if (!clients[index]->join_accepted) {
            return;
        }
        ASSERT_FALSE(clients[index]->required_near_chunks.empty());
        ASSERT_FALSE(clients[index]->required_chunks.empty());
        EXPECT_TRUE(clients[index]->join_accepted.value_or(false));
        EXPECT_TRUE(clients[index]->sent_ready_start);
        EXPECT_TRUE(clients[index]->rejected_disconnected_chunks.empty());
        EXPECT_EQ(clients[index]->acknowledged_chunks_at_first_near_ready,
            static_cast<uint32_t>(clients[index]->required_chunks.size()))
            << "Starting requires acknowledgements for every dense48 chunk.";
        EXPECT_LT(clients[index]->acknowledged_chunks_at_first_near_ready,
            static_cast<uint32_t>(clients[index]->required_interest_chunks.size()))
            << "Dense48 readiness must not wait for the optional80 interest to finish.";
    }
    if (all_players_near_ready()) {
        if (!clients.front()->first_near_ready_at) {
            FAIL() << "Ready players must have a recorded readiness time.";
            return;
        }
        auto earliest = clients.front()->first_near_ready_at.value_or(std::chrono::steady_clock::time_point{});
        auto latest = earliest;
        for (auto const& client : clients) {
            if (!client->first_near_ready_at) {
                FAIL() << "Ready players must have a recorded readiness time.";
                return;
            }
        earliest = std::min(
            earliest,
            client->first_near_ready_at.value_or(std::chrono::steady_clock::time_point{})
        );
            latest = std::max(latest, *client->first_near_ready_at);
        }
        EXPECT_LE(latest - earliest, MAX_NEAR_READY_SPREAD)
            << "Actual-sphere readiness must remain fair across all four accepted players.";
    }
}

TEST(GameServerPreviewTest, MaximumHeightTileFitsOneDeliveryBatch)
{
    static constexpr uint32_t BATCH_HEADER_BYTES = 13U;
    static constexpr uint32_t MAX_BATCH_BYTES = 10'118U;
    static constexpr uint32_t MAX_INFLIGHT_BYTES = 34'356U;
    shared::ServerHeightTileMessage tile;
    tile.surface.identity.configuration_identity.assign(
        shared::TERRAIN_CONFIGURATION_IDENTITY_MAX_BYTES,
        'x'
    );
    tile.heights.fill(0U);
    tile.heights.back() = std::numeric_limits<uint16_t>::max();

    shared::ServerHeightTileBatchMessage batch{
        .delivery_token = 1U,
        .tiles = {tile},
        .removals = {},
    };
    std::vector<uint8_t> const encoded_batch = shared::encodeMessage(batch);
    uint32_t const accounted_batch_bytes = BATCH_HEADER_BYTES + shared::encodedHeightTileSize(tile);

    EXPECT_EQ(accounted_batch_bytes, MAX_BATCH_BYTES);
    EXPECT_EQ(encoded_batch.size(), MAX_BATCH_BYTES);
    EXPECT_LE(3U * MAX_BATCH_BYTES, MAX_INFLIGHT_BYTES);
    EXPECT_GT(4U * MAX_BATCH_BYTES, MAX_INFLIGHT_BYTES);
}

TEST(GameServerPreviewTest, PreviewBatchAdmissionRefillsWithinInflightByteReserve)
{
    static constexpr std::chrono::seconds TIMEOUT{10};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr uint32_t MAX_INFLIGHT_BYTES = 34'356U;
    std::atomic_uint32_t ready_tiles{0U};
    std::atomic_uint32_t inflight_deliveries{0U};
    server::GameServer::BenchmarkHooks const hooks{
        .on_tick = {},
        .on_preview_buffered = {},
        .on_preview_metrics = [&ready_tiles, &inflight_deliveries](
            core::ClientId,
            server::GameServer::PreviewStreamMetrics const metrics
        ) {
            ready_tiles.store(metrics.ready_tiles, std::memory_order_relaxed);
            inflight_deliveries.store(metrics.inflight_deliveries, std::memory_order_relaxed);
        },
        .before_height_tile_completion = {},
    };
    shared::WorldConfiguration const world_configuration = shared::World::canonicalConfiguration();
    server::GameServer server{
        0U,
        {},
        shared::WorldMode::Flight,
        world_configuration,
        shared::HEIGHT_TILE_INTEREST_RADIUS,
        preparedTerrainWorld(),
    };
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested, &hooks] {
        server.run(stop_requested, &hooks);
    }};
    defer {
        stop_requested.store(true, std::memory_order_relaxed);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };

    PreviewClient client{false, true};
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@',
        .mode = shared::WorldMode::Flight,
        .configuration = world_configuration,
        .wants_previews = true,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));

    auto const initial_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while ((client.received_delivery_batches < 3U || ready_tiles.load(std::memory_order_relaxed) == 0U
            || inflight_deliveries.load(std::memory_order_relaxed) != 3U)
        && std::chrono::steady_clock::now() < initial_deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_EQ(client.received_delivery_batches, 3U);
    ASSERT_GT(ready_tiles.load(std::memory_order_relaxed), 0U);
    ASSERT_EQ(inflight_deliveries.load(std::memory_order_relaxed), 3U);

    std::vector<shared::ServerHeightTileBatchMessage> batches = deliveryBatchesFrom(client.messages, 0U);
    ASSERT_EQ(batches.size(), 3U);
    uint32_t inflight_bytes = 0U;
    for (shared::ServerHeightTileBatchMessage const& batch : batches) {
        ASSERT_EQ(batch.tiles.size(), 1U);
        ASSERT_TRUE(batch.removals.empty());
        inflight_bytes += static_cast<uint32_t>(shared::encodeMessage(batch).size());
    }
    EXPECT_LE(inflight_bytes, MAX_INFLIGHT_BYTES);

    auto const quiet_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{150};
    while (std::chrono::steady_clock::now() < quiet_deadline) {
        client.poll(POLL_INTERVAL);
    }
    EXPECT_EQ(client.received_delivery_batches, 3U);

    ASSERT_TRUE(client.acknowledgeDelivery(batches.front().delivery_token));
    auto const refill_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.received_delivery_batches < 4U && std::chrono::steady_clock::now() < refill_deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_GE(client.received_delivery_batches, 4U);
    batches = deliveryBatchesFrom(client.messages, 0U);
    ASSERT_EQ(batches.size(), 4U);
    inflight_bytes = 0U;
    for (size_t index = 1U; index < batches.size(); ++index) {
        inflight_bytes += static_cast<uint32_t>(shared::encodeMessage(batches[index]).size());
    }
    EXPECT_LE(inflight_bytes, MAX_INFLIGHT_BYTES);
}

TEST(GameServerPreviewTest, StreamsCompleteCurrentNearSquareBeforeFartherTiles)
{
    static constexpr std::chrono::seconds TIMEOUT{4};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr int32_t CURRENT_NEAR_RADIUS = 7;
    static constexpr uint32_t OBSERVED_TILE_COUNT = static_cast<uint32_t>(
        (CURRENT_NEAR_RADIUS * 2 + 1) * (CURRENT_NEAR_RADIUS * 2 + 1)
    );

    server::GameServer server{
        0,
        {},
        shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(),
        shared::HEIGHT_TILE_INTEREST_RADIUS,
        preparedTerrainWorld(),
    };
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested] {
        server.run(stop_requested);
    }};
    PreviewClient client;
    bool const connected = client.connect(core::Address::localhost(server.port()), TIMEOUT);
    bool const joined = connected && client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@',
        .mode = shared::WorldMode::Flight,
        .wants_previews = true,
    }), 0, core::SendMode{core::SendMode::Reliable});
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (joined && client.received_height_tiles < OBSERVED_TILE_COUNT
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    std::vector<shared::HeightTileKey> const keys = heightTileKeys(client.messages);
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    ASSERT_TRUE(connected);
    ASSERT_TRUE(joined);
    ASSERT_GE(keys.size(), OBSERVED_TILE_COUNT);
    std::vector<int32_t> rows;
    int32_t const center_x = shared::World::FLIGHT_SPAWN.x
        / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH);
    int32_t const center_y = shared::World::FLIGHT_SPAWN.y
        / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH);
    for (uint32_t index{0U}; index < OBSERVED_TILE_COUNT; ++index) {
        shared::HeightTileKey const key = keys[index];
        int32_t const distance = std::max(std::abs(key.x - center_x), std::abs(key.y - center_y));
        EXPECT_LE(distance, CURRENT_NEAR_RADIUS);
        if (std::ranges::find(rows, key.y) == rows.end()) {
            rows.push_back(key.y);
        }
    }
    EXPECT_GT(rows.size(), 1U);
}

TEST(GameServerPreviewTest, FlightPrewarmsTheNextNearFootprintBeforeRadialTiles)
{
    static constexpr std::chrono::seconds TIMEOUT{20};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr std::array<uint32_t, 2U> RENDER_DISTANCES{16U, 10U};
    static constexpr int32_t NEAR_RADIUS = 7;
    static constexpr uint32_t NEAR_TILE_COUNT = static_cast<uint32_t>(
        (NEAR_RADIUS * 2 + 1) * (NEAR_RADIUS * 2 + 1)
    );
    static constexpr shared::PlayerId FORECAST_PLAYER_ID = 1U;
    static constexpr shared::Direction MOVEMENT{
        .x = 127U,
        .y = 0U,
        .accelerated = true,
        .speedup = 200U,
    };

    for (uint32_t const render_distance : RENDER_DISTANCES) {
        SCOPED_TRACE(render_distance);
        uint32_t const initial_tile_count = shared::heightTileInterestCount(render_distance);
        std::atomic_uint32_t resident_tiles{0U};
        std::atomic_uint32_t inflight_deliveries{0U};
        server::GameServer::BenchmarkHooks const hooks{
            .on_tick = {},
            .on_preview_buffered = {},
            .on_preview_metrics = [&resident_tiles, &inflight_deliveries](
                core::ClientId,
                server::GameServer::PreviewStreamMetrics const metrics
            ) {
                resident_tiles.store(metrics.resident_tiles, std::memory_order_relaxed);
                inflight_deliveries.store(metrics.inflight_deliveries, std::memory_order_relaxed);
            },
            .before_height_tile_completion = {},
        };
        server::GameServer server{
            0,
            {},
            shared::WorldMode::Flight,
            shared::World::canonicalConfiguration(),
            render_distance,
            preparedTerrainWorld(),
        };
        std::atomic_bool stop_requested{false};
        std::thread server_thread{[&server, &stop_requested, &hooks] {
            server.run(stop_requested, &hooks);
        }};
        defer {
            stop_requested.store(true, std::memory_order_relaxed);
            if (server_thread.joinable()) {
                server_thread.join();
            }
        };

        PreviewClient client{true, true, '@', false};
        ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
        ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
            .ch = '@',
            .mode = shared::WorldMode::Flight,
            .wants_previews = true,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
        auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
        auto const poll_until = [&client](auto const& predicate, auto const stop_at) {
            while (!predicate() && std::chrono::steady_clock::now() < stop_at) {
                client.poll(POLL_INTERVAL);
            }
            return predicate();
        };
        ASSERT_TRUE(poll_until([&] {
            return client.received_height_tile_descriptors == 1U
                && resident_tiles.load(std::memory_order_relaxed) == initial_tile_count
                && inflight_deliveries.load(std::memory_order_relaxed) == 0U;
        }, deadline));
        uint32_t const initial_received_key_count = static_cast<uint32_t>(client.received_height_tile_keys.size());

        ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = MOVEMENT,
            .sequence = 1U,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
        ASSERT_TRUE(poll_until([&] {
            return client.last_acknowledged_input == 1U && client.last_player_position.has_value();
        }, deadline));
        shared::ServerPlayerPositionMessage const first_position = *client.last_player_position;
        shared::HeightTileKey const current_center = shared::normalizeHeightTileKey({
            .x = first_position.x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
            .y = first_position.y / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
        });

        shared::World forecast_world{shared::WorldMode::Flight};
        forecast_world.spawnPlayer(FORECAST_PLAYER_ID, '@', shared::PlayerPosition{
            .x = first_position.x,
            .y = first_position.y,
            .z = first_position.z,
            .x_subcell = first_position.x_subcell,
            .y_subcell = first_position.y_subcell,
            .z_subcell = first_position.z_subcell,
        });
        ASSERT_TRUE(forecast_world.setPlayerMovementCapabilities(
            FORECAST_PLAYER_ID,
            first_position.movement_capabilities
        ));
        std::optional<shared::PlayerPosition> const forecast_position = forecast_world.forecastFlightPlayerPosition(
            FORECAST_PLAYER_ID,
            MOVEMENT
        );
        ASSERT_TRUE(forecast_position.has_value());
        shared::HeightTileKey const forecast_center = shared::normalizeHeightTileKey({
            .x = forecast_position->x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
            .y = forecast_position->y / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
        });

        std::vector<shared::HeightTileKey> current_near_keys;
        std::vector<shared::HeightTileKey> forecast_keys;
        current_near_keys.reserve(NEAR_TILE_COUNT);
        forecast_keys.reserve(NEAR_TILE_COUNT);
        for (int32_t y = -NEAR_RADIUS; y <= NEAR_RADIUS; ++y) {
            for (int32_t x = -NEAR_RADIUS; x <= NEAR_RADIUS; ++x) {
                shared::HeightTileKey const current_key = shared::normalizeHeightTileKey({
                    .x = current_center.x + x,
                    .y = current_center.y + y,
                });
                if (shared::heightTileWithinInterest(current_center, current_key, render_distance)) {
                    current_near_keys.push_back(current_key);
                }
                shared::HeightTileKey const forecast_key = shared::normalizeHeightTileKey({
                    .x = forecast_center.x + x,
                    .y = forecast_center.y + y,
                });
                if (shared::heightTileWithinInterest(current_center, forecast_key, render_distance)) {
                    forecast_keys.push_back(forecast_key);
                }
            }
        }
        ASSERT_GT(forecast_keys.size(), 0U);
        if (render_distance == 16U) {
            ASSERT_EQ(current_near_keys.size(), NEAR_TILE_COUNT);
            ASSERT_EQ(forecast_keys.size(), NEAR_TILE_COUNT);
        } else {
            ASSERT_EQ(current_near_keys.size(), NEAR_TILE_COUNT);
            ASSERT_LT(forecast_keys.size(), NEAR_TILE_COUNT);
        }
        std::vector<shared::HeightTileKey> forecast_missing_before_move;
        for (shared::HeightTileKey const key : forecast_keys) {
            if (std::ranges::find(
                    client.received_height_tile_keys.begin(),
                    client.received_height_tile_keys.begin() + initial_received_key_count,
                    key
                ) == client.received_height_tile_keys.begin() + initial_received_key_count) {
                forecast_missing_before_move.push_back(key);
            }
        }
        ASSERT_FALSE(forecast_missing_before_move.empty());

        auto const has_received = [&client](shared::HeightTileKey const key) {
            return std::ranges::find(client.received_height_tile_keys, key)
                != client.received_height_tile_keys.end();
        };
        auto const forecast_is_received = [&forecast_keys, &has_received] {
            return std::ranges::all_of(forecast_keys, has_received);
        };
        auto const all_missing_forecast_received_before = [&client, &forecast_missing_before_move](
            uint32_t const end
        ) {
            auto const prefix_end = client.received_height_tile_keys.begin() + end;
            return std::ranges::all_of(
                forecast_missing_before_move,
                [&client, prefix_end](shared::HeightTileKey const key) {
                    return std::ranges::find(client.received_height_tile_keys.begin(), prefix_end, key) != prefix_end;
                }
            );
        };
        uint32_t inspected_received_keys = initial_received_key_count;
        bool radial_arrived_before_forecast = false;
        auto const inspect_new_deliveries = [&] {
            while (inspected_received_keys < client.received_height_tile_keys.size()) {
                shared::HeightTileKey const key = client.received_height_tile_keys[inspected_received_keys];
                bool const protected_key = std::ranges::find(current_near_keys, key) != current_near_keys.end()
                    || std::ranges::find(forecast_keys, key) != forecast_keys.end();
                if (!protected_key) {
                    radial_arrived_before_forecast |= !all_missing_forecast_received_before(
                        inspected_received_keys
                    );
                }
                ++inspected_received_keys;
            }
        };
        while ((!forecast_is_received() || inflight_deliveries.load(std::memory_order_relaxed) != 0U)
            && std::chrono::steady_clock::now() < deadline) {
            client.poll(POLL_INTERVAL);
            inspect_new_deliveries();
        }
        inspect_new_deliveries();
        EXPECT_FALSE(radial_arrived_before_forecast);
        ASSERT_TRUE(forecast_is_received());
        ASSERT_EQ(inflight_deliveries.load(std::memory_order_relaxed), 0U);

        ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = MOVEMENT,
            .sequence = 2U,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
        ASSERT_TRUE(poll_until([&] {
            return client.last_acknowledged_input == 2U
                && client.last_player_position.has_value()
                && client.last_player_position->acknowledged_input_sequence == 2U;
        }, deadline));
        shared::HeightTileKey const actual_second_center = shared::normalizeHeightTileKey({
            .x = client.last_player_position->x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
            .y = client.last_player_position->y / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
        });
        EXPECT_EQ(actual_second_center, forecast_center);
    }
}

TEST(GameServerPreviewTest, ProgressiveStreamStaysInsideTheNearbyRadialFrontier)
{
    static constexpr std::chrono::seconds TIMEOUT{5};
    static constexpr uint32_t OBSERVED_TILE_COUNT = 512U;
    static constexpr int32_t MAXIMUM_OBSERVED_RADIUS = 20;
    server::GameServer server{
        0,
        {},
        shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(),
        shared::HEIGHT_TILE_INTEREST_RADIUS,
        preparedTerrainWorld(),
    };
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested] { server.run(stop_requested); }};
    PreviewClient client;
    bool const connected = client.connect(core::Address::localhost(server.port()), TIMEOUT);
    bool const joined = connected && client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@',
        .mode = shared::WorldMode::Flight,
        .wants_previews = true,
    }), 0, core::SendMode{core::SendMode::Reliable});
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (joined && client.received_height_tiles < OBSERVED_TILE_COUNT
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    std::vector<shared::HeightTileKey> const keys = heightTileKeys(client.messages);
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    ASSERT_TRUE(connected);
    ASSERT_TRUE(joined);
    ASSERT_GE(keys.size(), OBSERVED_TILE_COUNT);
    int32_t const center_x = shared::World::FLIGHT_SPAWN.x
        / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH);
    int32_t const center_y = shared::World::FLIGHT_SPAWN.y
        / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH);
    int64_t maximum_distance_squared = 0;
    std::vector<int32_t> rows;
    for (uint32_t index = 0U; index < OBSERVED_TILE_COUNT; ++index) {
        shared::HeightTileKey const key = keys[index];
        int64_t const delta_x = key.x - center_x;
        int64_t const delta_y = key.y - center_y;
        maximum_distance_squared = std::max(
            maximum_distance_squared,
            delta_x * delta_x + delta_y * delta_y
        );
        if (std::ranges::find(rows, key.y) == rows.end()) {
            rows.push_back(key.y);
        }
    }
    EXPECT_LE(maximum_distance_squared, MAXIMUM_OBSERVED_RADIUS * MAXIMUM_OBSERVED_RADIUS);
    EXPECT_GT(rows.size(), 20U);
}

TEST(GameServerPreviewTest, ResetsPriorityCursorAfterProgressWhenHeadingChanges)
{
    static constexpr auto TIMEOUT = std::chrono::seconds{10};
    static constexpr auto POLL_INTERVAL = std::chrono::milliseconds{1};
    static constexpr uint32_t NORTH_SETUP_SEQUENCE = 1U;
    static constexpr uint32_t EAST_HEADING_SEQUENCE = 2U;
    static constexpr uint32_t PRIORITY_SCAN_PROGRESS_THRESHOLD = 512U;
    static constexpr uint32_t PRIORITY_SCAN_RESET_ADVANCE_LIMIT = 512U;
    static constexpr uint32_t PRIORITY_SCAN_WRAP_GUARD = 512U;
    static constexpr uint32_t PRIORITY_PREFIX_TILE_COUNT = 256U;

    std::atomic_bool north_setup_metrics_captured{false};
    std::atomic_bool north_cursor_captured{false};
    std::atomic_bool priority_prefixes_ready{false};
    std::atomic_uint32_t priority_cursor_before_rotation{0U};
    std::atomic_int32_t center_x_before_rotation{0};
    std::atomic_int32_t center_y_before_rotation{0};
    std::atomic_bool east_heading_metrics_captured{false};
    std::atomic_uint32_t priority_cursor_after_rotation{0U};
    std::atomic_int32_t center_x_after_rotation{0};
    std::atomic_int32_t center_y_after_rotation{0};
    server::GameServer::BenchmarkHooks const hooks{
        .on_tick = {},
        .on_preview_buffered = {},
        .on_preview_metrics = [
            &north_setup_metrics_captured,
            &north_cursor_captured,
            &priority_prefixes_ready,
            &priority_cursor_before_rotation,
            &center_x_before_rotation,
            &center_y_before_rotation,
            &east_heading_metrics_captured,
            &priority_cursor_after_rotation,
            &center_x_after_rotation,
            &center_y_after_rotation
        ](core::ClientId, server::GameServer::PreviewStreamMetrics const metrics) {
            if (!metrics.terrain_center_valid) {
                return;
            }
            if (metrics.acknowledged_input_sequence == NORTH_SETUP_SEQUENCE
                && !north_setup_metrics_captured.load(std::memory_order_relaxed)) {
                center_x_before_rotation.store(metrics.terrain_center_x, std::memory_order_relaxed);
                center_y_before_rotation.store(metrics.terrain_center_y, std::memory_order_relaxed);
                north_setup_metrics_captured.store(true, std::memory_order_release);
            }
            if (metrics.acknowledged_input_sequence == NORTH_SETUP_SEQUENCE
                && priority_prefixes_ready.load(std::memory_order_acquire)
                && metrics.priority_scan_cursor > PRIORITY_SCAN_PROGRESS_THRESHOLD
                && metrics.priority_scan_cursor
                    < shared::HEIGHT_TILE_INTEREST_COUNT - PRIORITY_SCAN_WRAP_GUARD
                && !north_cursor_captured.load(std::memory_order_relaxed)) {
                priority_cursor_before_rotation.store(metrics.priority_scan_cursor, std::memory_order_relaxed);
                north_cursor_captured.store(true, std::memory_order_release);
            }
            if (metrics.acknowledged_input_sequence == EAST_HEADING_SEQUENCE
                && !east_heading_metrics_captured.load(std::memory_order_relaxed)) {
                priority_cursor_after_rotation.store(metrics.priority_scan_cursor, std::memory_order_relaxed);
                center_x_after_rotation.store(metrics.terrain_center_x, std::memory_order_relaxed);
                center_y_after_rotation.store(metrics.terrain_center_y, std::memory_order_relaxed);
                east_heading_metrics_captured.store(true, std::memory_order_release);
            }
        },
        .before_height_tile_completion = {},
    };
    server::GameServer server{
        0,
        {},
        shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(),
        shared::HEIGHT_TILE_INTEREST_RADIUS,
        preparedTerrainWorld(),
    };
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested, &hooks] { server.run(stop_requested, &hooks); }};
    defer {
        stop_requested.store(true, std::memory_order_relaxed);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };
    PreviewClient client;
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@',
        .mode = shared::WorldMode::Flight,
        .wants_previews = true,
    }), 0, core::SendMode{core::SendMode::Reliable}));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = {.x = 0U, .y = 0U, .z = 0U, .view_x = 0, .view_y = 127},
        .sequence = NORTH_SETUP_SEQUENCE,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    auto const north_progress_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while ((!north_setup_metrics_captured.load(std::memory_order_acquire)
            || client.last_acknowledged_input < NORTH_SETUP_SEQUENCE)
        && std::chrono::steady_clock::now() < north_progress_deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_TRUE(north_setup_metrics_captured.load(std::memory_order_acquire));
    ASSERT_EQ(client.last_acknowledged_input, NORTH_SETUP_SEQUENCE);

    int32_t const center_x = center_x_before_rotation.load(std::memory_order_relaxed);
    int32_t const center_y = center_y_before_rotation.load(std::memory_order_relaxed);
    shared::HeightTileInterest const north_interest = shared::makeHeightTileInterest(
        {.x = center_x, .y = center_y},
        0,
        127
    );
    shared::HeightTileInterest const east_interest = shared::makeHeightTileInterest(
        {.x = center_x, .y = center_y},
        127,
        0
    );
    auto const is_current_near_key = [center_x, center_y](shared::HeightTileKey const key) {
        return shared::heightTileGenerationBand({.x = center_x, .y = center_y}, 0, 127, key)
            <= shared::HeightTileGenerationBand::Near;
    };
    auto const east_priority_prefix = std::span{east_interest.keys}.first(PRIORITY_PREFIX_TILE_COUNT);
    auto const north_priority_prefix = std::span{north_interest.keys}.first(PRIORITY_PREFIX_TILE_COUNT);
    auto const east_frontier_candidate = std::ranges::find_if(
        east_priority_prefix,
        [&north_priority_prefix](shared::HeightTileKey const key) {
            return std::ranges::find(north_priority_prefix, key) == north_priority_prefix.end();
        }
    );
    bool const has_east_frontier_candidate = east_frontier_candidate != east_priority_prefix.end();
    bool const east_frontier_is_outside_near = has_east_frontier_candidate
        && !is_current_near_key(*east_frontier_candidate);
    ASSERT_TRUE(has_east_frontier_candidate);
    EXPECT_TRUE(east_frontier_is_outside_near);
    priority_prefixes_ready.store(true, std::memory_order_release);

    while (!north_cursor_captured.load(std::memory_order_acquire)
        && std::chrono::steady_clock::now() < north_progress_deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_TRUE(north_cursor_captured.load(std::memory_order_acquire));
    uint32_t const cursor_before_rotation = priority_cursor_before_rotation.load(std::memory_order_relaxed);
    ASSERT_GT(cursor_before_rotation, PRIORITY_SCAN_PROGRESS_THRESHOLD);
    ASSERT_LT(
        cursor_before_rotation,
        shared::HEIGHT_TILE_INTEREST_COUNT - PRIORITY_SCAN_WRAP_GUARD
    );

    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = {.x = 0U, .y = 0U, .z = 0U, .view_x = 127, .view_y = 0},
        .sequence = EAST_HEADING_SEQUENCE,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    auto const east_heading_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while ((!east_heading_metrics_captured.load(std::memory_order_acquire)
            || client.last_acknowledged_input < EAST_HEADING_SEQUENCE)
        && std::chrono::steady_clock::now() < east_heading_deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_TRUE(east_heading_metrics_captured.load(std::memory_order_acquire));
    EXPECT_EQ(client.last_acknowledged_input, EAST_HEADING_SEQUENCE);
    EXPECT_EQ(center_x_after_rotation.load(std::memory_order_relaxed), center_x);
    EXPECT_EQ(center_y_after_rotation.load(std::memory_order_relaxed), center_y);
    uint32_t const cursor_after_rotation = priority_cursor_after_rotation.load(std::memory_order_relaxed);
    // Interest refresh and the enclosing stream pump each inspect at most 256 priority candidates.
    EXPECT_LE(cursor_after_rotation, PRIORITY_SCAN_RESET_ADVANCE_LIMIT);
    EXPECT_LT(cursor_after_rotation, cursor_before_rotation);
}

TEST(GameServerPreviewTest, CameraRotationKeepsInterestAndMovementAddsFreshFrontier)
{
    static constexpr std::chrono::seconds TIMEOUT{10};
    static constexpr uint32_t OBSERVED_TILE_COUNT = 16U;
    static constexpr uint16_t FLIGHT_SPEEDUP = 500U;
    static constexpr uint32_t MOVEMENT_INPUT_COUNT = (
        shared::HEIGHT_TILE_INTEREST_RADIUS * shared::HEIGHT_TILE_SIDE_LENGTH * shared::SUBCELLS_PER_CELL
    ) / (shared::MOVEMENT_SUBCELLS_PER_TICK * FLIGHT_SPEEDUP) + 1U;
    static constexpr uint32_t OBSERVED_NEW_FRONTIER_TILES = 8U;
    server::GameServer server{
        0,
        {},
        shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(),
        shared::HEIGHT_TILE_INTEREST_RADIUS,
        preparedTerrainWorld(),
    };
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested] { server.run(stop_requested); }};
    PreviewClient client;
    bool const connected = client.connect(core::Address::localhost(server.port()), TIMEOUT);
    bool const joined = connected && client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), 0, core::SendMode{core::SendMode::Reliable});
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (joined && client.received_height_tiles < OBSERVED_TILE_COUNT
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    size_t const before_rotation = client.messages.size();
    bool const rotated = client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = {.x = 0U, .y = 0U, .view_x = 127, .view_y = 0},
        .sequence = 1U,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable});
    auto const rotation_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (rotated && client.last_acknowledged_input < 1U
        && std::chrono::steady_clock::now() < rotation_deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    size_t const after_rotation_ack = client.messages.size();

    int32_t const original_center_x = shared::World::FLIGHT_SPAWN.x
        / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH);
    uint32_t const final_input_sequence = 1U + MOVEMENT_INPUT_COUNT;
    bool sent_all_movement = true;
    for (uint32_t sequence = 2U; sequence <= final_input_sequence; ++sequence) {
        sent_all_movement = client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {.x = 127U, .y = 0U, .accelerated = true, .speedup = FLIGHT_SPEEDUP, .view_x = 127},
            .sequence = sequence,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}) && sent_all_movement;
    }
    auto const movement_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (sent_all_movement
        && (client.last_acknowledged_input < final_input_sequence
            || client.last_player_x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH) <= original_center_x)
        && std::chrono::steady_clock::now() < movement_deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    int32_t const moved_center_x = client.last_player_x
        / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH);
    shared::HeightTileInterest const original_interest = shared::makeHeightTileInterest(
        {.x = original_center_x, .y = shared::World::FLIGHT_SPAWN.y
            / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH)},
        127,
        0
    );
    shared::HeightTileInterest const moved_interest = shared::makeHeightTileInterest(
        {.x = moved_center_x, .y = shared::World::FLIGHT_SPAWN.y
            / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH)},
        127,
        0
    );
    auto const key_id = [](shared::HeightTileKey const key) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(key.x)) << 32U)
            | static_cast<uint32_t>(key.y);
    };
    std::unordered_set<uint64_t> original_interest_ids;
    original_interest_ids.reserve(original_interest.keys.size());
    for (shared::HeightTileKey const key : original_interest.keys) {
        original_interest_ids.insert(key_id(key));
    }
    std::unordered_set<uint64_t> new_frontier_ids;
    for (shared::HeightTileKey const key : moved_interest.keys) {
        uint64_t const id = key_id(key);
        if (!original_interest_ids.contains(id)) {
            new_frontier_ids.insert(id);
        }
    }
    std::vector<shared::HeightTileKey> moved_frontier_tiles;
    auto const collect_new_frontier_tiles = [&] {
        moved_frontier_tiles = heightTileKeysFrom(client.messages, after_rotation_ack);
        return static_cast<uint32_t>(std::ranges::count_if(
            moved_frontier_tiles,
            [&key_id, &new_frontier_ids](shared::HeightTileKey const key) {
                return new_frontier_ids.contains(key_id(key));
            }
        ));
    };
    while (sent_all_movement && collect_new_frontier_tiles() < OBSERVED_NEW_FRONTIER_TILES
        && std::chrono::steady_clock::now() < movement_deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    std::span<shared::Message const> const rotation_only = std::span{client.messages}.subspan(
        before_rotation,
        after_rotation_ack - before_rotation
    );
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    ASSERT_TRUE(connected);
    ASSERT_TRUE(joined);
    ASSERT_TRUE(rotated);
    ASSERT_TRUE(sent_all_movement);
    EXPECT_GE(client.last_acknowledged_input, 1U);
    EXPECT_EQ(client.last_acknowledged_input, final_input_sequence);
    EXPECT_GT(moved_center_x, original_center_x);
    EXPECT_FALSE(new_frontier_ids.empty());
    EXPECT_GE(collect_new_frontier_tiles(), OBSERVED_NEW_FRONTIER_TILES);
    EXPECT_EQ(removalCount(rotation_only), 0U);
}

TEST(GameServerPreviewTest, StreamsPlayerCenteredHeightTilesAndRemovesDepartedTiles)
{
    static constexpr std::chrono::seconds STREAM_TIMEOUT{90};
    static constexpr uint32_t RENDER_DISTANCE = 16U;
    static constexpr std::chrono::seconds MOVEMENT_TIMEOUT{10};
    static constexpr std::chrono::seconds PROGRESS_INTERVAL{1};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr uint32_t MOVEMENT_INPUT_COUNT = (
        shared::HEIGHT_TILE_SIDE_LENGTH * shared::SUBCELLS_PER_CELL
        + shared::MOVEMENT_SUBCELLS_PER_TICK - 1U
    ) / shared::MOVEMENT_SUBCELLS_PER_TICK;
    shared::HeightTileInterest const expected_interest = shared::makeHeightTileInterest(
        {
            .x = shared::World::FLIGHT_SPAWN.x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
            .y = shared::World::FLIGHT_SPAWN.y / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
        },
        0,
        127,
        RENDER_DISTANCE
    );
    uint32_t const expected_height_tiles = static_cast<uint32_t>(expected_interest.keys.size());

    uint32_t const hardware_thread_count = static_cast<uint32_t>(std::thread::hardware_concurrency());
    auto const server_started_at = std::chrono::steady_clock::now();
    auto next_server_progress = server_started_at + PROGRESS_INTERVAL;
    std::atomic_uint32_t near_stage_sample_count{0U};
    std::atomic_bool near_stage_partition_valid{true};
    server::GameServer::BenchmarkHooks const hooks{
        .on_tick = {},
        .on_preview_buffered = {},
        .on_preview_metrics = [server_started_at, &next_server_progress, &near_stage_sample_count,
            &near_stage_partition_valid](
            core::ClientId,
            server::GameServer::PreviewStreamMetrics const metrics
        ) {
            if (metrics.terrain_center_valid) {
                uint32_t const classified = metrics.near_window_resident_tiles
                    + metrics.near_window_queued_tiles
                    + metrics.near_window_dispatched_tiles
                    + metrics.near_window_ready_tiles
                    + metrics.near_window_inflight_additions
                    + metrics.near_window_untracked_tiles;
                if (metrics.near_window_desired_tiles != 225U || classified != metrics.near_window_desired_tiles) {
                    near_stage_partition_valid.store(false, std::memory_order_relaxed);
                }
                near_stage_sample_count.fetch_add(1U, std::memory_order_relaxed);
            }
            auto const now = std::chrono::steady_clock::now();
            if (now < next_server_progress) {
                return;
            }
            auto const average_ms = [](
                std::chrono::nanoseconds const total,
                uint64_t const samples
            ) {
                return samples == 0U
                    ? int64_t{0}
                    : total.count() / static_cast<int64_t>(samples) / 1'000'000;
            };
            CORE_INFO(
                "Server preview at {} ms: queued={}, dispatched={}, worker pending={}, worker submitted={}, "
                "ready={}, in-flight batches={}, in-flight additions={}, credits={}, resident={}, "
                "tile jobs submitted/started/finished/collected={}/{}/{}/{}, "
                "tile enqueue-submit/executor/run/collect avg/max ms={}/{}/{}/{}/{}/{}/{}/{}, "
                "world jobs submitted/started/finished/collected={}/{}/{}/{}, "
                "world enqueue-submit/executor/run/collect avg/max ms={}/{}/{}/{}/{}/{}/{}/{}, "
                "loop interval/work/tick/pump/poll ms={}/{}/{}/{}/{}, input received/ack/pending/failures={}/{}/{}/{}, "
                "credit samples/avg/max ms={}/{}/{}",
                std::chrono::duration_cast<std::chrono::milliseconds>(now - server_started_at).count(),
                metrics.queued_tiles,
                metrics.dispatched_tiles,
                metrics.pending_worker_jobs,
                metrics.submitted_worker_jobs,
                metrics.ready_tiles,
                metrics.inflight_deliveries,
                metrics.inflight_additions,
                metrics.delivery_credits,
                metrics.resident_tiles,
                metrics.height_tile_jobs.submitted_total,
                metrics.height_tile_jobs.started_total,
                metrics.height_tile_jobs.finished_total,
                metrics.height_tile_jobs.collected_total,
                average_ms(
                    metrics.height_tile_jobs.enqueue_to_submit_total,
                    metrics.height_tile_jobs.submitted_total
                ),
                metrics.height_tile_jobs.enqueue_to_submit_max.count() / 1'000'000,
                average_ms(metrics.height_tile_jobs.executor_queue_total, metrics.height_tile_jobs.started_total),
                metrics.height_tile_jobs.executor_queue_max.count() / 1'000'000,
                average_ms(metrics.height_tile_jobs.execution_total, metrics.height_tile_jobs.finished_total),
                metrics.height_tile_jobs.execution_max.count() / 1'000'000,
                average_ms(
                    metrics.height_tile_jobs.completion_to_collection_total,
                    metrics.height_tile_jobs.collected_total
                ),
                metrics.height_tile_jobs.completion_to_collection_max.count() / 1'000'000,
                metrics.world_generation_jobs.submitted_total,
                metrics.world_generation_jobs.started_total,
                metrics.world_generation_jobs.finished_total,
                metrics.world_generation_jobs.collected_total,
                average_ms(
                    metrics.world_generation_jobs.enqueue_to_submit_total,
                    metrics.world_generation_jobs.submitted_total
                ),
                metrics.world_generation_jobs.enqueue_to_submit_max.count() / 1'000'000,
                average_ms(metrics.world_generation_jobs.executor_queue_total, metrics.world_generation_jobs.started_total),
                metrics.world_generation_jobs.executor_queue_max.count() / 1'000'000,
                average_ms(metrics.world_generation_jobs.execution_total, metrics.world_generation_jobs.finished_total),
                metrics.world_generation_jobs.execution_max.count() / 1'000'000,
                average_ms(
                    metrics.world_generation_jobs.completion_to_collection_total,
                    metrics.world_generation_jobs.collected_total
                ),
                metrics.world_generation_jobs.completion_to_collection_max.count() / 1'000'000,
                metrics.server_loop_interval.count() / 1'000'000,
                metrics.server_loop_work.count() / 1'000'000,
                metrics.server_tick.count() / 1'000'000,
                metrics.stream_pump.count() / 1'000'000,
                metrics.network_poll.count() / 1'000'000,
                metrics.latest_received_input_sequence,
                metrics.acknowledged_input_sequence,
                metrics.pending_input_count,
                metrics.materialization_admission_failures,
                metrics.delivery_credit_samples,
                metrics.delivery_credit_samples == 0U
                    ? 0U
                    : metrics.delivery_credit_total.count()
                        / static_cast<int64_t>(metrics.delivery_credit_samples) / 1'000'000,
                metrics.delivery_credit_max.count() / 1'000'000
            );
            next_server_progress = now + PROGRESS_INTERVAL;
        },
        .before_height_tile_completion = {},
    };

    CORE_INFO(
        "Full-radius preview worker budget: hardware threads={}, terrain workers={}",
        hardware_thread_count,
        server::GameServer::terrainWorkerCount(hardware_thread_count)
    );
    server::GameServer server{
        0,
        {},
        shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(),
        RENDER_DISTANCE,
        preparedTerrainWorld(),
    };
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested, &hooks] {
        server.run(stop_requested, &hooks);
    }};
    PreviewClient client{true, true, '@', false};
    client.received_height_tile_keys.reserve(expected_height_tiles);
    if (!client.connect(core::Address::localhost(server.port()), STREAM_TIMEOUT)) {
        stop_requested.store(true, std::memory_order_relaxed);
        server_thread.join();
        ADD_FAILURE() << "Client failed to connect within " << STREAM_TIMEOUT.count() << " seconds";
        return;
    }
    if (!client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@',
        .mode = shared::WorldMode::Flight,
        .wants_previews = true,
    }), 0, core::SendMode{core::SendMode::Reliable})) {
        stop_requested.store(true, std::memory_order_relaxed);
        server_thread.join();
        ADD_FAILURE() << "Client failed to join the preview stream";
        return;
    }

    auto const started_at = std::chrono::steady_clock::now();
    auto const stream_deadline = started_at + STREAM_TIMEOUT;
    auto next_progress = started_at + PROGRESS_INTERVAL;
    uint32_t poll_iterations = 0U;
    while (client.received_height_tiles < expected_height_tiles
        && std::chrono::steady_clock::now() < stream_deadline) {
        client.poll(POLL_INTERVAL);
        ++poll_iterations;
        auto const now = std::chrono::steady_clock::now();
        if (now >= next_progress) {
            CORE_INFO(
                "Client preview: {}/{} tiles, {} batches, {} credit messages, {} poll iterations after {} ms",
                client.received_height_tiles,
                expected_height_tiles,
                client.received_delivery_batches,
                client.sent_delivery_credit_messages,
                poll_iterations,
                std::chrono::duration_cast<std::chrono::milliseconds>(now - started_at).count()
            );
            next_progress = now + PROGRESS_INTERVAL;
        }
    }

    bool const received_all_height_tiles = client.received_height_tiles == expected_height_tiles;
    std::vector<shared::HeightTileKey> received_keys = std::move(client.received_height_tile_keys);
    std::vector<shared::HeightTileKey> expected_keys = expected_interest.keys;
    auto const key_order = [](shared::HeightTileKey const first, shared::HeightTileKey const second) {
        return std::tie(first.x, first.y) < std::tie(second.x, second.y);
    };
    std::ranges::sort(received_keys, key_order);
    std::ranges::sort(expected_keys, key_order);
    bool sent_all_inputs = true;
    for (uint32_t sequence{1U}; sequence <= MOVEMENT_INPUT_COUNT; ++sequence) {
        sent_all_inputs = client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {.x = 127U, .y = 0U},
            .sequence = sequence,
        }), 0, core::SendMode{core::SendMode::Reliable}) && sent_all_inputs;
    }
    auto const movement_deadline = std::chrono::steady_clock::now() + MOVEMENT_TIMEOUT;
    while (client.received_removals == 0U
        && std::chrono::steady_clock::now() < movement_deadline) {
        client.poll(POLL_INTERVAL);
    }
    bool const received_removal = client.received_removals > 0U;
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    CORE_INFO(
        "Full-radius preview result: {}/{} tiles, {} batches, {} credit messages, "
        "{} poll iterations; connected-to-result duration={} ms",
        client.received_height_tiles,
        expected_height_tiles,
        client.received_delivery_batches,
        client.sent_delivery_credit_messages,
        poll_iterations,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at
        ).count()
    );
    EXPECT_TRUE(received_all_height_tiles) << "received " << client.received_height_tiles
        << " of " << expected_height_tiles;
    EXPECT_TRUE(client.messages.empty());
    EXPECT_EQ(received_keys, expected_keys);
    EXPECT_EQ(client.invalid_height_tiles, 0U);
    EXPECT_EQ(client.received_height_tile_descriptors, 1U);
    EXPECT_EQ(client.descriptor_max_height_tiles, expected_height_tiles);
    EXPECT_EQ(client.descriptor_max_height_tile_bytes, shared::HEIGHT_TILE_PAYLOAD_BYTES);
    EXPECT_TRUE(sent_all_inputs);
    EXPECT_TRUE(received_removal);
    EXPECT_GT(near_stage_sample_count.load(std::memory_order_relaxed), 0U);
    EXPECT_TRUE(near_stage_partition_valid.load(std::memory_order_relaxed));
    EXPECT_GT(
        client.last_player_x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
        shared::World::FLIGHT_SPAWN.x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH)
    );
}

TEST(GameServerPreviewTest, FastFlightGeneratesFrontierTilesWithinDeadline)
{
    static constexpr std::chrono::seconds TIMEOUT{5};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr uint32_t INPUT_COUNT = 4U;
    static constexpr uint32_t OBSERVED_TILE_COUNT = 16U;
    static constexpr int32_t MAXIMUM_FRONTIER_DISTANCE = 16;

    server::GameServer server{
        0,
        {},
        shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(),
        shared::HEIGHT_TILE_INTEREST_RADIUS,
        preparedTerrainWorld(),
    };
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested] {
        server.run(stop_requested);
    }};
    PreviewClient client{true, false};
    bool const connected = client.connect(core::Address::localhost(server.port()), TIMEOUT);
    bool const joined = connected && client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@',
        .mode = shared::WorldMode::Flight,
        .wants_previews = true,
    }), 0, core::SendMode{core::SendMode::Reliable});
    auto const initial_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (joined && std::ranges::none_of(client.messages, [](shared::Message const& message) {
        return std::holds_alternative<shared::ServerHeightTileDescriptorMessage>(message);
    })
        && std::chrono::steady_clock::now() < initial_deadline) {
        client.poll(POLL_INTERVAL);
    }
    bool sent_all_inputs = true;
    for (uint32_t sequence{1U}; sequence <= INPUT_COUNT; ++sequence) {
        sent_all_inputs = client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {.x = 127U, .y = 0U, .accelerated = true, .speedup = 500U},
            .sequence = sequence,
        }), 0, core::SendMode{core::SendMode::Reliable}) && sent_all_inputs;
    }
    auto const movement_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (joined && client.last_acknowledged_input < INPUT_COUNT
        && std::chrono::steady_clock::now() < movement_deadline) {
        client.poll(POLL_INTERVAL);
    }
    size_t const post_movement_message = client.messages.size();
    bool const started_deliveries = client.send(shared::encodeMessage(
        shared::ClientHeightTileCreditMessage{
            .world_revision = 1U,
            .delivery_token = 0U,
            .credits = shared::HEIGHT_TILE_DELIVERY_WINDOW,
            .not_resident_keys = {},
        }
    ), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable});
    auto const player = shared::World::FLIGHT_SPAWN;
    shared::HeightTileKey const center{
        .x = (client.last_player_x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH)),
        .y = player.y / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
    };
    auto const is_frontier_key = [center](shared::HeightTileKey const key) {
        int32_t const x_distance = std::abs(key.x - center.x);
        int32_t const y_distance = std::abs(key.y - center.y);
        return std::max(x_distance, y_distance) <= MAXIMUM_FRONTIER_DISTANCE;
    };
    std::vector<shared::HeightTileKey> keys;
    while (joined && std::ranges::count_if(keys, is_frontier_key) < OBSERVED_TILE_COUNT
        && std::chrono::steady_clock::now() < movement_deadline) {
        client.poll(POLL_INTERVAL);
        keys = heightTileKeysFrom(client.messages, post_movement_message);
    }
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    ASSERT_TRUE(connected);
    ASSERT_TRUE(joined);
    ASSERT_TRUE(sent_all_inputs);
    ASSERT_TRUE(started_deliveries);
    ASSERT_EQ(client.last_acknowledged_input, INPUT_COUNT);
    EXPECT_GE(std::ranges::count_if(keys, is_frontier_key), OBSERVED_TILE_COUNT);
}

TEST(GameServerPreviewTest, BackgroundGenerationCooldownStaysOpenDuringCenterTravel)
{
    static constexpr std::chrono::seconds TIMEOUT{5};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr uint32_t MOVEMENT_INPUT_COUNT = 8U;

    std::atomic_bool stop_requested{false};
    std::atomic_bool movement_started{false};
    std::atomic_bool initial_center_captured{false};
    std::atomic_bool initially_eligible{false};
    std::atomic_bool movement_center_change_observed{false};
    std::atomic_bool eligible_at_first_movement_center{false};
    std::atomic_int32_t initial_center_x{0};
    server::GameServer::BenchmarkHooks const hooks{
        .on_tick = {},
        .on_preview_buffered = {},
        .on_preview_metrics = [
            &movement_started,
            &initial_center_captured,
            &initially_eligible,
            &movement_center_change_observed,
            &eligible_at_first_movement_center,
            &initial_center_x
        ](core::ClientId, server::GameServer::PreviewStreamMetrics const metrics) {
            if (!metrics.terrain_center_valid) {
                return;
            }
            if (!initial_center_captured.exchange(true, std::memory_order_relaxed)) {
                initial_center_x.store(metrics.terrain_center_x, std::memory_order_relaxed);
            }
            if (metrics.background_generation_eligible) {
                initially_eligible.store(true, std::memory_order_relaxed);
            }
            if (movement_started.load(std::memory_order_relaxed)
                && metrics.terrain_center_x != initial_center_x.load(std::memory_order_relaxed)
                && !movement_center_change_observed.exchange(true, std::memory_order_relaxed)) {
                eligible_at_first_movement_center.store(
                    metrics.background_generation_eligible,
                    std::memory_order_relaxed
                );
            }
        },
        .before_height_tile_completion = {},
    };
    server::GameServer server{
        0,
        {},
        shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(),
        shared::HEIGHT_TILE_INTEREST_RADIUS,
        preparedTerrainWorld(),
    };
    std::thread server_thread{[&server, &stop_requested, &hooks] {
        server.run(stop_requested, &hooks);
    }};
    defer {
        stop_requested.store(true, std::memory_order_relaxed);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };
    PreviewClient client;
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));

    auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.received_height_tile_descriptors == 0U
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_EQ(client.received_height_tile_descriptors, 1U);
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!initially_eligible.load(std::memory_order_relaxed)
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_TRUE(initially_eligible.load(std::memory_order_relaxed));

    movement_started.store(true, std::memory_order_relaxed);
    bool sent_all_inputs = true;
    for (uint32_t sequence = 1U; sequence <= MOVEMENT_INPUT_COUNT; ++sequence) {
        sent_all_inputs = client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {.x = 127U, .y = 0U, .accelerated = true, .speedup = 500U},
            .sequence = sequence,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}) && sent_all_inputs;
    }
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (sent_all_inputs
        && (client.last_acknowledged_input < MOVEMENT_INPUT_COUNT
            || !movement_center_change_observed.load(std::memory_order_relaxed))
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_TRUE(sent_all_inputs);
    ASSERT_EQ(client.last_acknowledged_input, MOVEMENT_INPUT_COUNT);
    ASSERT_TRUE(movement_center_change_observed.load(std::memory_order_relaxed));
    EXPECT_TRUE(eligible_at_first_movement_center.load(std::memory_order_relaxed));
}

TEST(GameServerPreviewTest, RepeatedCenterTravelRestoresNearTerrainAheadOfQueuedBacklog)
{
    static constexpr std::chrono::seconds TIMEOUT{10};
    static constexpr std::chrono::seconds NEAR_PROGRESS_TIMEOUT{4};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr uint32_t MAXIMUM_BUFFERED_TILES = 128U;
    static constexpr uint32_t MAXIMUM_BACKLOG_TILES_BEFORE_NEAR_PROGRESS =
        MAXIMUM_BUFFERED_TILES + shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY;
    static constexpr uint16_t FLIGHT_SPEEDUP = 500U;
    static constexpr uint32_t HEADING_SETUP_SEQUENCE = 1U;
    static constexpr uint32_t TRAVEL_SEQUENCE = HEADING_SETUP_SEQUENCE + 1U;

    std::atomic_uint32_t buffered_tiles{0U};
    std::atomic_uint32_t queued_tiles{0U};
    server::GameServer::BenchmarkHooks const hooks{
        .on_tick = {},
        .on_preview_buffered = [&buffered_tiles](core::ClientId, uint32_t const count) {
            buffered_tiles.store(count, std::memory_order_relaxed);
        },
        .on_preview_metrics = [&queued_tiles](
            core::ClientId,
            server::GameServer::PreviewStreamMetrics const metrics
        ) {
            queued_tiles.store(metrics.queued_tiles, std::memory_order_relaxed);
        },
        .before_height_tile_completion = {},
    };
    server::GameServer server{
        0,
        {},
        shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(),
        shared::HEIGHT_TILE_INTEREST_RADIUS,
        preparedTerrainWorld(),
    };
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested, &hooks] {
        server.run(stop_requested, &hooks);
    }};
    defer {
        stop_requested.store(true, std::memory_order_relaxed);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };
    PreviewClient client{false, false};
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));

    auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.received_height_tile_descriptors == 0U
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_EQ(client.received_height_tile_descriptors, 1U);
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientHeightTileCreditMessage{
        .world_revision = 1U,
        .delivery_token = 0U,
        .credits = shared::HEIGHT_TILE_DELIVERY_WINDOW,
        .not_resident_keys = {},
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));

    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while ((client.received_delivery_batches == 0U
            || buffered_tiles.load(std::memory_order_relaxed) < MAXIMUM_BUFFERED_TILES
            || queued_tiles.load(std::memory_order_relaxed) == 0U)
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_TRUE(waitForStalledDeliveryWindow(client));
    expectBoundedDeliveryWindow(deliveryBatchesFrom(client.messages, 0U));
    ASSERT_GT(client.received_height_tiles, 0U);
    ASSERT_EQ(buffered_tiles.load(std::memory_order_relaxed), MAXIMUM_BUFFERED_TILES);
    ASSERT_GT(queued_tiles.load(std::memory_order_relaxed), 0U);

    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = {
            .x = 127U,
            .y = 0U,
            .accelerated = true,
            .speedup = 2U,
        },
        .sequence = HEADING_SETUP_SEQUENCE,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.last_acknowledged_input < HEADING_SETUP_SEQUENCE
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_EQ(client.last_acknowledged_input, HEADING_SETUP_SEQUENCE);
    bool const sent_all_inputs = client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = {
            .x = 127U,
            .y = 0U,
            .accelerated = true,
            .speedup = FLIGHT_SPEEDUP,
        },
        .sequence = TRAVEL_SEQUENCE,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable});
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (sent_all_inputs && client.last_acknowledged_input < TRAVEL_SEQUENCE
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_TRUE(sent_all_inputs);
    ASSERT_EQ(client.last_acknowledged_input, TRAVEL_SEQUENCE);

    shared::HeightTileKey const center{
        .x = client.last_player_x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
        .y = shared::World::FLIGHT_SPAWN.y / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
    };
    uint32_t const first_post_travel_message = static_cast<uint32_t>(client.messages.size());
    std::vector<shared::ServerHeightTileBatchMessage> const stalled_batches = deliveryBatchesFrom(
        client.messages,
        0U
    );
    expectBoundedDeliveryWindow(stalled_batches);

    client.setAcknowledgesDeliveries(true);
    bool acknowledged_all = true;
    for (shared::ServerHeightTileBatchMessage const& batch : stalled_batches) {
        acknowledged_all = client.acknowledgeDelivery(batch.delivery_token) && acknowledged_all;
    }
    deadline = std::chrono::steady_clock::now() + NEAR_PROGRESS_TIMEOUT;
    std::vector<shared::HeightTileKey> streamed_after_travel;
    auto const is_near_current_key = [center](shared::HeightTileKey const key) {
        return shared::heightTileGenerationBand(center, 127, 0, key)
            <= shared::HeightTileGenerationBand::Near;
    };
    auto const near_progress_prefix_length = [&streamed_after_travel, &is_near_current_key] {
        uint32_t near_tiles = 0U;
        uint32_t const streamed_tile_count = static_cast<uint32_t>(streamed_after_travel.size());
        for (uint32_t index = 0U; index < streamed_tile_count; ++index) {
            if (is_near_current_key(streamed_after_travel[index]) && ++near_tiles == 16U) {
                return index + 1U;
            }
        }
        return uint32_t{0U};
    };
    auto const streamed_after_travel_count = [&streamed_after_travel] {
        return static_cast<uint32_t>(streamed_after_travel.size());
    };
    uint32_t near_progress_prefix = near_progress_prefix_length();
    while (acknowledged_all && near_progress_prefix == 0U
        && streamed_after_travel_count() < MAXIMUM_BACKLOG_TILES_BEFORE_NEAR_PROGRESS
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
        streamed_after_travel = heightTileKeysFrom(client.messages, first_post_travel_message);
        near_progress_prefix = near_progress_prefix_length();
    }
    if (streamed_after_travel.empty()) {
        streamed_after_travel = heightTileKeysFrom(client.messages, first_post_travel_message);
        near_progress_prefix = near_progress_prefix_length();
    }

    std::string streamed_band_trace;
    std::string streamed_near_keys;
    for (uint32_t index = 0U; index < streamed_after_travel_count(); ++index) {
        shared::HeightTileKey const key = streamed_after_travel[index];
        bool const is_near = is_near_current_key(key);
        streamed_band_trace += is_near ? 'N' : '.';
        if (is_near) {
            streamed_near_keys += std::to_string(index);
            streamed_near_keys += ":(";
            streamed_near_keys += std::to_string(key.x);
            streamed_near_keys += ',';
            streamed_near_keys += std::to_string(key.y);
            streamed_near_keys += ") ";
        }
    }
    SCOPED_TRACE(::testing::Message()
        << "center=(" << center.x << ',' << center.y << ") streamed=" << streamed_after_travel_count()
        << " near_prefix=" << near_progress_prefix << " bands=" << streamed_band_trace
        << " near_keys=" << streamed_near_keys);
    EXPECT_TRUE(acknowledged_all);
    EXPECT_GT(near_progress_prefix, 0U);
    EXPECT_LE(near_progress_prefix, MAXIMUM_BACKLOG_TILES_BEFORE_NEAR_PROGRESS);
}

TEST(GameServerPreviewTest, LateFarCompletionCannotUseAWindowWhileCurrentNearWorkIsDispatched)
{
    static constexpr std::chrono::seconds TIMEOUT{15};
    static constexpr std::chrono::seconds COMPLETION_HOOK_WATCHDOG{30};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr int32_t X200_CENTER_STEP_TILES = 7;
    static constexpr int32_t CURRENT_NEAR_WINDOW_RADIUS = 7;
    static constexpr int32_t PENDING_NEAR_OFFSET = 14;
    static constexpr int32_t LATE_FAR_OFFSET = 20;
    static constexpr uint32_t MOVEMENT_SEQUENCE = 1U;

    uint32_t const hardware_concurrency = static_cast<uint32_t>(std::thread::hardware_concurrency());
    if (server::GameServer::terrainWorkerCount(hardware_concurrency) < 2U) {
        GTEST_SKIP() << "The controlled two-key completion gates require at least two terrain workers";
    }

    shared::HeightTileKey const initial_center{
        .x = shared::World::FLIGHT_SPAWN.x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
        .y = shared::World::FLIGHT_SPAWN.y / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
    };
    shared::HeightTileKey const expected_travel_center = shared::normalizeHeightTileKey({
        .x = initial_center.x + X200_CENTER_STEP_TILES,
        .y = initial_center.y,
    });
    shared::HeightTileKey const pending_near_key = shared::normalizeHeightTileKey({
        .x = initial_center.x + PENDING_NEAR_OFFSET,
        .y = initial_center.y,
    });
    shared::HeightTileKey const late_far_key = shared::normalizeHeightTileKey({
        .x = initial_center.x + LATE_FAR_OFFSET,
        .y = initial_center.y,
    });

    std::atomic_bool stop_requested{false};
    std::atomic_bool pending_near_entered{false};
    std::atomic_bool pending_near_release{false};
    std::atomic_bool late_far_entered{false};
    std::atomic_bool late_far_release{false};
    std::atomic_bool completion_hook_timed_out{false};
    std::atomic_bool initial_center_captured{false};
    std::atomic_int32_t current_center_x{0};
    std::atomic_uint32_t current_near_dispatched{0U};
    std::atomic_uint64_t finished_jobs{0U};
    std::atomic_uint64_t collected_jobs{0U};

    auto const wait_for_release = [&](std::atomic_bool& entered, std::atomic_bool const& release) {
        entered.store(true, std::memory_order_release);
        auto const gate_deadline = std::chrono::steady_clock::now() + COMPLETION_HOOK_WATCHDOG;
        while (!release.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < gate_deadline) {
            std::this_thread::sleep_for(POLL_INTERVAL);
        }
        if (!release.load(std::memory_order_acquire)) {
            completion_hook_timed_out.store(true, std::memory_order_release);
        }
    };

    server::GameServer::BenchmarkHooks const hooks{
        .on_tick = {},
        .on_preview_buffered = {},
        .on_preview_metrics = [
            &initial_center_captured,
            &current_center_x,
            &current_near_dispatched,
            &finished_jobs,
            &collected_jobs
        ](core::ClientId, server::GameServer::PreviewStreamMetrics const metrics) {
            if (!metrics.terrain_center_valid) {
                return;
            }
            if (!initial_center_captured.load(std::memory_order_relaxed)) {
                initial_center_captured.store(true, std::memory_order_release);
            }
            current_center_x.store(metrics.terrain_center_x, std::memory_order_relaxed);
            current_near_dispatched.store(metrics.near_window_dispatched_tiles, std::memory_order_relaxed);
            finished_jobs.store(metrics.height_tile_jobs.finished_total, std::memory_order_relaxed);
            collected_jobs.store(metrics.height_tile_jobs.collected_total, std::memory_order_relaxed);
        },
        .before_height_tile_completion = [
            &pending_near_key,
            &late_far_key,
            &pending_near_entered,
            &pending_near_release,
            &late_far_entered,
            &late_far_release,
            &wait_for_release
        ](core::ClientId, shared::HeightTileKey const key) {
            if (key == pending_near_key) {
                wait_for_release(pending_near_entered, pending_near_release);
            } else if (key == late_far_key) {
                wait_for_release(late_far_entered, late_far_release);
            }
        },
    };

    server::GameServer server{
        0,
        {},
        shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(),
        shared::HEIGHT_TILE_INTEREST_RADIUS,
        preparedTerrainWorld(),
    };
    std::thread server_thread{[&server, &stop_requested, &hooks] {
        server.run(stop_requested, &hooks);
    }};
    defer {
        pending_near_release.store(true, std::memory_order_release);
        late_far_release.store(true, std::memory_order_release);
        stop_requested.store(true, std::memory_order_relaxed);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };

    PreviewClient client;
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@',
        .mode = shared::WorldMode::Flight,
        .wants_previews = true,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));

    auto const poll_until = [&](auto const& predicate, auto const stop_at) {
        while (!predicate() && std::chrono::steady_clock::now() < stop_at) {
            client.poll(POLL_INTERVAL);
        }
        return predicate();
    };
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    ASSERT_TRUE(poll_until([&] {
        return client.received_height_tile_descriptors == 1U
            && initial_center_captured.load(std::memory_order_acquire)
            && pending_near_entered.load(std::memory_order_acquire)
            && late_far_entered.load(std::memory_order_acquire);
    }, deadline));
    EXPECT_FALSE(completion_hook_timed_out.load(std::memory_order_acquire));
    EXPECT_EQ(current_center_x.load(std::memory_order_relaxed), initial_center.x);

    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = {.x = 127U, .y = 0U, .accelerated = true, .speedup = 200U},
        .sequence = MOVEMENT_SEQUENCE,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    ASSERT_TRUE(poll_until([&] {
        return client.last_acknowledged_input == MOVEMENT_SEQUENCE
            && current_center_x.load(std::memory_order_relaxed) == expected_travel_center.x;
    }, deadline));
    EXPECT_EQ(current_center_x.load(std::memory_order_relaxed), expected_travel_center.x);
    ASSERT_TRUE(pending_near_entered.load(std::memory_order_acquire));
    ASSERT_FALSE(pending_near_release.load(std::memory_order_acquire));
    EXPECT_GT(current_near_dispatched.load(std::memory_order_relaxed), 0U);

    uint64_t const finished_before_far_release = finished_jobs.load(std::memory_order_relaxed);
    uint64_t const collected_before_far_release = collected_jobs.load(std::memory_order_relaxed);
    size_t const observations_begin = client.messages.size();
    late_far_release.store(true, std::memory_order_release);
    ASSERT_TRUE(poll_until([&] {
        return finished_jobs.load(std::memory_order_relaxed) > finished_before_far_release
            && collected_jobs.load(std::memory_order_relaxed) > collected_before_far_release;
    }, deadline));

    auto const observation_deadline = std::min(
        deadline,
        std::chrono::steady_clock::now() + std::chrono::milliseconds{250}
    );
    static_cast<void>(poll_until([&] {
        auto const batches = deliveryBatchesFrom(client.messages, observations_begin);
        return std::ranges::any_of(batches, [&late_far_key](auto const& batch) {
            return std::ranges::any_of(batch.tiles, [&late_far_key](auto const& tile) {
                return tile.key == late_far_key;
            });
        });
    }, observation_deadline));
    std::vector<shared::ServerHeightTileBatchMessage> const observed_batches = deliveryBatchesFrom(
        client.messages,
        observations_begin
    );
    bool outside_current_near_admitted = false;
    shared::HeightTileKey first_outside_current_near_key{};
    for (shared::ServerHeightTileBatchMessage const& batch : observed_batches) {
        for (shared::ServerHeightTileMessage const& tile : batch.tiles) {
            bool const is_current_near =
                std::abs(tile.key.x - expected_travel_center.x) <= CURRENT_NEAR_WINDOW_RADIUS
                && std::abs(tile.key.y - expected_travel_center.y) <= CURRENT_NEAR_WINDOW_RADIUS;
            if (!is_current_near && !outside_current_near_admitted) {
                outside_current_near_admitted = true;
                first_outside_current_near_key = tile.key;
            }
        }
    }
    EXPECT_FALSE(outside_current_near_admitted)
        << "tile (" << first_outside_current_near_key.x << ',' << first_outside_current_near_key.y
        << ") outside the current 225-tile window was admitted while nearer work was dispatched";
    EXPECT_FALSE(pending_near_release.load(std::memory_order_acquire));
    EXPECT_GT(current_near_dispatched.load(std::memory_order_relaxed), 0U);

    pending_near_release.store(true, std::memory_order_release);
    auto const near_delivery_deadline = std::min(
        deadline,
        std::chrono::steady_clock::now() + std::chrono::seconds{3}
    );
    ASSERT_TRUE(poll_until([&] {
        return std::ranges::any_of(deliveryBatchesFrom(client.messages, observations_begin),
            [&pending_near_key](auto const& batch) {
                return std::ranges::any_of(batch.tiles, [&pending_near_key](auto const& tile) {
                    return tile.key == pending_near_key;
                });
            });
    }, near_delivery_deadline));
}

TEST(GameServerPreviewTest, RemovalBacklogDoesNotStarveCurrentNearTileDeliveries)
{
    static constexpr std::chrono::seconds TIMEOUT{15};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr uint32_t RENDER_DISTANCE = 16U;
    static constexpr uint32_t EXPECTED_TILE_COUNT = shared::heightTileInterestCount(RENDER_DISTANCE);
    static constexpr uint32_t FLIGHT_SPEEDUP = 500U;
    static constexpr uint32_t MOVEMENT_INPUT_COUNT = 3U;
    static constexpr uint32_t MAXIMUM_ADD_BATCHES = 1U;

    std::atomic_uint32_t resident_tiles{0U};
    std::atomic_uint32_t inflight_deliveries{0U};
    std::atomic_uint32_t near_ready_tiles{0U};
    server::GameServer::BenchmarkHooks const hooks{
        .on_tick = {},
        .on_preview_buffered = {},
        .on_preview_metrics = [
            &resident_tiles,
            &inflight_deliveries,
            &near_ready_tiles
        ](core::ClientId, server::GameServer::PreviewStreamMetrics const metrics) {
            resident_tiles.store(metrics.resident_tiles, std::memory_order_relaxed);
            inflight_deliveries.store(metrics.inflight_deliveries, std::memory_order_relaxed);
            near_ready_tiles.store(metrics.near_window_ready_tiles, std::memory_order_relaxed);
        },
        .before_height_tile_completion = {},
    };
    server::GameServer server{
        0,
        {},
        shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(),
        RENDER_DISTANCE,
        preparedTerrainWorld(),
    };
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested, &hooks] {
        server.run(stop_requested, &hooks);
    }};
    defer {
        stop_requested.store(true, std::memory_order_relaxed);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };
    PreviewClient client;
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@',
        .mode = shared::WorldMode::Flight,
        .wants_previews = true,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));

    auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while ((resident_tiles.load(std::memory_order_relaxed) != EXPECTED_TILE_COUNT
            || inflight_deliveries.load(std::memory_order_relaxed) != 0U)
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_EQ(resident_tiles.load(std::memory_order_relaxed), EXPECTED_TILE_COUNT);
    ASSERT_EQ(inflight_deliveries.load(std::memory_order_relaxed), 0U);

    client.setAcknowledgesDeliveries(false);
    uint32_t const first_post_travel_message = static_cast<uint32_t>(client.messages.size());
    for (uint32_t sequence = 1U; sequence <= MOVEMENT_INPUT_COUNT; ++sequence) {
        ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {
                .x = 127U,
                .y = 0U,
                .accelerated = true,
                .speedup = FLIGHT_SPEEDUP,
            },
            .sequence = sequence,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    }
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.last_acknowledged_input < MOVEMENT_INPUT_COUNT
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_EQ(client.last_acknowledged_input, MOVEMENT_INPUT_COUNT);
    shared::HeightTileKey const center{
        .x = client.last_player_x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
        .y = shared::World::FLIGHT_SPAWN.y / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
    };
    ASSERT_GT(std::abs(center.x - (shared::World::FLIGHT_SPAWN.x
        / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH))), 32);

    auto const post_travel_batch_count = [&client, first_post_travel_message] {
        return static_cast<uint32_t>(std::ranges::count_if(
            std::span<shared::Message const>{client.messages}.subspan(first_post_travel_message),
            [](shared::Message const& message) {
                return std::holds_alternative<shared::ServerHeightTileBatchMessage>(message);
            }
        ));
    };
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while ((post_travel_batch_count() == 0U
            || near_ready_tiles.load(std::memory_order_relaxed) == 0U)
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_TRUE(waitForStalledDeliveryWindow(client, first_post_travel_message));
    ASSERT_GT(near_ready_tiles.load(std::memory_order_relaxed), 0U);

    std::vector<shared::ServerHeightTileBatchMessage> const stalled_batches = deliveryBatchesFrom(
        client.messages,
        first_post_travel_message
    );
    expectBoundedDeliveryWindow(stalled_batches);
    uint32_t const maximum_pre_near_operations = (
        static_cast<uint32_t>(stalled_batches.size()) + MAXIMUM_ADD_BATCHES
    ) * shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY;
    client.setAcknowledgesDeliveries(true);
    for (shared::ServerHeightTileBatchMessage const& batch : stalled_batches) {
        ASSERT_TRUE(client.acknowledgeDelivery(batch.delivery_token));
    }

    auto const first_near_operation_count = [&client, first_post_travel_message, center] {
        uint32_t operations = 0U;
        for (uint32_t index = first_post_travel_message;
             index < static_cast<uint32_t>(client.messages.size());
             ++index) {
            shared::Message const& message = client.messages[index];
            if (auto const* const batch = std::get_if<shared::ServerHeightTileBatchMessage>(&message)) {
                operations += static_cast<uint32_t>(batch->removals.size());
                uint32_t additions = 0U;
                for (shared::ServerHeightTileMessage const& tile : batch->tiles) {
                    ++additions;
                    if (shared::heightTileGenerationBand(center, 127, 0, tile.key)
                        <= shared::HeightTileGenerationBand::Near) {
                        return std::optional<uint32_t>{operations + additions};
                    }
                }
                operations += additions;
            }
        }
        return std::optional<uint32_t>{};
    };
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    std::optional<uint32_t> near_operation_count = first_near_operation_count();
    while (!near_operation_count.has_value() && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
        near_operation_count = first_near_operation_count();
    }

    ASSERT_TRUE(near_operation_count.has_value());
    EXPECT_LE(*near_operation_count, maximum_pre_near_operations);
}

TEST(GameServerPreviewTest, SustainedNetworkIngressDoesNotStarveTicksTerrainOrShutdown)
{
    static constexpr auto WARMUP_TIMEOUT = std::chrono::seconds{10};
    static constexpr auto PROGRESS_TIMEOUT = std::chrono::seconds{2};
    static constexpr auto SHUTDOWN_TIMEOUT = std::chrono::milliseconds{250};
    static constexpr auto MINIMUM_INGRESS_DURATION = std::chrono::milliseconds{500};
    static constexpr uint32_t FLOOD_CLIENT_COUNT = 3U;
    static constexpr uint32_t MAX_FLOOD_PACKETS_PER_CLIENT = 500'000U;
    static constexpr uint32_t PACKETS_PER_FLUSH = 128U;
    server::GameServer server{
        0, {}, shared::WorldMode::Flight, shared::World::canonicalConfiguration(), 4U,
        preparedTerrainWorld(),
    };
    std::atomic_bool stop_requested{false};
    std::atomic_bool flood_stopped{false};
    std::atomic_bool server_exited{false};
    std::atomic_uint64_t tick_count{0U};
    std::atomic_uint64_t network_events_count{0U};
    std::atomic_uint64_t metrics_count{0U};
    std::atomic_uint64_t submitted_tile_jobs{0U};
    server::GameServer::BenchmarkHooks const hooks{
        .on_tick = [&tick_count, &network_events_count](std::chrono::nanoseconds, uint64_t events) {
            tick_count.fetch_add(1U, std::memory_order_relaxed);
            network_events_count.fetch_add(events, std::memory_order_relaxed);
        },
        .on_preview_buffered = {},
        .on_preview_metrics = [&metrics_count, &submitted_tile_jobs](
            core::ClientId,
            server::GameServer::PreviewStreamMetrics const metrics
        ) {
            metrics_count.fetch_add(1U, std::memory_order_relaxed);
            submitted_tile_jobs.store(metrics.height_tile_jobs.submitted_total, std::memory_order_relaxed);
        },
        .before_height_tile_completion = {},
    };
    PreviewClient observer;
    auto const flood_clients_storage = std::make_unique<std::array<PreviewClient, FLOOD_CLIENT_COUNT>>();
    std::array<PreviewClient, FLOOD_CLIENT_COUNT>& flood_clients = *flood_clients_storage;
    std::array<std::atomic_uint64_t, FLOOD_CLIENT_COUNT> sent_packets{};
    std::array<std::atomic_uint64_t, FLOOD_CLIENT_COUNT> flushed_packets{};
    std::array<std::thread, FLOOD_CLIENT_COUNT> flood_threads;
    auto const totalSentPackets = [&sent_packets] {
        uint64_t count = 0U;
        for (std::atomic_uint64_t const& sent : sent_packets) {
            count += sent.load(std::memory_order_relaxed);
        }
        return count;
    };
    auto const totalFlushedPackets = [&flushed_packets] {
        uint64_t count = 0U;
        for (std::atomic_uint64_t const& flushed : flushed_packets) {
            count += flushed.load(std::memory_order_relaxed);
        }
        return count;
    };
    std::thread server_thread{[&server, &stop_requested, &hooks, &server_exited] {
        server.run(stop_requested, &hooks);
        server_exited.store(true, std::memory_order_release);
    }};
    defer {
        flood_stopped.store(true, std::memory_order_relaxed);
        for (std::thread& flood_thread : flood_threads) {
            if (flood_thread.joinable()) {
                flood_thread.join();
            }
        }
        stop_requested.store(true, std::memory_order_relaxed);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };

    ASSERT_TRUE(observer.connect(core::Address::localhost(server.port()), WARMUP_TIMEOUT));
    ASSERT_TRUE(observer.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), 0, core::SendMode{core::SendMode::Reliable}));
    auto deadline = std::chrono::steady_clock::now() + WARMUP_TIMEOUT;
    while (observer.received_height_tiles == 0U && std::chrono::steady_clock::now() < deadline) {
        observer.poll(std::chrono::milliseconds{1});
    }
    ASSERT_GT(observer.received_height_tiles, 0U);
    ASSERT_GT(metrics_count.load(std::memory_order_relaxed), 0U);

    for (PreviewClient& flood_client : flood_clients) {
        ASSERT_TRUE(flood_client.connect(core::Address::localhost(server.port()), WARMUP_TIMEOUT));
    }
    auto const flood_packet = shared::encodeMessage(shared::ClientHeightTileCreditMessage{
        .world_revision = 1U,
        .delivery_token = 0xA'11CEU,
        .credits = 1U,
        .not_resident_keys = {},
    });
    auto const flood_client_loop = [&flood_stopped, &flood_packet](
        PreviewClient& client,
        std::atomic_uint64_t& sent_count,
        std::atomic_uint64_t& flushed_count
    ) {
        while (!flood_stopped.load(std::memory_order_relaxed)
            && sent_count.load(std::memory_order_relaxed) < MAX_FLOOD_PACKETS_PER_CLIENT) {
            uint32_t batch_count = 0U;
            while (batch_count < PACKETS_PER_FLUSH
                && !flood_stopped.load(std::memory_order_relaxed)
                && sent_count.load(std::memory_order_relaxed) < MAX_FLOOD_PACKETS_PER_CLIENT) {
                if (client.send(flood_packet, shared::GAME_CHANNEL,
                        core::SendMode{core::SendMode::Unsequenced})) {
                    sent_count.fetch_add(1U, std::memory_order_relaxed);
                    ++batch_count;
                } else {
                    break;
                }
            }
            client.flush();
            flushed_count.store(sent_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    };
    for (uint32_t index = 0U; index < FLOOD_CLIENT_COUNT; ++index) {
        flood_threads[index] = std::thread{
            [client = &flood_clients[index], sent_count = &sent_packets[index],
                flushed_count = &flushed_packets[index], flood_client_loop] {
                flood_client_loop(*client, *sent_count, *flushed_count);
            }
        };
    }

    deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (totalSentPackets() < 1'000U && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    uint32_t const baseline_tiles = observer.received_height_tiles;
    uint64_t const baseline_ticks = tick_count.load(std::memory_order_relaxed);
    uint64_t const baseline_metrics = metrics_count.load(std::memory_order_relaxed);
    uint64_t const baseline_jobs = submitted_tile_jobs.load(std::memory_order_relaxed);
    auto const flood_started_at = std::chrono::steady_clock::now();
    for (uint32_t sequence = 1U; sequence <= 8U; ++sequence) {
        ASSERT_TRUE(observer.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {.x = 127U, .y = 0U, .accelerated = true, .speedup = 500U},
            .sequence = sequence,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    }

    bool made_progress_under_ingress = false;
    deadline = std::chrono::steady_clock::now() + PROGRESS_TIMEOUT;
    while (std::chrono::steady_clock::now() < deadline) {
        observer.poll(std::chrono::milliseconds{1});
        bool const tick_progress = tick_count.load(std::memory_order_relaxed) >= baseline_ticks + 2U;
        bool const metrics_progress = metrics_count.load(std::memory_order_relaxed) > baseline_metrics;
        bool const terrain_progress = observer.received_height_tiles > baseline_tiles
            || submitted_tile_jobs.load(std::memory_order_relaxed) > baseline_jobs;
        if (tick_progress && metrics_progress && terrain_progress) {
            made_progress_under_ingress = true;
        }
        if (made_progress_under_ingress
            && std::chrono::steady_clock::now() - flood_started_at >= MINIMUM_INGRESS_DURATION) {
            break;
        }
    }
    uint64_t const flushed_packets_before_shutdown = totalFlushedPackets();
    deadline = std::chrono::steady_clock::now() + PROGRESS_TIMEOUT;
    while (totalFlushedPackets() == flushed_packets_before_shutdown
        && std::chrono::steady_clock::now() < deadline) {
        observer.poll(std::chrono::milliseconds{1});
    }
    bool const ingress_active_at_shutdown = totalFlushedPackets() > flushed_packets_before_shutdown;
    stop_requested.store(true, std::memory_order_relaxed);
    deadline = std::chrono::steady_clock::now() + SHUTDOWN_TIMEOUT;
    while (!server_exited.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        observer.poll(std::chrono::milliseconds{1});
    }
    bool const stopped_during_ingress = ingress_active_at_shutdown
        && server_exited.load(std::memory_order_acquire);
    flood_stopped.store(true, std::memory_order_relaxed);
    for (std::thread& flood_thread : flood_threads) {
        if (flood_thread.joinable()) {
            flood_thread.join();
        }
    }
    if (!server_exited.load(std::memory_order_acquire)) {
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!server_exited.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    ASSERT_TRUE(server_exited.load(std::memory_order_acquire));
    server_thread.join();

    EXPECT_GE(totalSentPackets(), 1'000U);
    EXPECT_GT(network_events_count.load(std::memory_order_relaxed), 0U);
    EXPECT_TRUE(made_progress_under_ingress);
    EXPECT_TRUE(stopped_during_ingress);
}

TEST(GameServerPreviewTest, CompactTerrainUsesAvailableCreditsWithinTheGameplayByteReserve)
{
    static constexpr std::chrono::seconds TIMEOUT{2};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr uint32_t MAXIMUM_BATCH_BYTES = 10'118U;
    static constexpr uint32_t MAXIMUM_OUTSTANDING_BYTES = 34'356U;
    static constexpr uint32_t RENDER_DISTANCE = 32U;
    static constexpr server::GameServer::SpawnPoint SPAWN{
        .character = '@', .x = 0, .y = 0, .z = 400,
    };

    server::GameServer server{
        0U, {SPAWN}, shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(), RENDER_DISTANCE, preparedTerrainWorld(),
    };
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested] { server.run(stop_requested); }};
    defer {
        stop_requested.store(true, std::memory_order_relaxed);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };
    PreviewClient client{false};
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    ASSERT_TRUE(waitForStalledDeliveryWindow(client));
    std::vector<shared::ServerHeightTileBatchMessage> const batches = deliveryBatchesFrom(client.messages, 0U);
    expectBoundedDeliveryWindow(batches);
    ASSERT_LT(batches.size(), shared::HEIGHT_TILE_DELIVERY_WINDOW);
    uint32_t outstanding_bytes = 0U;
    for (shared::ServerHeightTileBatchMessage const& batch : batches) {
        uint32_t const bytes = static_cast<uint32_t>(shared::encodeMessage(batch).size());
        EXPECT_LE(bytes, MAXIMUM_BATCH_BYTES);
        outstanding_bytes += bytes;
    }
    EXPECT_LE(outstanding_bytes, MAXIMUM_OUTSTANDING_BYTES);
    ASSERT_FALSE(batches.front().tiles.empty());
    shared::ServerHeightTileBatchMessage const next_tile_batch{
        .delivery_token = 1U,
        .tiles = { batches.front().tiles.front() },
        .removals = {},
    };
    EXPECT_GT(outstanding_bytes + shared::encodeMessage(next_tile_batch).size(), MAXIMUM_OUTSTANDING_BYTES);

    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = {}, .sequence = 1U,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.last_acknowledged_input != 1U && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    EXPECT_EQ(client.last_acknowledged_input, 1U);
    EXPECT_EQ(deliveryBatchCount(client.messages), batches.size());
}

TEST(GameServerPreviewTest, StalledClientReservesGameplayCapacityAndResumesOneAcknowledgedBatch)
{
    static constexpr auto DURATION = std::chrono::seconds{2};
    static constexpr std::chrono::milliseconds CREDIT_OBSERVATION_DURATION{100};
    server::GameServer server{
        0,
        {},
        shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(),
        shared::HEIGHT_TILE_INTEREST_RADIUS,
        preparedTerrainWorld(),
    };
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested] { server.run(stop_requested); }};
    defer {
        stop_requested.store(true, std::memory_order_relaxed);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };
    PreviewClient client{false, false};
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), std::chrono::seconds{2}));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), 0, core::SendMode{core::SendMode::Reliable}));
    auto deadline = std::chrono::steady_clock::now() + DURATION;
    while (std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds::zero());
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    ASSERT_TRUE(std::ranges::any_of(client.messages, [](shared::Message const& message) {
        return std::holds_alternative<shared::ServerHeightTileDescriptorMessage>(message);
    }));
    EXPECT_EQ(deliveryBatchCount(client.messages), 0U);
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientHeightTileCreditMessage{
        .world_revision = 1U,
        .delivery_token = 0xA'11CEU,
        .credits = 1U,
        .not_resident_keys = {},
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = {.x = 127U, .y = 0U}, .sequence = 1U,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    deadline = std::chrono::steady_clock::now() + DURATION;
    while (client.last_acknowledged_input < 1U && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    EXPECT_EQ(client.last_acknowledged_input, 1U);
    deadline = std::chrono::steady_clock::now() + CREDIT_OBSERVATION_DURATION;
    while (std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    EXPECT_EQ(deliveryBatchCount(client.messages), 0U);
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientHeightTileCreditMessage{
        .world_revision = 1U,
        .delivery_token = 0U,
        .credits = shared::HEIGHT_TILE_DELIVERY_WINDOW,
        .not_resident_keys = {},
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    ASSERT_TRUE(waitForStalledDeliveryWindow(client));
    std::vector<shared::ServerHeightTileBatchMessage> const stalled_batches = deliveryBatchesFrom(client.messages, 0U);
    expectBoundedDeliveryWindow(stalled_batches);
    ASSERT_FALSE(stalled_batches.empty());
    uint32_t const stalled_batch_count = static_cast<uint32_t>(stalled_batches.size());
    ASSERT_FALSE(stalled_batches.front().tiles.empty());
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = {.x = 127U, .y = 0U}, .sequence = 2U,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    deadline = std::chrono::steady_clock::now() + DURATION;
    while (client.last_acknowledged_input < 2U && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    EXPECT_EQ(client.last_acknowledged_input, 2U);
    EXPECT_EQ(deliveryBatchCount(client.messages), stalled_batch_count);
    ASSERT_TRUE(client.acknowledgeDelivery(stalled_batches.front().delivery_token));
    deadline = std::chrono::steady_clock::now() + DURATION;
    while (deliveryBatchCount(client.messages) < stalled_batch_count + 1U
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    ASSERT_TRUE(waitForStalledDeliveryWindow(client));
    EXPECT_GE(deliveryBatchCount(client.messages), stalled_batch_count + 1U);
    EXPECT_LE(deliveryBatchCount(client.messages), shared::HEIGHT_TILE_DELIVERY_WINDOW + 1U);
    std::vector<shared::ServerHeightTileBatchMessage> outstanding_batches = deliveryBatchesFrom(client.messages, 0U);
    std::erase_if(outstanding_batches, [&](shared::ServerHeightTileBatchMessage const& batch) {
        return batch.delivery_token == stalled_batches.front().delivery_token;
    });
    expectBoundedDeliveryWindow(outstanding_batches);
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    EXPECT_LE(deliveryBatchCount(client.messages), shared::HEIGHT_TILE_DELIVERY_WINDOW + 1U);
    EXPECT_LE(
        heightTileCount(client.messages),
        (shared::HEIGHT_TILE_DELIVERY_WINDOW + 1U) * shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY
    );
}

TEST(GameServerPreviewTest, StalledCreditsBoundPrefetchAndResumeTerrainProgress)
{
    static constexpr std::chrono::seconds TIMEOUT{5};
    static constexpr std::chrono::seconds STALL_DURATION{2};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr uint32_t MAXIMUM_BUFFERED_TILES = 128U;
    static constexpr uint32_t RECOVERED_TILE_COUNT = 512U;

    uint32_t maximum_buffered_tiles = 0U;
    server::GameServer::BenchmarkHooks const hooks{
        .on_tick = {},
        .on_preview_buffered = [&maximum_buffered_tiles](core::ClientId, uint32_t const buffered_tiles) {
            maximum_buffered_tiles = std::max(maximum_buffered_tiles, buffered_tiles);
        },
        .on_preview_metrics = {},
        .before_height_tile_completion = {},
    };
    server::GameServer server{
        0,
        {},
        shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(),
        shared::HEIGHT_TILE_INTEREST_RADIUS,
        preparedTerrainWorld(),
    };
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested, &hooks] {
        server.run(stop_requested, &hooks);
    }};
    PreviewClient client{false};
    bool const connected = client.connect(core::Address::localhost(server.port()), TIMEOUT);
    bool const joined = connected && client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable});
    auto const stalled_deadline = std::chrono::steady_clock::now() + STALL_DURATION;
    while (joined && std::chrono::steady_clock::now() < stalled_deadline) {
        client.poll(POLL_INTERVAL);
    }
    bool const stalled_window_settled = joined && waitForStalledDeliveryWindow(client);
    uint32_t const stalled_tile_count = client.received_height_tiles;
    std::vector<shared::ServerHeightTileBatchMessage> const stalled_batches = deliveryBatchesFrom(
        client.messages, 0U
    );
    client.setAcknowledgesDeliveries(true);
    bool acknowledged_all = true;
    for (shared::ServerHeightTileBatchMessage const& batch : stalled_batches) {
        acknowledged_all = client.acknowledgeDelivery(batch.delivery_token) && acknowledged_all;
    }
    auto const recovery_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (joined && client.received_height_tiles < RECOVERED_TILE_COUNT
        && std::chrono::steady_clock::now() < recovery_deadline) {
        client.poll(POLL_INTERVAL);
    }
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    ASSERT_TRUE(connected);
    ASSERT_TRUE(joined);
    ASSERT_TRUE(stalled_window_settled);
    ASSERT_TRUE(acknowledged_all);
    expectBoundedDeliveryWindow(stalled_batches);
    EXPECT_LE(
        stalled_tile_count,
        shared::HEIGHT_TILE_DELIVERY_WINDOW * shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY
    );
    EXPECT_EQ(maximum_buffered_tiles, MAXIMUM_BUFFERED_TILES);
    EXPECT_GE(client.received_height_tiles, RECOVERED_TILE_COUNT);
}

void verifyRejectedResidencyReceipt(bool const replay_acknowledgement)
{
    static constexpr auto TIMEOUT = std::chrono::seconds{8};
    static constexpr auto POLL_INTERVAL = std::chrono::milliseconds{1};
    std::atomic_uint64_t rejected_acknowledgements{0U};
    std::atomic_uint64_t accepted_acknowledgements{0U};
    std::atomic_uint32_t resident_tiles{0U};
    server::GameServer::BenchmarkHooks const hooks{
        .on_tick = {},
        .on_preview_buffered = {},
        .on_preview_metrics = [&](core::ClientId, server::GameServer::PreviewStreamMetrics const metrics) {
            resident_tiles.store(metrics.resident_tiles, std::memory_order_relaxed);
            accepted_acknowledgements.store(metrics.delivery_credit_samples, std::memory_order_relaxed);
            rejected_acknowledgements.store(metrics.rejected_delivery_acknowledgements, std::memory_order_release);
        },
        .before_height_tile_completion = {},
    };
    server::GameServer server{
        0,
        {},
        shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(),
        shared::HEIGHT_TILE_INTEREST_RADIUS,
        preparedTerrainWorld(),
    };
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&] { server.run(stop_requested, &hooks); }};
    defer {
        stop_requested.store(true, std::memory_order_relaxed);
        server_thread.join();
    };
    PreviewClient client{false};
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    std::vector<shared::ServerHeightTileBatchMessage> batches;
    while (std::ranges::none_of(batches, [](auto const& batch) { return !batch.tiles.empty(); })
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
        batches = deliveryBatchesFrom(client.messages, 0U);
    }
    auto const delivery = std::ranges::find_if(batches, [](auto const& batch) { return !batch.tiles.empty(); });
    ASSERT_NE(delivery, batches.end());
    auto const& batch = *delivery;
    if (replay_acknowledgement) {
        ASSERT_TRUE(client.acknowledgeDelivery(batch.delivery_token));
        ASSERT_TRUE(client.acknowledgeDelivery(batch.delivery_token));
    } else {
        shared::HeightTileKey const unrelated_key = shared::normalizeHeightTileKey({
            .x = 1'000'000, .y = 1'000'000,
        });
        ASSERT_TRUE(client.acknowledgeDelivery(batch.delivery_token, { unrelated_key }));
    }
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (rejected_acknowledgements.load(std::memory_order_acquire) == 0U
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_EQ(rejected_acknowledgements.load(std::memory_order_acquire), 1U);
    EXPECT_EQ(accepted_acknowledgements.load(std::memory_order_relaxed), replay_acknowledgement ? 1U : 0U);
    EXPECT_EQ(resident_tiles.load(std::memory_order_relaxed), replay_acknowledgement ? batch.tiles.size() : 0U);
    if (!replay_acknowledgement) {
        ASSERT_TRUE(client.acknowledgeDelivery(batch.delivery_token));
        deadline = std::chrono::steady_clock::now() + TIMEOUT;
        while (accepted_acknowledgements.load(std::memory_order_relaxed) == 0U
            && std::chrono::steady_clock::now() < deadline) {
            client.poll(POLL_INTERVAL);
        }
        EXPECT_EQ(accepted_acknowledgements.load(std::memory_order_relaxed), 1U);
    }
}

TEST(GameServerPreviewTest, ReplayedResidencyAcknowledgementDoesNotApplyTwice)
{
    verifyRejectedResidencyReceipt(true);
}

TEST(GameServerPreviewTest, UnrelatedResidencyReceiptKeyDoesNotConsumeDelivery)
{
    verifyRejectedResidencyReceipt(false);
}

TEST(GameServerPreviewTest, ClientResidencyReceiptRegeneratesAnEvictedDesiredTile)
{
    static constexpr auto TIMEOUT = std::chrono::seconds{8};
    static constexpr auto POLL_INTERVAL = std::chrono::milliseconds{1};
    server::GameServer server{
        0,
        {},
        shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(),
        shared::HEIGHT_TILE_INTEREST_RADIUS,
        preparedTerrainWorld(),
    };
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested] { server.run(stop_requested); }};
    defer {
        stop_requested.store(true, std::memory_order_relaxed);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };
    PreviewClient client;
    client.reportFirstDeliveryTileAsEvicted();
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));

    auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.received_height_tile_descriptors == 0U
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_EQ(client.received_height_tile_descriptors, 1U);

    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    std::vector<shared::ServerHeightTileBatchMessage> batches;
    while (batches.empty() && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
        batches = deliveryBatchesFrom(client.messages, 0U);
    }
    ASSERT_FALSE(batches.empty());
    auto const first_batch = std::ranges::find_if(batches, [](auto const& batch) {
        return !batch.tiles.empty();
    });
    ASSERT_NE(first_batch, batches.end());
    shared::HeightTileKey const evicted_key = first_batch->tiles.front().key;
    uint64_t const first_delivery_token = first_batch->delivery_token;
    ASSERT_TRUE(client.acknowledgeDelivery(first_delivery_token, { evicted_key }));

    bool regenerated = false;
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!regenerated && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
        for (shared::ServerHeightTileBatchMessage const& batch : deliveryBatchesFrom(client.messages, 0U)) {
            if (batch.delivery_token != first_delivery_token
                && std::ranges::any_of(batch.tiles, [evicted_key](auto const& tile) {
                    return tile.key == evicted_key;
                })) {
                regenerated = true;
                break;
            }
        }
    }

    EXPECT_TRUE(regenerated);
}

TEST(GameServerPreviewTest, ReversalCancelsQueuedRemovalsForRestoredInterest)
{
    static constexpr auto TIMEOUT = std::chrono::seconds{10};
    static constexpr uint32_t INITIAL_TILES = 512U;
    static constexpr uint16_t FLIGHT_SPEEDUP = 500U;
    static constexpr uint32_t MOVEMENT_INPUT_COUNT = (
        shared::HEIGHT_TILE_INTEREST_RADIUS * shared::HEIGHT_TILE_SIDE_LENGTH * shared::SUBCELLS_PER_CELL
    ) / (shared::MOVEMENT_SUBCELLS_PER_TICK * FLIGHT_SPEEDUP) + 1U;
    static constexpr uint32_t FINAL_INPUT_SEQUENCE = 2U * MOVEMENT_INPUT_COUNT;
    server::GameServer server{
        0,
        {},
        shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(),
        shared::HEIGHT_TILE_INTEREST_RADIUS,
        preparedTerrainWorld(),
    };
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested] { server.run(stop_requested); }};
    defer {
        stop_requested.store(true, std::memory_order_relaxed);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };
    PreviewClient client;
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), 0, core::SendMode{core::SendMode::Reliable}));

    auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.received_height_tiles < INITIAL_TILES
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    ASSERT_GE(client.received_height_tiles, INITIAL_TILES);

    client.setAcknowledgesDeliveries(false);
    size_t const stalled_message = client.messages.size();
    ASSERT_TRUE(waitForStalledDeliveryWindow(client, static_cast<uint32_t>(stalled_message)));
    std::vector<shared::ServerHeightTileBatchMessage> const stalled_batches = deliveryBatchesFrom(
        client.messages,
        stalled_message
    );
    expectBoundedDeliveryWindow(stalled_batches);

    for (uint32_t sequence = 1U; sequence <= MOVEMENT_INPUT_COUNT; ++sequence) {
        ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {.x = 127U, .y = 0U, .accelerated = true, .speedup = FLIGHT_SPEEDUP},
            .sequence = sequence,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    }
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.last_acknowledged_input < MOVEMENT_INPUT_COUNT && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    ASSERT_EQ(client.last_acknowledged_input, MOVEMENT_INPUT_COUNT);
    for (uint32_t sequence = MOVEMENT_INPUT_COUNT + 1U; sequence <= FINAL_INPUT_SEQUENCE; ++sequence) {
        ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {
                .x = static_cast<uint8_t>(-127),
                .y = 0U,
                .accelerated = true,
                .speedup = FLIGHT_SPEEDUP,
            },
            .sequence = sequence,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    }
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.last_acknowledged_input < FINAL_INPUT_SEQUENCE && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    ASSERT_EQ(client.last_acknowledged_input, FINAL_INPUT_SEQUENCE);

    shared::HeightTileInterest const restored_interest = shared::makeHeightTileInterest({
        .x = client.last_player_x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
        .y = shared::World::FLIGHT_SPAWN.y / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
    }, -127, 0);
    size_t const resumed_message = client.messages.size();
    client.setAcknowledgesDeliveries(true);
    for (shared::ServerHeightTileBatchMessage const& batch : stalled_batches) {
        ASSERT_TRUE(client.acknowledgeDelivery(batch.delivery_token));
    }
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    std::vector<shared::ServerHeightTileBatchMessage> const resumed_batches = deliveryBatchesFrom(
        client.messages,
        resumed_message
    );
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    for (shared::ServerHeightTileBatchMessage const& batch : resumed_batches) {
        for (shared::ServerRemoveHeightTileMessage const& removal : batch.removals) {
            EXPECT_EQ(
                std::ranges::find(restored_interest.keys, removal.key),
                restored_interest.keys.end()
            );
        }
    }
}

TEST(GameServerPreviewTest, SaturatedAdditionsDoNotStarveRemovalsAndInflightReclaimsRecover)
{
    static constexpr auto TIMEOUT = std::chrono::seconds{12};
    static constexpr uint32_t MAXIMUM_PACKET_BYTES = 10'118U;
    static constexpr uint32_t MAXIMUM_WINDOW_BYTES = 34'356U;
    static constexpr uint32_t INITIAL_TILES = 1'024U;
    static constexpr uint32_t MINIMUM_REMOVALS_PER_MIXED_BATCH =
        shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY / 2U;
    static constexpr uint16_t FLIGHT_SPEEDUP = 500U;
    static constexpr uint32_t MOVEMENT_INPUT_COUNT = (
        shared::HEIGHT_TILE_INTEREST_RADIUS * shared::HEIGHT_TILE_SIDE_LENGTH * shared::SUBCELLS_PER_CELL
    ) / (shared::MOVEMENT_SUBCELLS_PER_TICK * FLIGHT_SPEEDUP) + 1U;
    static constexpr uint32_t FINAL_INPUT_SEQUENCE = 2U * MOVEMENT_INPUT_COUNT;
    EXPECT_FALSE(meetsHeightTileRemovalQuota(0U, 9'565U, 10'118U));
    EXPECT_FALSE(meetsHeightTileRemovalQuota(0U, 9'565U, 9'589U));
    EXPECT_TRUE(meetsHeightTileRemovalQuota(0U, 9'565U, 9'585U));
    EXPECT_TRUE(meetsHeightTileRemovalProgress(0U, 1U, 9'565U, 9'585U));
    EXPECT_FALSE(meetsHeightTileRemovalProgress(0U, 1U, 9'565U, 9'589U));
    server::GameServer server{
        0,
        {},
        shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(),
        shared::HEIGHT_TILE_INTEREST_RADIUS,
        preparedTerrainWorld(),
    };
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested] { server.run(stop_requested); }};
    defer {
        stop_requested.store(true, std::memory_order_relaxed);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };
    PreviewClient client;
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), 0, core::SendMode{core::SendMode::Reliable}));

    auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.received_height_tiles < INITIAL_TILES
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    ASSERT_GE(client.received_height_tiles, INITIAL_TILES);

    client.setAcknowledgesDeliveries(false);
    size_t const stalled_message = client.messages.size();
    ASSERT_TRUE(waitForStalledDeliveryWindow(client, static_cast<uint32_t>(stalled_message)));
    std::vector<shared::ServerHeightTileBatchMessage> const stalled_batches = deliveryBatchesFrom(
        client.messages,
        stalled_message
    );
    expectBoundedDeliveryWindow(stalled_batches);
    ASSERT_GE(stalled_batches.size(), 2U);

    for (uint32_t sequence = 1U; sequence <= MOVEMENT_INPUT_COUNT; ++sequence) {
        ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {.x = 127U, .y = 0U, .accelerated = true, .speedup = FLIGHT_SPEEDUP},
            .sequence = sequence,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    }
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.last_acknowledged_input < MOVEMENT_INPUT_COUNT && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    ASSERT_EQ(client.last_acknowledged_input, MOVEMENT_INPUT_COUNT);
    size_t const outward_message = client.messages.size();
    std::vector<shared::ServerHeightTileBatchMessage> const before_first_ack_batches = deliveryBatchesFrom(
        client.messages, stalled_message
    );
    ASSERT_TRUE(client.acknowledgeDelivery(stalled_batches.front().delivery_token));
    std::vector<uint64_t> acknowledged_tokens{ stalled_batches.front().delivery_token };
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (deliveryBatchesFrom(client.messages, outward_message).empty()
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    ASSERT_TRUE(waitForStalledDeliveryWindow(client, static_cast<uint32_t>(outward_message)));
    std::vector<shared::ServerHeightTileBatchMessage> outward_batches = deliveryBatchesFrom(
        client.messages,
        outward_message
    );
    ASSERT_FALSE(outward_batches.empty());
    expectAcknowledgedDeliveryWindow(deliveryBatchesFrom(client.messages, stalled_message), acknowledged_tokens);
    EXPECT_LE(stalled_batches.size() + outward_batches.size(), shared::HEIGHT_TILE_DELIVERY_WINDOW + 1U);
    uint32_t const first_ack_batch_count = static_cast<uint32_t>(outward_batches.size());
    std::vector<shared::ServerHeightTileBatchMessage> const before_second_ack_batches = deliveryBatchesFrom(
        client.messages, stalled_message
    );
    ASSERT_TRUE(client.acknowledgeDelivery(outward_batches.front().delivery_token));
    acknowledged_tokens.push_back(outward_batches.front().delivery_token);
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (deliveryBatchesFrom(client.messages, outward_message).size() < first_ack_batch_count + 1U
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    ASSERT_TRUE(waitForStalledDeliveryWindow(client, static_cast<uint32_t>(outward_message)));
    outward_batches = deliveryBatchesFrom(client.messages, outward_message);
    ASSERT_GE(outward_batches.size(), first_ack_batch_count + 1U);
    expectAcknowledgedDeliveryWindow(deliveryBatchesFrom(client.messages, stalled_message), acknowledged_tokens);
    EXPECT_LE(stalled_batches.size() + outward_batches.size(), shared::HEIGHT_TILE_DELIVERY_WINDOW + 2U);

    shared::HeightTileKey const outward_center{
        .x = client.last_player_x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
        .y = shared::World::FLIGHT_SPAWN.y / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
    };
    std::vector<shared::HeightTileKey> outward_removal_keys;
    uint32_t outward_removals = 0U;
    uint64_t first_ack_inflight_bytes = unacknowledgedHeightTileBytes(
        before_first_ack_batches, {stalled_batches.front().delivery_token}
    );
    uint64_t second_ack_inflight_bytes = unacknowledgedHeightTileBytes(
        before_second_ack_batches, acknowledged_tokens
    );
    uint32_t outward_batch_index = 0U;
    for (shared::ServerHeightTileBatchMessage const& batch : outward_batches) {
        uint64_t& phase_inflight_bytes = outward_batch_index < first_ack_batch_count
            ? first_ack_inflight_bytes : second_ack_inflight_bytes;
        ASSERT_LE(phase_inflight_bytes, MAXIMUM_WINDOW_BYTES);
        uint64_t const available_bytes = std::min<uint64_t>(
            MAXIMUM_PACKET_BYTES, MAXIMUM_WINDOW_BYTES - phase_inflight_bytes
        );
        uint64_t const encoded_bytes = shared::encodeMessage(batch).size();
        EXPECT_LE(encoded_bytes, available_bytes);
        EXPECT_TRUE(meetsHeightTileRemovalProgress(
            static_cast<uint32_t>(batch.removals.size()),
            static_cast<uint32_t>(batch.tiles.size()),
            encoded_bytes,
            available_bytes
        )) << "An outward batch must make removal progress whenever another removal fits both byte budgets.";
        phase_inflight_bytes += encoded_bytes;
        ++outward_batch_index;
        for (shared::ServerRemoveHeightTileMessage const& removal : batch.removals) {
            EXPECT_FALSE(shared::heightTileWithinInterest(
                outward_center, removal.key, shared::HEIGHT_TILE_INTEREST_RADIUS
            ));
            EXPECT_EQ(std::ranges::find(outward_removal_keys, removal.key), outward_removal_keys.end());
            outward_removal_keys.push_back(removal.key);
            ++outward_removals;
        }
    }
    ASSERT_GE(outward_removals, 2U * MINIMUM_REMOVALS_PER_MIXED_BATCH);

    std::vector<shared::ServerHeightTileBatchMessage> const before_late_ack_batches = deliveryBatchesFrom(
        client.messages, stalled_message
    );
    uint32_t const late_ack_message = static_cast<uint32_t>(client.messages.size());
    ASSERT_TRUE(client.acknowledgeDelivery(stalled_batches[1U].delivery_token));
    acknowledged_tokens.push_back(stalled_batches[1U].delivery_token);
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (deliveryBatchesFrom(client.messages, late_ack_message).empty()
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    ASSERT_TRUE(waitForStalledDeliveryWindow(client, late_ack_message));
    std::vector<shared::ServerHeightTileBatchMessage> const late_ack_batches = deliveryBatchesFrom(
        client.messages,
        late_ack_message
    );
    ASSERT_FALSE(late_ack_batches.empty());
    expectAcknowledgedDeliveryWindow(deliveryBatchesFrom(client.messages, stalled_message), acknowledged_tokens);
    EXPECT_LE(
        stalled_batches.size() + outward_batches.size() + late_ack_batches.size(),
        shared::HEIGHT_TILE_DELIVERY_WINDOW + 3U
    );
    uint64_t inflight_bytes = unacknowledgedHeightTileBytes(before_late_ack_batches, acknowledged_tokens);
    for (shared::ServerHeightTileBatchMessage const& batch : late_ack_batches) {
        ASSERT_LE(inflight_bytes, MAXIMUM_WINDOW_BYTES);
        uint64_t const available_bytes = std::min<uint64_t>(
            MAXIMUM_PACKET_BYTES, MAXIMUM_WINDOW_BYTES - inflight_bytes
        );
        uint64_t const encoded_bytes = shared::encodeMessage(batch).size();
        EXPECT_LE(encoded_bytes, available_bytes);
        EXPECT_TRUE(meetsHeightTileRemovalQuota(
            static_cast<uint32_t>(batch.removals.size()), encoded_bytes, available_bytes
        )) << "A resumed batch must meet the removal quota whenever another removal fits both byte budgets.";
        inflight_bytes += encoded_bytes;
        for (shared::ServerRemoveHeightTileMessage const& removal : batch.removals) {
            EXPECT_FALSE(shared::heightTileWithinInterest(
                outward_center, removal.key, shared::HEIGHT_TILE_INTEREST_RADIUS
            ));
            EXPECT_EQ(std::ranges::find(outward_removal_keys, removal.key), outward_removal_keys.end());
            outward_removal_keys.push_back(removal.key);
        }
    }
    outward_batches.insert(outward_batches.end(), late_ack_batches.begin(), late_ack_batches.end());

    for (uint32_t sequence = MOVEMENT_INPUT_COUNT + 1U; sequence <= FINAL_INPUT_SEQUENCE; ++sequence) {
        ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {
                .x = static_cast<uint8_t>(-127),
                .y = 0U,
                .accelerated = true,
                .speedup = FLIGHT_SPEEDUP,
            },
            .sequence = sequence,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    }
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.last_acknowledged_input < FINAL_INPUT_SEQUENCE && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    ASSERT_EQ(client.last_acknowledged_input, FINAL_INPUT_SEQUENCE);
    shared::HeightTileInterest const restored_interest = shared::makeHeightTileInterest({
        .x = client.last_player_x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
        .y = shared::World::FLIGHT_SPAWN.y / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
    }, -127, 0);
    std::vector<shared::HeightTileKey> reclaimed_keys;
    for (shared::ServerHeightTileBatchMessage const& batch : outward_batches) {
        for (shared::ServerRemoveHeightTileMessage const& removal : batch.removals) {
            if (std::ranges::find(restored_interest.keys, removal.key) != restored_interest.keys.end()) {
                reclaimed_keys.push_back(removal.key);
            }
        }
    }
    ASSERT_FALSE(reclaimed_keys.empty());

    // Let the restored-interest cursor pass resident keys before the already-sent removal is acknowledged.
    auto const reentry_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{250};
    while (std::chrono::steady_clock::now() < reentry_deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    size_t const recovery_message = client.messages.size();
    client.setAcknowledgesDeliveries(true);
    // Movement can admit removal-only batches between the captured stalled and outward windows.
    for (shared::ServerHeightTileBatchMessage const& batch : deliveryBatchesFrom(client.messages, stalled_message)) {
        if (std::ranges::find(acknowledged_tokens, batch.delivery_token) == acknowledged_tokens.end()) {
            ASSERT_TRUE(client.acknowledgeDelivery(batch.delivery_token));
        }
    }
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    std::vector<shared::HeightTileKey> recovered;
    while (std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
        recovered = heightTileKeysFrom(client.messages, recovery_message);
        if (std::ranges::all_of(reclaimed_keys, [&recovered](shared::HeightTileKey const key) {
                return std::ranges::find(recovered, key) != recovered.end();
            })) {
            break;
        }
    }
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    for (shared::HeightTileKey const key : reclaimed_keys) {
        EXPECT_NE(std::ranges::find(recovered, key), recovered.end());
    }
}

TEST(GameServerPreviewTest, SustainedFlightKeepsInputAcknowledgementsCurrentWhileTilesStream)
{
#ifndef NDEBUG
    GTEST_SKIP() << "Four-peer full-radius latency acceptance requires an optimized Release build";
#else
    static constexpr auto DURATION = std::chrono::seconds{35};
    static constexpr auto INPUT_PERIOD = std::chrono::milliseconds{100};
    static constexpr auto PROGRESS_INTERVAL = std::chrono::seconds{1};
    static constexpr auto JOIN_TIMEOUT = std::chrono::seconds{10};
    static constexpr uint32_t MAXIMUM_ACK_LAG = 16U;
    static constexpr uint32_t MAX_CLIENT_POLL_CALLS_PER_PASS = 32U;
    server::GameServer server{
        0,
        {},
        shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(),
        shared::HEIGHT_TILE_INTEREST_RADIUS,
        preparedTerrainWorld(),
    };
    std::atomic_bool stop_requested{false};
    struct ClientProgress final {
        core::ClientId client_id = 0U;
        std::chrono::steady_clock::time_point next_log{};
        bool active = false;
    };
    std::array<ClientProgress, 4> server_progress{};
    auto const server_started = std::chrono::steady_clock::now();
    server::GameServer::BenchmarkHooks const hooks{
        .on_preview_metrics = [server_started, &server_progress](
            core::ClientId const client_id,
            server::GameServer::PreviewStreamMetrics const metrics
        ) {
            auto const progress = std::ranges::find(server_progress, client_id, &ClientProgress::client_id);
            auto const slot = progress != server_progress.end()
                ? progress
                : std::ranges::find(server_progress, false, &ClientProgress::active);
            if (slot == server_progress.end()) {
                return;
            }
            if (!slot->active) {
                slot->active = true;
                slot->client_id = client_id;
            }
            auto const now = std::chrono::steady_clock::now();
            if (now < slot->next_log) {
                return;
            }
            slot->next_log = now + PROGRESS_INTERVAL;
            auto const average_ms = [](
                std::chrono::nanoseconds const total,
                uint64_t const samples
            ) {
                return samples == 0U
                    ? int64_t{0}
                    : total.count() / static_cast<int64_t>(samples) / 1'000'000;
            };
            CORE_INFO(
                "Flight server client {} at {} ms: input received/ack/unacked/pending/materialization-failures="
                "{}/{}/{}/{}/{}, jobs pending/queued/running/finished-uncollected={}/{}/{}/{}, "
                "tile jobs submitted/started/finished/collected={}/{}/{}/{}, "
                "enqueue-submit/executor/run/collect avg/max ms={}/{}/{}/{}/{}/{}/{}/{}, "
                "world jobs submitted/started/finished/collected={}/{}/{}/{}, "
                "enqueue-submit/executor/run/collect avg/max ms={}/{}/{}/{}/{}/{}/{}/{}, "
                "ready/inflight/credits={}/{}/{}, "
                "credit samples/avg/max ms={}/{}/{}, loop interval/work/tick/pump/poll ms={}/{}/{}/{}/{}",
                client_id,
                std::chrono::duration_cast<std::chrono::milliseconds>(now - server_started).count(),
                metrics.latest_received_input_sequence,
                metrics.acknowledged_input_sequence,
                metrics.unacknowledged_input_count,
                metrics.pending_input_count,
                metrics.materialization_admission_failures,
                metrics.pending_worker_jobs,
                metrics.height_tile_jobs.executor_queued_jobs + metrics.world_generation_jobs.executor_queued_jobs,
                metrics.height_tile_jobs.running_jobs + metrics.world_generation_jobs.running_jobs,
                metrics.height_tile_jobs.completed_uncollected_jobs
                    + metrics.world_generation_jobs.completed_uncollected_jobs,
                metrics.height_tile_jobs.submitted_total,
                metrics.height_tile_jobs.started_total,
                metrics.height_tile_jobs.finished_total,
                metrics.height_tile_jobs.collected_total,
                average_ms(
                    metrics.height_tile_jobs.enqueue_to_submit_total,
                    metrics.height_tile_jobs.submitted_total
                ),
                metrics.height_tile_jobs.enqueue_to_submit_max.count() / 1'000'000,
                average_ms(metrics.height_tile_jobs.executor_queue_total, metrics.height_tile_jobs.started_total),
                metrics.height_tile_jobs.executor_queue_max.count() / 1'000'000,
                average_ms(metrics.height_tile_jobs.execution_total, metrics.height_tile_jobs.finished_total),
                metrics.height_tile_jobs.execution_max.count() / 1'000'000,
                average_ms(
                    metrics.height_tile_jobs.completion_to_collection_total,
                    metrics.height_tile_jobs.collected_total
                ),
                metrics.height_tile_jobs.completion_to_collection_max.count() / 1'000'000,
                metrics.world_generation_jobs.submitted_total,
                metrics.world_generation_jobs.started_total,
                metrics.world_generation_jobs.finished_total,
                metrics.world_generation_jobs.collected_total,
                average_ms(
                    metrics.world_generation_jobs.enqueue_to_submit_total,
                    metrics.world_generation_jobs.submitted_total
                ),
                metrics.world_generation_jobs.enqueue_to_submit_max.count() / 1'000'000,
                average_ms(
                    metrics.world_generation_jobs.executor_queue_total,
                    metrics.world_generation_jobs.started_total
                ),
                metrics.world_generation_jobs.executor_queue_max.count() / 1'000'000,
                average_ms(metrics.world_generation_jobs.execution_total, metrics.world_generation_jobs.finished_total),
                metrics.world_generation_jobs.execution_max.count() / 1'000'000,
                average_ms(
                    metrics.world_generation_jobs.completion_to_collection_total,
                    metrics.world_generation_jobs.collected_total
                ),
                metrics.world_generation_jobs.completion_to_collection_max.count() / 1'000'000,
                metrics.ready_tiles,
                metrics.inflight_deliveries,
                metrics.delivery_credits,
                metrics.delivery_credit_samples,
                average_ms(metrics.delivery_credit_total, metrics.delivery_credit_samples),
                metrics.delivery_credit_max.count() / 1'000'000,
                metrics.server_loop_interval.count() / 1'000'000,
                metrics.server_loop_work.count() / 1'000'000,
                metrics.server_tick.count() / 1'000'000,
                metrics.stream_pump.count() / 1'000'000,
                metrics.network_poll.count() / 1'000'000
            );
        },
    };
    std::thread server_thread{[&server, &stop_requested, &hooks] {
        server.run(stop_requested, &hooks);
    }};
    PreviewClient first_client{true, true, '@', false};
    PreviewClient second_client{true, true, '#', false};
    PreviewClient third_client{true, true, '$', false};
    PreviewClient fourth_client{true, true, '%', false};
    std::array<PreviewClient*, 4> const clients{
        &first_client, &second_client, &third_client, &fourth_client,
    };
    std::array<char, 4> const characters{'@', '#', '$', '%'};
    bool connected = true;
    bool joined = true;
    for (size_t index = 0U; index < clients.size(); ++index) {
        connected = clients[index]->connect(
            core::Address::localhost(server.port()), std::chrono::seconds{2}
        ) && connected;
        joined = clients[index]->send(shared::encodeMessage(shared::JoinRequestMessage{
            .ch = characters[index], .mode = shared::WorldMode::Flight, .wants_previews = true,
        }), 0, core::SendMode{core::SendMode::Reliable}) && joined;
    }
    joined = connected && joined;
    auto const all_clients_joined = [&clients] {
        return std::ranges::all_of(clients, [](PreviewClient const* const client) {
            return client->join_accepted.value_or(false);
        });
    };
    auto const join_deadline = std::chrono::steady_clock::now() + JOIN_TIMEOUT;
    while (joined && !all_clients_joined() && std::chrono::steady_clock::now() < join_deadline) {
        for (PreviewClient* const client : clients) {
            client->poll(std::chrono::milliseconds{1});
        }
    }
    joined = joined && all_clients_joined();
    for (PreviewClient* const client : clients) {
        while (client->poll(std::chrono::milliseconds::zero()) > 0) {
        }
    }
    uint32_t sent = 0U;
    uint32_t maximum_ack_lag = 0U;
    uint32_t sent_at_maximum_ack_lag = 0U;
    std::array<size_t, 4> midpoint_tile_counts{};
    bool midpoint_recorded = false;
    bool all_sent = true;
    auto const started = std::chrono::steady_clock::now();
    auto next_client_progress = started + PROGRESS_INTERVAL;
    auto next_input = started;
    auto const poll_clients_fairly = [&clients] {
        for (PreviewClient* const client : clients) {
            for (uint32_t poll_call = 0U; poll_call < MAX_CLIENT_POLL_CALLS_PER_PASS; ++poll_call) {
                if (client->poll(std::chrono::milliseconds::zero()) == 0U) {
                    break;
                }
            }
        }
    };
    while (joined && std::chrono::steady_clock::now() - started < DURATION) {
        auto const now = std::chrono::steady_clock::now();
        if (now >= next_input) {
            ++sent;
            std::vector<uint8_t> const input = shared::encodeMessage(shared::ClientInputMessage{
                .direction = {.x = 127U, .accelerated = true, .speedup = 500U},
                .sequence = sent,
            });
            for (PreviewClient* const client : clients) {
                client->trackInputSent(sent);
                all_sent = client->send(input, 0, core::SendMode{core::SendMode::Reliable}) && all_sent;
            }
            next_input += INPUT_PERIOD;
        }
        poll_clients_fairly();
        if (now >= next_client_progress) {
            for (size_t index = 0U; index < clients.size(); ++index) {
                PreviewClient const* const client = clients[index];
                uint64_t const latency_samples = client->acknowledgement_latency_samples;
                CORE_INFO(
                    "Flight client {} sent/ack/lag={}/{}/{}, ack latency samples/avg/max ms={}/{}/{}, "
                    "received tiles={}",
                    index,
                    sent,
                    client->last_acknowledged_input,
                    sent - client->last_acknowledged_input,
                    latency_samples,
                    latency_samples == 0U
                        ? 0U
                        : client->acknowledgement_latency_total_ns / latency_samples / 1'000'000U,
                    client->acknowledgement_latency_max_ns / 1'000'000U,
                    client->received_height_tiles
                );
            }
            next_client_progress = now + PROGRESS_INTERVAL;
        }
        if (!midpoint_recorded && now - started >= DURATION / 2) {
            for (size_t index = 0U; index < clients.size(); ++index) {
                midpoint_tile_counts[index] = clients[index]->received_height_tiles;
            }
            midpoint_recorded = true;
        }
        uint32_t lag = 0U;
        for (PreviewClient const* const client : clients) {
            lag = std::max(lag, sent - client->last_acknowledged_input);
        }
        if (lag > maximum_ack_lag) {
            maximum_ack_lag = lag;
            sent_at_maximum_ack_lag = sent;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{16});
    }
    auto const catch_up_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    auto const all_caught_up = [&clients, sent] {
        return std::ranges::all_of(clients, [sent](PreviewClient const* const client) {
            return sent - client->last_acknowledged_input <= 2U;
        });
    };
    while (!all_caught_up() && std::chrono::steady_clock::now() < catch_up_deadline) {
        for (PreviewClient* const client : clients) {
            client->poll(std::chrono::milliseconds{1});
        }
    }
    CORE_INFO(
        "Flight summary sent={}, maximum ack lag={} at input={}, "
        "final acknowledgements={}/{}/{}/{}, duration={} ms",
        sent,
        maximum_ack_lag,
        sent_at_maximum_ack_lag,
        first_client.last_acknowledged_input,
        second_client.last_acknowledged_input,
        third_client.last_acknowledged_input,
        fourth_client.last_acknowledged_input,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started
        ).count()
    );
    for (size_t index = 0U; index < clients.size(); ++index) {
        PreviewClient const* const client = clients[index];
        uint64_t const latency_samples = client->acknowledgement_latency_samples;
        CORE_INFO(
            "Flight result client {} ack latency samples/avg/max ms={}/{}/{}, received tiles={}",
            index,
            latency_samples,
            latency_samples == 0U
                ? 0U
                : client->acknowledgement_latency_total_ns / latency_samples / 1'000'000U,
            client->acknowledgement_latency_max_ns / 1'000'000U,
            client->received_height_tiles
        );
    }
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    ASSERT_TRUE(connected);
    ASSERT_TRUE(joined);
    EXPECT_TRUE(all_sent);
    ASSERT_TRUE(midpoint_recorded);
    EXPECT_GE(sent, 340U);
    EXPECT_LE(maximum_ack_lag, MAXIMUM_ACK_LAG)
        << "maximum lag occurred after input " << sent_at_maximum_ack_lag
        << ", last acknowledgements " << first_client.last_acknowledged_input << ", "
        << second_client.last_acknowledged_input << ", " << third_client.last_acknowledged_input
        << ", and " << fourth_client.last_acknowledged_input;
    for (size_t index = 0U; index < clients.size(); ++index) {
        PreviewClient const* const client = clients[index];
        EXPECT_LE(sent - client->last_acknowledged_input, 2U);
        EXPECT_GT(client->acknowledgement_latency_samples, 0U)
            << "client " << index << " produced no input acknowledgement latency samples";
        EXPECT_TRUE(client->messages.empty());
        EXPECT_GT(client->received_delivery_batches, 0U);
        EXPECT_GT(client->received_height_tiles, midpoint_tile_counts[index])
            << "client " << index << " made no terrain progress during the second half";
    }
#endif
}

TEST(GameServerPreviewTest, FailedAuthoritativeWorkerCompletionIsReadmittedBeforeDeliveryCanLeaveAGap)
{
    static constexpr std::chrono::seconds TIMEOUT{15};
    static constexpr uint64_t NO_FAILED_KEY = UINT64_MAX;
    static constexpr uint32_t MIN_RECEIVED_COLUMNS = 128U;
    generation::GenerationConfiguration configuration;
    configuration.seed = shared::World::canonicalConfiguration().seed;
    configuration.epochs = 1U;
    configuration.erosion_steps = 0U;
    configuration.initial_climate_iterations = 1U;
    configuration.climate_iterations = 1U;
    configuration.currents = false;
    configuration.coastal_elaboration = false;
    auto preparation = std::make_shared<server::WorldPreparation>(
        configuration, shared::World::canonicalConfiguration().algorithm_version
    );
    std::atomic<uint64_t> failed_key{NO_FAILED_KEY};
    server::GameServer server{0U, {}, shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(), shared::HEIGHT_TILE_INTEREST_RADIUS, nullptr, preparation};
    auto const preparation_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!preparation->readyWorld() && std::chrono::steady_clock::now() < preparation_deadline) {
        static_cast<void>(server.tick(std::chrono::milliseconds{1}));
    }
    ASSERT_NE(preparation->readyWorld(), nullptr);
    static_cast<void>(server.tick(std::chrono::milliseconds{0}));
    server::GameServer::BenchmarkHooks hooks;
    hooks.before_height_tile_completion = [&](core::ClientId, shared::HeightTileKey const key) {
        uint64_t expected = NO_FAILED_KEY;
        uint64_t const encoded = (static_cast<uint64_t>(static_cast<uint32_t>(key.x)) << 32U)
            | static_cast<uint32_t>(key.y);
        if (failed_key.compare_exchange_strong(expected, encoded, std::memory_order_relaxed)) {
            throw std::runtime_error{"injected first authoritative profile worker failure"};
        }
    };
    std::atomic_bool stop{false};
    std::thread server_thread{[&] { server.run(stop, &hooks); }};
    defer {
        stop.store(true, std::memory_order_relaxed);
        server_thread.join();
    };
    PreparedWorldClient client;
    client.acknowledge_authoritative_batches = true;
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = false,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    bool recovered = false;
    while (std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
        uint64_t const encoded = failed_key.load(std::memory_order_relaxed);
        if (encoded != NO_FAILED_KEY) {
            shared::ChunkCoordinate const coordinate{
                static_cast<int32_t>(encoded >> 32U), static_cast<int32_t>(encoded & UINT32_MAX), 0,
            };
            recovered = std::ranges::find(client.received_authoritative_columns, coordinate)
                != client.received_authoritative_columns.end();
        }
        if (recovered && client.received_authoritative_columns.size() >= MIN_RECEIVED_COLUMNS) {
            break;
        }
    }
    EXPECT_NE(failed_key.load(std::memory_order_relaxed), NO_FAILED_KEY);
    EXPECT_TRUE(recovered);
    EXPECT_GE(client.received_authoritative_columns.size(), MIN_RECEIVED_COLUMNS);
}

TEST(GameServerPreviewTest, PreviewMovementCannotOrphanQueuedAuthoritativeProfiles)
{
    static constexpr std::chrono::seconds TIMEOUT{15};
    static constexpr uint32_t INPUT_COUNT = 10U;
    static constexpr uint32_t REQUIRED_COLUMNS = 6'561U;
    generation::GenerationConfiguration configuration;
    configuration.seed = shared::World::canonicalConfiguration().seed;
    configuration.epochs = 1U;
    configuration.erosion_steps = 0U;
    configuration.initial_climate_iterations = 1U;
    configuration.climate_iterations = 1U;
    configuration.currents = false;
    configuration.coastal_elaboration = false;
    auto preparation = std::make_shared<server::WorldPreparation>(
        configuration, shared::World::canonicalConfiguration().algorithm_version
    );
    std::atomic_bool hold_completions{false};
    std::atomic_uint32_t held_completions{0U};
    std::atomic_uint32_t pending_authority{0U};
    server::GameServer::BenchmarkHooks hooks;
    server::GameServer server{0U, {}, shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(), shared::HEIGHT_TILE_INTEREST_RADIUS, nullptr, preparation};
    auto const preparation_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!preparation->readyWorld() && std::chrono::steady_clock::now() < preparation_deadline) {
        static_cast<void>(server.tick(std::chrono::milliseconds{1}));
    }
    ASSERT_NE(preparation->readyWorld(), nullptr);
    static_cast<void>(server.tick(std::chrono::milliseconds{0}));
    hooks.before_height_tile_completion = [&](core::ClientId, shared::HeightTileKey) {
        if (hold_completions.load(std::memory_order_acquire)) {
            held_completions.fetch_add(1U, std::memory_order_relaxed);
            while (hold_completions.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }
    };
    hooks.on_preview_metrics = [&](core::ClientId, server::GameServer::PreviewStreamMetrics const metrics) {
        pending_authority.store(metrics.authoritative_pending_jobs, std::memory_order_relaxed);
    };
    std::atomic_bool stop{false};
    std::thread server_thread{[&] { server.run(stop, &hooks); }};
    defer {
        hold_completions.store(false, std::memory_order_release);
        stop.store(true, std::memory_order_relaxed);
        server_thread.join();
    };
    PreparedWorldClient client;
    client.acknowledge_authoritative_batches = true;
    client.wants_previews = true;
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    auto current_columns = [&] {
        if (!client.coverage_center) {
            return uint32_t{0U};
        }
        return static_cast<uint32_t>(std::ranges::count_if(
            client.required_authoritative_columns, [&](auto const key) {
                return client.received_authoritative_column_keys.contains(key);
            }
        ));
    };
    auto const initial_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (current_columns() != REQUIRED_COLUMNS && std::chrono::steady_clock::now() < initial_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    ASSERT_EQ(current_columns(), REQUIRED_COLUMNS);
    hold_completions.store(true, std::memory_order_release);
    for (uint32_t sequence = 1U; sequence <= INPUT_COUNT; ++sequence) {
        ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {.x = 0U, .y = 127U, .accelerated = true, .speedup = 30U},
            .sequence = sequence,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
        auto const next_input = std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
        while (std::chrono::steady_clock::now() < next_input) {
            static_cast<void>(client.poll(std::chrono::milliseconds{1}));
        }
    }
    EXPECT_GT(held_completions.load(std::memory_order_relaxed), 0U);
    hold_completions.store(false, std::memory_order_release);
    auto const recovery_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (current_columns() != REQUIRED_COLUMNS && std::chrono::steady_clock::now() < recovery_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    EXPECT_EQ(current_columns(), REQUIRED_COLUMNS)
        << "authority jobs still tracked=" << pending_authority.load(std::memory_order_relaxed);
}

TEST(GameServerPreviewTest, MissingAuthoritativeLeafDoesNotBlockConnectedReadyPrefetch)
{
    static constexpr std::chrono::seconds TIMEOUT{15};
    static constexpr uint64_t NO_HELD_KEY = UINT64_MAX;
    static constexpr uint32_t REQUIRED_COLUMNS = 6'561U;
    static constexpr int32_t REQUIRED_RADIUS = shared::FULL_DETAIL_RADIUS_CHUNKS;
    generation::GenerationConfiguration configuration;
    configuration.seed = shared::World::canonicalConfiguration().seed;
    configuration.epochs = 1U;
    configuration.erosion_steps = 0U;
    configuration.initial_climate_iterations = 1U;
    configuration.climate_iterations = 1U;
    configuration.currents = false;
    configuration.coastal_elaboration = false;
    auto preparation = std::make_shared<server::WorldPreparation>(
        configuration, shared::World::canonicalConfiguration().algorithm_version
    );
    std::atomic_bool armed{false};
    std::atomic_bool release{false};
    std::atomic<uint64_t> held_key{NO_HELD_KEY};
    std::unordered_set<shared::ChunkCoordinate, ChunkCoordinateHash> initial_columns;
    shared::ChunkCoordinate initial_center{};
    std::atomic<uint64_t> ready_key{NO_HELD_KEY};
    server::GameServer::BenchmarkHooks hooks;
    server::GameServer server{0U, {}, shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(), shared::HEIGHT_TILE_INTEREST_RADIUS, nullptr, preparation};
    auto const preparation_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!preparation->readyWorld() && std::chrono::steady_clock::now() < preparation_deadline) {
        static_cast<void>(server.tick(std::chrono::milliseconds{1}));
    }
    ASSERT_NE(preparation->readyWorld(), nullptr);
    static_cast<void>(server.tick(std::chrono::milliseconds{0}));
    hooks.before_height_tile_completion = [&](core::ClientId, shared::HeightTileKey const key) {
        if (!armed.load(std::memory_order_acquire)) {
            return;
        }
        shared::ChunkCoordinate const coordinate{key.x, key.y, 0};
        if (initial_columns.contains(coordinate)
            || !std::ranges::any_of(chunkNeighbors(coordinate), [&](auto const neighbor) {
                return initial_columns.contains(neighbor);
            })) {
            return;
        }
        uint64_t const encoded = (uint64_t{static_cast<uint32_t>(key.x)} << 32U)
            | static_cast<uint32_t>(key.y);
        auto const distance = [&](shared::ChunkCoordinate const column) {
            return std::abs(column.x - initial_center.x) + std::abs(column.y - initial_center.y);
        };
        uint64_t expected = NO_HELD_KEY;
        if (held_key.load(std::memory_order_acquire) == NO_HELD_KEY
            && (distance(coordinate) < REQUIRED_RADIUS + 1 + REQUIRED_RADIUS / 2
                || distance(coordinate) > REQUIRED_RADIUS * 2)) {
            return;
        }
        if (held_key.compare_exchange_strong(expected, encoded, std::memory_order_acq_rel)) {
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        } else if (encoded != expected && distance(coordinate) > distance({
                static_cast<int32_t>(expected >> 32U), static_cast<int32_t>(expected & UINT32_MAX), 0,
            })) {
            uint64_t no_ready_key = NO_HELD_KEY;
            static_cast<void>(ready_key.compare_exchange_strong(
                no_ready_key, encoded, std::memory_order_release, std::memory_order_relaxed
            ));
        }
    };
    std::atomic_bool stop{false};
    std::thread server_thread{[&] { server.run(stop, &hooks); }};
    defer {
        release.store(true, std::memory_order_release);
        stop.store(true, std::memory_order_relaxed);
        server_thread.join();
    };
    PreparedWorldClient client;
    client.acknowledge_authoritative_batches = true;
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = false,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    auto const coverage_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!client.coverage_center && std::chrono::steady_clock::now() < coverage_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    ASSERT_TRUE(client.coverage_center.has_value());
    initial_center = shared::WorldBounds::chunkCoordinate({
        client.coverage_center->x, client.coverage_center->y, 0,
    });
    auto const required = shared::requiredTerrainColumns(*client.coverage_center);
    initial_columns.insert(required.begin(), required.end());
    // Hold a real prefetch completion outside startup readiness and the dense
    // movement corridor, where a second producer cannot publish the same gap.
    armed.store(true, std::memory_order_release);
    auto const hold_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (held_key.load(std::memory_order_acquire) == NO_HELD_KEY
        && std::chrono::steady_clock::now() < hold_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    ASSERT_NE(held_key.load(std::memory_order_relaxed), NO_HELD_KEY);
    uint64_t const encoded_held = held_key.load(std::memory_order_acquire);
    shared::ChunkCoordinate const held{
        static_cast<int32_t>(encoded_held >> 32U), static_cast<int32_t>(encoded_held & UINT32_MAX), 0,
    };
    auto const ready_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (ready_key.load(std::memory_order_acquire) == NO_HELD_KEY
        && std::chrono::steady_clock::now() < ready_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    uint64_t const encoded_ready = ready_key.load(std::memory_order_acquire);
    ASSERT_NE(encoded_ready, NO_HELD_KEY) << "An alternative connected leaf must have finished generation.";
    shared::ChunkCoordinate const ready{
        static_cast<int32_t>(encoded_ready >> 32U), static_cast<int32_t>(encoded_ready & UINT32_MAX), 0,
    };
    ASSERT_FALSE(initial_columns.contains(held));
    ASSERT_FALSE(initial_columns.contains(ready));
    auto const startup_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    auto const received_required = [&] {
        return std::ranges::count_if(client.received_authoritative_columns, [&](auto const column) {
            return initial_columns.contains(column);
        });
    };
    while (received_required() < REQUIRED_COLUMNS && std::chrono::steady_clock::now() < startup_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    ASSERT_EQ(received_required(), REQUIRED_COLUMNS);
    ASSERT_TRUE(std::ranges::any_of(chunkNeighbors(ready), [&](auto const neighbor) {
        return initial_columns.contains(neighbor)
            && std::ranges::find(client.received_authoritative_columns, neighbor)
                != client.received_authoritative_columns.end();
    })) << "The ready leaf must connect through an already received initial column.";
    shared::Direction direction{.x = 0U, .y = 0U, .accelerated = true, .speedup = 30U};
    if (std::abs(held.x - initial_center.x) == REQUIRED_RADIUS + 1) {
        direction.x = held.x > initial_center.x ? 127U : 129U;
    } else {
        direction.y = held.y > initial_center.y ? 127U : 129U;
    }
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = direction, .sequence = 1U,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    auto const progress_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
    while (std::ranges::find(client.received_authoritative_columns, ready)
            == client.received_authoritative_columns.end()
        && std::chrono::steady_clock::now() < progress_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    ASSERT_EQ(std::ranges::find(client.received_authoritative_columns, held),
        client.received_authoritative_columns.end()) << "The fixture must hold an actually unavailable leaf.";
    EXPECT_NE(std::ranges::find(client.received_authoritative_columns, ready),
        client.received_authoritative_columns.end()) << "A ready profile remains connected through the old interest edge.";
    release.store(true, std::memory_order_release);
    auto const recovery_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (std::ranges::find(client.received_authoritative_columns, held)
            == client.received_authoritative_columns.end()
        && std::chrono::steady_clock::now() < recovery_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    EXPECT_NE(std::ranges::find(client.received_authoritative_columns, held),
        client.received_authoritative_columns.end());
}

TEST(GameServerPreviewTest, AuthoritativeFrontierRespectsWindowAndRevisitsRejectedReceipt)
{
    static constexpr std::chrono::seconds TIMEOUT{15};
    generation::GenerationConfiguration configuration;
    configuration.seed = shared::World::canonicalConfiguration().seed;
    configuration.epochs = 1U;
    configuration.erosion_steps = 0U;
    configuration.initial_climate_iterations = 1U;
    configuration.climate_iterations = 1U;
    configuration.currents = false;
    configuration.coastal_elaboration = false;
    auto preparation = std::make_shared<server::WorldPreparation>(
        configuration, shared::World::canonicalConfiguration().algorithm_version
    );
    server::GameServer server{0U, {}, shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(), shared::HEIGHT_TILE_INTEREST_RADIUS, nullptr, preparation};
    std::atomic_bool stop{false};
    std::thread server_thread{[&] { server.run(stop); }};
    defer {
        stop.store(true, std::memory_order_relaxed);
        server_thread.join();
    };
    PreparedWorldClient client;
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = false,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    auto batches = [&] {
        std::vector<shared::ServerAuthoritativeTerrainBatchMessage> result;
        for (auto const& message : client.messages) {
            if (auto const* batch = std::get_if<shared::ServerAuthoritativeTerrainBatchMessage>(&message)) {
                result.push_back(*batch);
            }
        }
        return result;
    };
    auto const initial_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    auto const inflight_bytes = [&] {
        uint64_t bytes = 0U;
        for (auto const& batch : batches()) {
            bytes += shared::encodedAuthoritativeTerrainBatchSize(batch);
        }
        return bytes;
    };
    while (inflight_bytes() < 14'000U
        && std::chrono::steady_clock::now() < initial_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    auto const initial = batches();
    ASSERT_FALSE(initial.empty());
    ASSERT_LE(initial.size(), shared::AUTHORITATIVE_TERRAIN_DELIVERY_WINDOW);
    ASSERT_LE(inflight_bytes(), 16'384U);
    ASSERT_FALSE(initial.front().tiles.empty());
    auto const stalled_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{200};
    while (std::chrono::steady_clock::now() < stalled_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    EXPECT_EQ(batches().size(), initial.size());
    shared::ClientAuthoritativeTerrainAckMessage const receipt{
        .session = initial.front().session,
        .coverage_epoch = initial.front().coverage_epoch,
        .delivery_token = initial.front().delivery_token,
        .not_resident_keys = {initial.front().tiles.front().coordinate},
    };
    ASSERT_TRUE(client.send(shared::encodeMessage(receipt), shared::GAME_CHANNEL,
        core::SendMode{core::SendMode::Reliable}));
    auto const retry_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (batches().size() == initial.size() && std::chrono::steady_clock::now() < retry_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    auto const after = batches();
    ASSERT_EQ(after.size(), initial.size() + 1U);
    EXPECT_TRUE(std::ranges::any_of(after.back().tiles, [&](auto const& tile) {
        return tile.coordinate == receipt.not_resident_keys.front();
    }));
    ASSERT_TRUE(client.send(shared::encodeMessage(receipt), shared::GAME_CHANNEL,
        core::SendMode{core::SendMode::Reliable}));
    auto const replay_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{200};
    while (std::chrono::steady_clock::now() < replay_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    EXPECT_EQ(batches().size(), after.size());
}

TEST(GameServerPreviewTest, PreparedStreamsShareAByteBudgetWhileDeliveryReceiptsAreDelayed)
{
    static constexpr std::chrono::seconds TIMEOUT{15};
    static constexpr std::chrono::milliseconds RECEIPT_DELAY{300};
    static constexpr uint32_t MAX_STREAM_BYTES = 16'384U;
    static constexpr uint32_t MAX_TERRAIN_BYTES = 49'152U;
    generation::GenerationConfiguration configuration;
    configuration.seed = shared::World::canonicalConfiguration().seed;
    configuration.epochs = 1U;
    configuration.erosion_steps = 0U;
    configuration.initial_climate_iterations = 1U;
    configuration.climate_iterations = 1U;
    configuration.currents = false;
    configuration.coastal_elaboration = false;
    auto preparation = std::make_shared<server::WorldPreparation>(
        configuration, shared::World::canonicalConfiguration().algorithm_version
    );
    server::GameServer server{
        0U, {}, shared::WorldMode::Flight, shared::World::canonicalConfiguration(),
        shared::HEIGHT_TILE_INTEREST_RADIUS, nullptr, preparation, server::GameServer::BindMode::Loopback,
    };
    std::atomic_bool stop{false};
    std::thread server_thread{[&] { server.run(stop); }};
    defer {
        stop.store(true, std::memory_order_relaxed);
        server_thread.join();
    };
    PreparedWorldClient client;
    client.wants_previews = true;
    client.acknowledge_authoritative_batches = true;
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    auto const started = [&] {
        return std::ranges::any_of(client.start_acknowledgements, [](auto const& ack) { return ack.accepted; });
    };
    auto const start_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!started() && std::chrono::steady_clock::now() < start_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    ASSERT_TRUE(started());
    client.acknowledge_materialized_batches = false;
    client.acknowledge_authoritative_batches = false;
    client.acknowledge_preview_batches = false;
    auto const first_held = client.messages.size();
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = {.x = 127U, .y = 0U, .accelerated = true, .speedup = 30U}, .sequence = 1U,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    auto const stalled_until = std::chrono::steady_clock::now() + RECEIPT_DELAY;
    while (std::chrono::steady_clock::now() < stalled_until) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    uint32_t dense_bytes = 0U;
    uint32_t profile_bytes = 0U;
    uint32_t preview_bytes = 0U;
    std::optional<shared::ServerAuthoritativeTerrainBatchMessage> first_profile;
    for (auto index = first_held; index < client.messages.size(); ++index) {
        auto const& message = client.messages[index];
        uint32_t const encoded_bytes = static_cast<uint32_t>(shared::encodeMessage(message).size());
        if (std::holds_alternative<shared::ServerMaterializedChunkBatchMessage>(message)) {
            dense_bytes += encoded_bytes;
        } else if (auto const* profile = std::get_if<shared::ServerAuthoritativeTerrainBatchMessage>(&message)) {
            profile_bytes += encoded_bytes;
            if (!first_profile) {
                first_profile = *profile;
            }
        } else if (std::holds_alternative<shared::ServerHeightTileBatchMessage>(message)) {
            preview_bytes += encoded_bytes;
        }
    }
    EXPECT_GT(dense_bytes, 0U);
    EXPECT_GT(profile_bytes, 0U);
    EXPECT_GT(preview_bytes, 0U);
    EXPECT_LE(dense_bytes, MAX_STREAM_BYTES);
    EXPECT_LE(profile_bytes, MAX_STREAM_BYTES);
    EXPECT_LE(preview_bytes, MAX_STREAM_BYTES);
    EXPECT_LE(dense_bytes + profile_bytes + preview_bytes, MAX_TERRAIN_BYTES);
    ASSERT_TRUE(first_profile.has_value());
    auto const previous_count = client.messages.size();
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientAuthoritativeTerrainAckMessage{
        .session = first_profile->session, .coverage_epoch = first_profile->coverage_epoch,
        .delivery_token = first_profile->delivery_token,
        .not_resident_keys = {},
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    bool resumed = false;
    auto const resume_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!resumed && std::chrono::steady_clock::now() < resume_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
        resumed = std::ranges::any_of(client.messages.begin() + static_cast<ptrdiff_t>(previous_count),
            client.messages.end(), [](auto const& message) {
                return std::holds_alternative<shared::ServerAuthoritativeTerrainBatchMessage>(message);
            });
    }
    EXPECT_TRUE(resumed) << "A returned profile receipt must make progress while the other bulk streams remain held.";
    std::optional<shared::ServerAuthoritativeTerrainBatchMessage> stale_profile;
    for (auto index = previous_count; index < client.messages.size(); ++index) {
        if (auto const* profile = std::get_if<shared::ServerAuthoritativeTerrainBatchMessage>(&client.messages[index])) {
            stale_profile = *profile;
            break;
        }
    }
    ASSERT_TRUE(stale_profile.has_value());
    uint64_t const old_epoch = client.coverage_epoch;
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = {.x = 127U, .y = 0U, .accelerated = true, .speedup = 500U}, .sequence = 2U,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    auto const epoch_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.coverage_epoch == old_epoch && std::chrono::steady_clock::now() < epoch_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    ASSERT_GT(client.coverage_epoch, old_epoch);
    auto const epoch_stalled_until = std::chrono::steady_clock::now() + RECEIPT_DELAY;
    while (std::chrono::steady_clock::now() < epoch_stalled_until) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    uint64_t unacknowledged_bytes = 0U;
    for (auto index = first_held; index < client.messages.size(); ++index) {
        auto const& message = client.messages[index];
        if (auto const* profile = std::get_if<shared::ServerAuthoritativeTerrainBatchMessage>(&message)) {
            if (profile->coverage_epoch == first_profile->coverage_epoch
                && profile->delivery_token == first_profile->delivery_token) {
                continue;
            }
        } else if (!std::holds_alternative<shared::ServerMaterializedChunkBatchMessage>(message)
            && !std::holds_alternative<shared::ServerHeightTileBatchMessage>(message)) {
            continue;
        }
        unacknowledged_bytes += shared::encodeMessage(message).size();
    }
    EXPECT_LE(unacknowledged_bytes, MAX_TERRAIN_BYTES)
        << "A new coverage epoch must keep the bytes of older in-flight deliveries reserved.";
    std::vector<shared::ChunkCoordinate> rejected;
    rejected.reserve(stale_profile->tiles.size());
    for (auto const& tile : stale_profile->tiles) {
        rejected.push_back(tile.coordinate);
    }
    auto const before_stale_receipt = client.messages.size();
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientAuthoritativeTerrainAckMessage{
        .session = stale_profile->session, .coverage_epoch = stale_profile->coverage_epoch,
        .delivery_token = stale_profile->delivery_token, .not_resident_keys = std::move(rejected),
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    bool current_epoch_resumed = false;
    auto const current_epoch_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!current_epoch_resumed && std::chrono::steady_clock::now() < current_epoch_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
        current_epoch_resumed = std::ranges::any_of(
            client.messages.begin() + static_cast<ptrdiff_t>(before_stale_receipt), client.messages.end(),
            [&client](auto const& message) {
                auto const* profile = std::get_if<shared::ServerAuthoritativeTerrainBatchMessage>(&message);
                return profile != nullptr && profile->coverage_epoch == client.coverage_epoch;
            });
    }
    EXPECT_TRUE(current_epoch_resumed) << "A stale-epoch rejection must retire its old byte debt.";
    auto const before_stale_dense_receipt = client.messages.size();
    for (auto index = first_held; index < before_stale_dense_receipt; ++index) {
        auto const* dense = std::get_if<shared::ServerMaterializedChunkBatchMessage>(&client.messages[index]);
        if (dense == nullptr || dense->coverage_epoch != old_epoch) {
            continue;
        }
        std::vector<shared::ChunkCoordinate> rejected_dense;
        rejected_dense.reserve(dense->chunks.size());
        for (auto const& chunk : dense->chunks) {
            rejected_dense.push_back(chunk.coordinate);
        }
        ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientChunkDeliveryAckMessage{
            .session = dense->session, .coverage_epoch = dense->coverage_epoch,
            .delivery_token = dense->delivery_token, .credits = 1U,
            .not_resident_keys = std::move(rejected_dense),
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    }
    bool current_dense_resumed = false;
    auto const dense_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!current_dense_resumed && std::chrono::steady_clock::now() < dense_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
        current_dense_resumed = std::ranges::any_of(
            client.messages.begin() + static_cast<ptrdiff_t>(before_stale_dense_receipt), client.messages.end(),
            [&client](auto const& message) {
                auto const* dense = std::get_if<shared::ServerMaterializedChunkBatchMessage>(&message);
                return dense != nullptr && dense->coverage_epoch == client.coverage_epoch;
            });
    }
    EXPECT_TRUE(current_dense_resumed)
        << "Stale dense receipts must restore the current-epoch delivery budget; held bytes=" << dense_bytes;
}

class FragmentDelayRelay final {
public:
    explicit FragmentDelayRelay(uint16_t const server_port)
    {
        m_socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
        if (m_socket == ENET_SOCKET_NULL) {
            throw std::runtime_error{"Cannot create loopback relay socket"};
        }
        ENetAddress local{};
        if (enet_address_set_host_ip(&local, "127.0.0.1") != 0
            || enet_socket_bind(m_socket, &local) != 0
            || enet_socket_get_address(m_socket, &local) != 0
            || enet_socket_set_option(m_socket, ENET_SOCKOPT_NONBLOCK, 1) != 0) {
            enet_socket_destroy(m_socket);
            throw std::runtime_error{"Cannot bind loopback relay socket"};
        }
        m_port = local.port;
        m_server = local;
        m_server.port = server_port;
        m_thread = std::thread{[this] { run(); }};
    }

    ~FragmentDelayRelay()
    {
        m_stop.store(true, std::memory_order_relaxed);
        m_thread.join();
        enet_socket_destroy(m_socket);
    }

    [[nodiscard]] uint16_t port() const noexcept { return m_port; }
    void holdNextFragment() noexcept { m_armed.store(true, std::memory_order_release); }
    [[nodiscard]] bool holding() const noexcept { return m_holding.load(std::memory_order_acquire); }
    [[nodiscard]] bool failed() const noexcept { return m_failed.load(std::memory_order_relaxed); }
    [[nodiscard]] std::chrono::steady_clock::time_point holdUntil() const noexcept
    {
        return std::chrono::steady_clock::time_point{
            std::chrono::nanoseconds{m_hold_until_ns.load(std::memory_order_acquire)}
        };
    }

private:
    struct Fragment final {
        uint8_t channel;
        uint16_t sequence;
        bool operator==(Fragment const&) const noexcept = default;
    };
    struct Datagram final {
        ENetAddress target;
        std::vector<uint8_t> bytes;
        std::chrono::steady_clock::time_point deliver_at;
    };

    [[nodiscard]] static std::optional<Fragment> fragment(std::span<uint8_t const> const bytes)
    {
        if (bytes.size() < sizeof(uint16_t)) {
            return std::nullopt;
        }
        uint16_t peer = 0U;
        std::memcpy(&peer, bytes.data(), sizeof(peer));
        uint32_t const header_bytes = (ENET_NET_TO_HOST_16(peer) & ENET_PROTOCOL_HEADER_FLAG_SENT_TIME) != 0
            ? sizeof(ENetProtocolHeader) : sizeof(uint16_t);
        if (bytes.size() < header_bytes + sizeof(ENetProtocolSendFragment)
            || (bytes[header_bytes] & ENET_PROTOCOL_COMMAND_MASK) != ENET_PROTOCOL_COMMAND_SEND_FRAGMENT) {
            return std::nullopt;
        }
        ENetProtocolSendFragment command{};
        std::memcpy(&command, bytes.data() + header_bytes, sizeof(command));
        return Fragment{command.header.channelID, ENET_NET_TO_HOST_16(command.startSequenceNumber)};
    }

    void run()
    {
        static constexpr std::chrono::milliseconds LINK_DELAY{5};
        static constexpr std::chrono::milliseconds FRAGMENT_DELAY{500};
        std::deque<Datagram> pending;
        std::optional<ENetAddress> client;
        std::optional<Fragment> held;
        auto held_until = std::chrono::steady_clock::time_point{};
        while (!m_stop.load(std::memory_order_relaxed)) {
            std::array<uint8_t, 4'096U> bytes{};
            ENetBuffer buffer{};
            buffer.data = bytes.data();
            buffer.dataLength = bytes.size();
            ENetAddress source{};
            auto const received = enet_socket_receive(m_socket, &source, &buffer, 1U);
            auto const now = std::chrono::steady_clock::now();
            if (received < 0) {
                m_failed.store(true, std::memory_order_relaxed);
                return;
            }
            if (received > 0) {
                bool const downstream = source.host == m_server.host && source.port == m_server.port;
                if (!downstream) {
                    client = source;
                }
                if (!downstream || client.has_value()) {
                    auto deliver_at = now + LINK_DELAY;
                    auto const current_fragment = fragment(std::span<uint8_t const>{
                        bytes.data(), static_cast<uint32_t>(received),
                    });
                    if (downstream && current_fragment && !held && m_armed.load(std::memory_order_acquire)) {
                        held = current_fragment;
                        held_until = now + FRAGMENT_DELAY;
                        m_hold_until_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            held_until.time_since_epoch()).count(), std::memory_order_release);
                        m_holding.store(true, std::memory_order_release);
                    }
                    if (downstream && current_fragment && current_fragment == held && now < held_until) {
                        deliver_at = held_until;
                    }
                    pending.push_back({
                        .target = downstream ? *client : m_server,
                        .bytes = {bytes.begin(), bytes.begin() + received},
                        .deliver_at = deliver_at,
                    });
                }
            }
            for (auto packet = pending.begin(); packet != pending.end();) {
                if (packet->deliver_at > now) {
                    ++packet;
                    continue;
                }
                ENetBuffer outgoing{};
                outgoing.data = packet->bytes.data();
                outgoing.dataLength = packet->bytes.size();
                if (enet_socket_send(m_socket, &packet->target, &outgoing, 1U) < 0) {
                    m_failed.store(true, std::memory_order_relaxed);
                    return;
                }
                packet = pending.erase(packet);
            }
            if (received == 0) {
                uint32_t condition = ENET_SOCKET_WAIT_RECEIVE;
                static_cast<void>(enet_socket_wait(m_socket, &condition, 1U));
            }
        }
    }

    ENetSocket m_socket = ENET_SOCKET_NULL;
    ENetAddress m_server{};
    uint16_t m_port = 0U;
    std::atomic_bool m_stop{false};
    std::atomic_bool m_armed{false};
    std::atomic_bool m_holding{false};
    std::atomic_bool m_failed{false};
    std::atomic<int64_t> m_hold_until_ns{0};
    std::thread m_thread;
};

TEST(GameServerPreviewTest, DelayedTerrainFragmentsDoNotBlockAuthoritativeInputAcks)
{
    static constexpr std::chrono::seconds TIMEOUT{15};
    static constexpr std::chrono::milliseconds ACK_DEADLINE{250};
    generation::GenerationConfiguration configuration;
    configuration.seed = shared::World::canonicalConfiguration().seed;
    configuration.epochs = 1U;
    configuration.erosion_steps = 0U;
    configuration.initial_climate_iterations = 1U;
    configuration.climate_iterations = 1U;
    configuration.currents = false;
    configuration.coastal_elaboration = false;
    auto preparation = std::make_shared<server::WorldPreparation>(
        configuration, shared::World::canonicalConfiguration().algorithm_version
    );
    server::GameServer server{
        0U, {}, shared::WorldMode::Flight, shared::World::canonicalConfiguration(),
        shared::HEIGHT_TILE_INTEREST_RADIUS, nullptr, preparation, server::GameServer::BindMode::Loopback,
    };
    std::atomic_bool stop{false};
    std::thread server_thread{[&] { server.run(stop); }};
    defer {
        stop.store(true, std::memory_order_relaxed);
        server_thread.join();
    };
    FragmentDelayRelay relay{server.port()};
    PreparedWorldClient client;
    client.acknowledge_authoritative_batches = true;
    ASSERT_TRUE(client.connect(core::Address::localhost(relay.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = false,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    auto const start_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    auto const started = [&] {
        return std::ranges::any_of(client.start_acknowledgements, [](auto const& ack) { return ack.accepted; });
    };
    while (!started() && std::chrono::steady_clock::now() < start_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    ASSERT_TRUE(started());
    relay.holdNextFragment();
    auto const hold_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!relay.holding() && std::chrono::steady_clock::now() < hold_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
    }
    ASSERT_TRUE(relay.holding());
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = {.x = 127U, .y = 0U}, .sequence = 1U,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    bool acknowledged = false;
    auto const ack_deadline = std::chrono::steady_clock::now() + ACK_DEADLINE;
    while (!acknowledged && std::chrono::steady_clock::now() < ack_deadline) {
        static_cast<void>(client.poll(std::chrono::milliseconds{1}));
        acknowledged = std::ranges::any_of(client.messages, [](auto const& message) {
            auto const* position = std::get_if<shared::ServerPlayerPositionMessage>(&message);
            return position != nullptr && position->ch == '@' && position->acknowledged_input_sequence == 1U;
        });
    }
    EXPECT_TRUE(acknowledged) << "Control ACK must arrive while a terrain fragment is still held.";
    EXPECT_LT(std::chrono::steady_clock::now(), relay.holdUntil());
    EXPECT_FALSE(relay.failed());
}
