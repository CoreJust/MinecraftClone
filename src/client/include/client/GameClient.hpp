#pragma once

#include "ClientAudio.hpp"
#include "PlayerPresentation.hpp"
#include "PreviewResidency.hpp"
#include "WorldLoading.hpp"

#include <shared/net/Message.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>

#include <core/net/Address.hpp>
#include <core/net/Client.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace shared {

class TerrainWorld;
struct TerrainLodArtifact;

} // namespace shared

namespace client {

class TerrainLodSource;

struct GameClientLoopSample final {
    std::chrono::steady_clock::time_point started_at;
    std::chrono::steady_clock::time_point completed_at;
    std::chrono::nanoseconds network_poll_duration{ 0 };
    std::chrono::nanoseconds height_tile_delivery_duration{ 0 };
    std::chrono::nanoseconds render_duration{ 0 };
    uint64_t network_event_count = 0U;
    uint64_t client_message_payload_bytes_sent = 0U;
    uint64_t client_message_payload_bytes_received = 0U;
    bool input_sent = false;
    bool presentation_succeeded = false;
};

struct GameClientBenchmarkHooks final {
    std::optional<std::chrono::steady_clock::time_point> deadline;
    std::function<bool()> should_stop;
    std::function<bool()> is_uncapped_phase;
    std::function<std::optional<shared::Direction>(uint64_t)> input_override;
    std::function<bool(uint64_t)> start_input_override;
    std::function<void(GameClientLoopSample const&)> on_loop;
    std::function<void(shared::Player const&)> on_authoritative_player;
    std::function<void(
        shared::ClientInputMessage const&,
        std::chrono::steady_clock::time_point,
        std::chrono::steady_clock::time_point
    )> on_input_sent;
    std::function<void(
        shared::ServerPlayerPositionMessage const&,
        shared::Player const&,
        std::chrono::steady_clock::time_point
    )> on_authoritative_state;
    std::function<void(shared::ServerMaterializedChunkBatchMessage const&)> on_materialized_chunk_batch_received = {};
    std::function<void(
        uint64_t,
        uint64_t,
        shared::ChunkCoordinate,
        WorldLoadingStatus const&,
        WorldLoadingStatus const&
    )> on_materialized_chunk_recorded = {};
};

class GameClient : public core::Client {
public:
    static constexpr uint32_t MAX_PENDING_INPUTS = 64;

    explicit GameClient(
        shared::WorldMode const mode = shared::WorldMode::Flat,
        shared::WorldConfiguration const configuration = shared::World::canonicalConfiguration(),
        bool const wants_previews = true,
        std::unique_ptr<ClientAudioOutput> audio_output = {}
    );
    ~GameClient() override;

    void run(
        core::Address server_address,
        char ch,
        GameClientBenchmarkHooks const* benchmark_hooks = nullptr,
        std::optional<std::chrono::steady_clock::time_point> request_accepted_at = std::nullopt
    );
    [[nodiscard]] std::optional<uint32_t> renderDistance() const noexcept { return m_render_distance; }
    [[nodiscard]] uint32_t requiredHeightTileCount() const noexcept
    {
        return m_render_distance ? shared::heightTileInterestCount(*m_render_distance) : 0U;
    }
    [[nodiscard]] PreviewResidency const& heightTileResidency() const noexcept
    {
        return m_height_tile_residency;
    }
    [[nodiscard]] PreviewResidency& heightTileResidency() noexcept
    {
        return m_height_tile_residency;
    }
    [[nodiscard]] WorldLoadingStatus worldLoadingStatus() const;
    [[nodiscard]] shared::SparseWorld const* authoritativeWorld() const noexcept;
    [[nodiscard]] std::optional<shared::TerrainIdentity> activeTerrainIdentity() const;
    [[nodiscard]] std::optional<shared::TerrainSessionIdentity> activeTerrainSession() const
    {
        return m_terrain_session;
    }
    [[nodiscard]]
    std::shared_ptr<shared::TerrainWorld const> terrainLodWorld() const noexcept;
    [[nodiscard]]
    std::shared_ptr<shared::TerrainLodArtifact const> terrainLodRoot() const noexcept;
    [[nodiscard]] std::optional<uint64_t> activeCoverageEpoch() const noexcept;
    [[nodiscard]] std::optional<shared::PlayerPosition> activeCoverageCenter() const noexcept;
    [[nodiscard]] uint32_t requiredAuthoritativeColumnCount() const noexcept;
    [[nodiscard]] uint32_t residentAuthoritativeColumnCount() const noexcept;
    [[nodiscard]] bool activeCoverageContains(shared::ChunkCoordinate coordinate) const noexcept;
protected:
    virtual void servicePlatformEvents() {}
    virtual void onInputSubmitted(shared::Direction const&) {}
    virtual shared::Direction input() = 0;
    virtual void render() = 0;
    [[nodiscard]] virtual bool presentationSucceeded() const { return false; }

    using LoadingTimePoint = WorldLoading::TimePoint;
    [[nodiscard]] bool beginLoadingRequest(LoadingTimePoint accepted_at);
    [[nodiscard]] bool applyWorldPreparation(
        shared::ServerWorldPreparationMessage const& message,
        LoadingTimePoint received_at
    );
    [[nodiscard]] bool applyChunkCoverage(
        shared::ServerChunkCoverageMessage const& message,
        LoadingTimePoint received_at
    );
    [[nodiscard]] bool applyAuthoritativeTerrainBatch(
        shared::ServerAuthoritativeTerrainBatchMessage const& message, LoadingTimePoint received_at
    );
    [[nodiscard]] bool applyMaterializedChunkBatch(
        shared::ServerMaterializedChunkBatchMessage const& message,
        LoadingTimePoint received_at
    );
    [[nodiscard]] bool applyStartWorldAck(
        shared::ServerStartWorldAckMessage const& message,
        LoadingTimePoint acknowledged_at
    );
    void markLoadingPromptRendered(LoadingTimePoint rendered_at);
    [[nodiscard]] LoadingInputResult acceptLoadingStartInput(LoadingTimePoint accepted_at);

    [[nodiscard]] bool send(shared::Message const& message);
    [[nodiscard]]
    std::optional<shared::ClientInputMessage> predictInput(
        shared::Direction direction,
        std::chrono::steady_clock::time_point predicted_at = std::chrono::steady_clock::now()
    );
    void discardPredictedInput(
        uint32_t sequence,
        std::chrono::steady_clock::time_point discarded_at = std::chrono::steady_clock::now()
    );
    [[nodiscard]]
    bool applyServerPosition(
        shared::ServerPlayerPositionMessage const& message,
        std::chrono::steady_clock::time_point received_at = std::chrono::steady_clock::now()
    );
    void applyServerRemoval(char character);
    [[nodiscard]] std::optional<shared::Player> predictedLocalPlayer() const noexcept;
    [[nodiscard]] std::optional<shared::PlayerPosition> forecastedLocalPlayerPosition() const noexcept;
    [[nodiscard]]
    std::optional<PlayerPresentationPosition> predictedLocalPresentation(
        std::chrono::steady_clock::time_point now
    ) const noexcept;
    [[nodiscard]] bool applyHeightTile(shared::ServerHeightTileMessage const& message);
    std::vector<shared::HeightTileKey> applyHeightTileBatch(
        shared::ServerHeightTileBatchMessage const& message
    );
    [[nodiscard]] bool applyHeightTileRemoval(shared::ServerRemoveHeightTileMessage const& message);
    void processPendingHeightTileDeliveries();
    [[nodiscard]] bool applyHeightTileDescriptor(shared::ServerHeightTileDescriptorMessage const& message);
protected:
    static constexpr std::array<char, 5> FLIGHT_CHARACTERS{ '@', '#', '$', '%', '&' };

    void onDisconnected(core::DisconnectEvent const event) override;
    virtual void onConnectionStateReset() { }
    void resetConnectionState();

private:
    void onReceived(core::ReceiveEvent event) override;

    struct ChunkCoordinateHash final {
        [[nodiscard]] uint64_t operator()(shared::ChunkCoordinate coordinate) const noexcept;
    };

    struct PendingMaterializedBatch final {
        uint64_t delivery_token = 0U;
        std::vector<shared::ChunkCoordinate> unresolved_coordinates;
        std::vector<shared::ChunkCoordinate> not_resident_keys;
    };

    struct PendingMaterializedChunk final {
        shared::Chunk chunk;
        uint64_t delivery_token = 0U;
    };

    using ChunkKeySet = std::unordered_set<shared::ChunkCoordinate, ChunkCoordinateHash>;

    [[nodiscard]] bool sendChunkDeliveryAck(
        uint64_t coverage_epoch,
        uint64_t delivery_token,
        uint8_t credits,
        std::vector<shared::ChunkCoordinate> not_resident_keys = {}
    );
    void processPendingMaterializedChunks(LoadingTimePoint received_at);
    void resolvePendingMaterializedChunk(
        uint64_t delivery_token,
        shared::ChunkCoordinate coordinate,
        bool not_resident,
        LoadingTimePoint received_at
    );
    [[nodiscard]] bool connectedToActiveCoverage(shared::ChunkCoordinate coordinate) const noexcept;
    [[nodiscard]] bool isActiveCoverageKey(shared::ChunkCoordinate coordinate) const noexcept;
    [[nodiscard]] bool isActiveMaterializedInterestKey(shared::ChunkCoordinate coordinate) const noexcept;
    [[nodiscard]] bool validateSession(shared::TerrainSessionIdentity const& session) const noexcept;
    void clearActiveCoverage() noexcept;
    void clearResidentSurfaceMetadata();
    void removeResidentSurfaceMetadata(shared::ChunkCoordinate coordinate);
    void publishResidentSurfaceMetadata(shared::ChunkCoordinate coordinate);
protected:
    shared::World m_world;
    shared::World m_predicted_world;
    PlayerPresentation m_player_presentation;
    PreviewResidency m_height_tile_residency;
    bool m_wants_previews;
    ClientAudio m_client_audio;
    HeightTileRevision m_height_tile_revision{ .generation = 1, .revision = 1 };
    std::optional<uint32_t> m_render_distance;
    std::deque<shared::ServerHeightTileBatchMessage> m_pending_height_tile_deliveries;
    std::unordered_set<uint64_t> m_pending_height_tile_delivery_tokens;
    uint64_t m_height_tile_credit_revision = 0U;
    std::optional<shared::TerrainIdentity> m_terrain_identity;
    std::deque<shared::ClientInputMessage> m_pending_inputs;
    std::optional<shared::Direction> m_last_predicted_direction;
    std::optional<shared::PlayerPosition> m_forecast_player_position;
    std::unordered_map<char, uint32_t> m_state_revisions;
    shared::PlayerId m_next_id = 0;
    uint32_t m_last_received_input_ack = 0U;
    uint32_t m_last_received_position_revision = 0U;
    std::chrono::steady_clock::time_point m_last_received_position_at{};
    bool m_last_received_position_accepted = false;
    uint64_t m_rejected_local_position_count = 0U;
    uint32_t m_last_accepted_input_ack = 0U;
    uint32_t m_last_accepted_position_revision = 0U;
    std::chrono::steady_clock::time_point m_last_accepted_position_at{};
    uint32_t m_next_input_sequence = 1;
    char m_local_character = 0;
    bool m_running = true;
    bool m_accepted = false;
    uint32_t m_join_character_index = 0;
    GameClientBenchmarkHooks const* m_benchmark_hooks = nullptr;
    uint64_t m_benchmark_bytes_sent = 0U;
    uint64_t m_benchmark_bytes_received = 0U;
    WorldLoading m_world_loading;
    std::optional<shared::TerrainSessionIdentity> m_terrain_session;
    std::unique_ptr<TerrainLodSource> m_terrain_lod_source;
    std::unique_ptr<shared::SparseWorld> m_authoritative_world;
    std::optional<uint64_t> m_active_coverage_epoch;
    std::optional<shared::PlayerPosition> m_active_coverage_center;
    std::optional<shared::PlayerPosition> m_coverage_bfs_root;
    std::vector<shared::ChunkCoordinate> m_active_coverage_required_keys;
    ChunkKeySet m_active_coverage_required_set;
    ChunkKeySet m_active_materialized_interest_set;
    ChunkKeySet m_required_authoritative_columns;
    ChunkKeySet m_authoritative_interest_columns;
    ChunkKeySet m_connected_authoritative_columns;
    uint64_t m_last_authoritative_delivery_token = 0U;
    ChunkKeySet m_active_coverage_chunks;
    ChunkKeySet m_connected_coverage_chunks;
    ChunkKeySet m_resident_height_tile_keys;
    ChunkKeySet m_pending_materialized_keys;
    std::unordered_map<uint64_t, PendingMaterializedBatch> m_pending_materialized_batches;
    std::vector<PendingMaterializedChunk> m_pending_materialized_chunks;
    uint64_t m_last_world_preparation_sequence = 0U;
    uint64_t m_next_join_epoch = 1U;
    bool m_initial_chunk_credit_sent = false;
    bool m_start_message_sent = false;
    bool m_coverage_root_pending = false;
    WorldLoadingIdentity m_world_loading_identity;
private:
    [[nodiscard]] bool sendJoinRequest();
    [[nodiscard]] bool applyWorldRevision(shared::ServerWorldRevisionMessage const& message);
    [[nodiscard]] bool grantHeightTileCredit(
        uint64_t delivery_token,
        uint8_t credits,
        std::vector<shared::HeightTileKey> not_resident_keys = {}
    );
    [[nodiscard]] bool queueHeightTileDelivery(shared::ServerHeightTileBatchMessage message);
    void rebuildPrediction();
    void refreshMovementForecast();
    void updatePredictedPresentation(
        std::chrono::steady_clock::time_point updated_at,
        PlayerPresentationUpdateKind update_kind
    );
    [[nodiscard]] bool applyHeightTileAndTrackEviction(
        shared::ServerHeightTileMessage const& message,
        std::optional<shared::HeightTileKey>& evicted_key
    );
};

} // namespace client
