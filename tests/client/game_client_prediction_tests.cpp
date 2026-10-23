#include <client/Camera.hpp>
#include <client/ClientAudio.hpp>
#include <client/GameClient.hpp>
#include <client/PlayerPresentation.hpp>
#include <client/PreviewResidency.hpp>
#include <client/WorldLoading.hpp>
#include <server/GameServer.hpp>
#include <server/WorldPreparation.hpp>
#include <shared/net/Message.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldGeneration.hpp>
#include <generation/Generation.hpp>
#include <core/net/Address.hpp>
#include <core/net/Event.hpp>
#include <core/net/Server.hpp>

#include <gtest/gtest.h>
#include <testsupport/TerrainFixtures.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <ratio>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

class ImmediateAudioOutput final : public client::ClientAudioOutput {
public:
    [[nodiscard]] bool start() noexcept override { return true; }
    void stop() noexcept override {}
    [[nodiscard]] bool play(std::span<int16_t const> samples) noexcept override
    {
        return !samples.empty();
    }
};

class PredictionClient final : public client::GameClient {
public:
    explicit PredictionClient(shared::WorldMode const mode = shared::WorldMode::Flat)
        : GameClient{ mode }
    { }

    using GameClient::applyServerPosition;
    using GameClient::applyServerRemoval;
    using GameClient::applyHeightTile;
    using GameClient::applyHeightTileBatch;
    using GameClient::applyHeightTileRemoval;
    using GameClient::applyHeightTileDescriptor;
    using GameClient::discardPredictedInput;
    using GameClient::predictInput;
    using GameClient::predictedLocalPlayer;
    using GameClient::forecastedLocalPlayerPosition;
    using GameClient::predictedLocalPresentation;
    using GameClient::resetConnectionState;
    using GameClient::m_last_received_input_ack;
    using GameClient::m_last_received_position_revision;
    using GameClient::m_last_received_position_at;
    using GameClient::m_last_received_position_accepted;
    using GameClient::m_rejected_local_position_count;
    using GameClient::m_last_accepted_input_ack;
    using GameClient::m_last_accepted_position_revision;
    using GameClient::m_last_accepted_position_at;


    void setLocalCharacter(char const character) noexcept
    {
        m_local_character = character;
    }

    [[nodiscard]] std::optional<shared::Player> authoritativePlayer(char const character) const noexcept
    {
        return m_world.playerByCharacter(character);
    }

    [[nodiscard]] client::HeightTileRevision heightTileRevision() const noexcept
    {
        return m_height_tile_revision;
    }

    [[nodiscard]] uint64_t heightTileCreditRevision() const noexcept
    {
        return m_height_tile_credit_revision;
    }

    void setHeightTileResidencyLimits(client::PreviewResidencyLimits const limits)
    {
        m_height_tile_residency = client::PreviewResidency{
            { .generation = 1U, .revision = 1U }, limits
        };
    }

private:
    shared::Direction input() override
    {
        return { };
    }

    void render() override {}
};

class ProductionPredictionClient final : public client::GameClient {
public:
    static constexpr uint32_t FIXED_INPUT_COUNT{ 6 };
    static constexpr std::array<char, 3> PLAYERS{ '@', '#', '$' };

    ProductionPredictionClient(
        shared::Direction const direction,
        std::chrono::steady_clock::time_point const deadline
    )
        : GameClient{
            shared::WorldMode::Flight,
            shared::World::canonicalConfiguration(),
            false,
            std::make_unique<ImmediateAudioOutput>(),
        }
        , m_direction(direction)
        , m_deadline(deadline)
    { }

    [[nodiscard]] bool completed() const noexcept
    {
        return m_completed;
    }

    [[nodiscard]] bool deadlineExpired() const noexcept
    {
        return m_deadline_expired;
    }

    [[nodiscard]] uint32_t inputsSent() const noexcept
    {
        return m_inputs_sent;
    }

    [[nodiscard]] uint32_t highestLocalRevision() const noexcept
    {
        return m_highest_local_revision;
    }

    [[nodiscard]] bool observedAllPlayers() const noexcept
    {
        return m_seen_players[0] && m_seen_players[1] && m_seen_players[2];
    }

    [[nodiscard]] std::optional<shared::Player> authoritativeLocalPlayer() const noexcept
    {
        return m_world.playerByCharacter(m_local_character);
    }

    [[nodiscard]] std::optional<shared::Player> predictedLocalPlayerForTest() const noexcept
    {
        return predictedLocalPlayer();
    }

    [[nodiscard]] bool hasPendingInputs() const noexcept
    {
        return !m_pending_inputs.empty();
    }

    [[nodiscard]] bool movementInputsAcknowledged() const noexcept
    {
        if (m_inputs_sent != FIXED_INPUT_COUNT) {
            return false;
        }
        for (uint32_t const movement_sequence : m_movement_sequences) {
            for (shared::ClientInputMessage const& pending : m_pending_inputs) {
                if (pending.sequence == movement_sequence) {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]] bool pendingInputsAreNeutral() const noexcept
    {
        shared::Direction const neutral{};
        for (shared::ClientInputMessage const& pending : m_pending_inputs) {
            shared::Direction const& direction = pending.direction;
            if (direction.x != neutral.x || direction.y != neutral.y || direction.z != neutral.z
                || direction.accelerated != neutral.accelerated || direction.speedup != neutral.speedup
                || direction.cycle_movement_capabilities != neutral.cycle_movement_capabilities
                || direction.view_x != neutral.view_x || direction.view_y != neutral.view_y) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool predictedPositionMatchesAuthoritative() const noexcept
    {
        std::optional<shared::Player> const authoritative = authoritativeLocalPlayer();
        std::optional<shared::Player> const predicted = predictedLocalPlayerForTest();
        return authoritative && predicted
            && predicted->x == authoritative->x
            && predicted->y == authoritative->y
            && predicted->z == authoritative->z
            && predicted->x_subcell == authoritative->x_subcell
            && predicted->y_subcell == authoritative->y_subcell
            && predicted->z_subcell == authoritative->z_subcell;
    }

    [[nodiscard]] std::string reconciliationDiagnostic() const
    {
        auto const revision = m_state_revisions.find(m_local_character);
        std::string result = "movement_inputs=" + std::to_string(m_inputs_sent)
            + " next_input_sequence=" + std::to_string(m_next_input_sequence)
            + " pending_count=" + std::to_string(m_pending_inputs.size())
            + " movement_acknowledged=" + std::to_string(movementInputsAcknowledged())
            + " pending_neutral=" + std::to_string(pendingInputsAreNeutral())
            + " positions_match=" + std::to_string(predictedPositionMatchesAuthoritative())
            + " local_revision=" + std::to_string(
                revision == m_state_revisions.end() ? uint32_t{0U} : revision->second
            );
        if (!m_pending_inputs.empty()) {
            result += " pending_sequence_range=" + std::to_string(m_pending_inputs.front().sequence)
                + ".." + std::to_string(m_pending_inputs.back().sequence);
        }
        return result;
    }

    [[nodiscard]] std::optional<shared::Player> initialAuthoritativeLocalPlayer() const noexcept
    {
        return m_initial_local_player;
    }

    [[nodiscard]] bool finishAfterReconciliation() noexcept
    {
        auto const revision = m_state_revisions.find(m_local_character);
        if (!movementInputsAcknowledged()
            || revision == m_state_revisions.end()
            || revision->second < FIXED_INPUT_COUNT + 1U
            || !observedAllPlayers()
            || !pendingInputsAreNeutral()
            || !predictedPositionMatchesAuthoritative()) {
            return false;
        }
        m_highest_local_revision = std::max(m_highest_local_revision, revision->second);
        m_completed = true;
        return true;
    }

    void runWithPreparedStart(core::Address const server_address, char const character)
    {
        bool start_requested = false;
        client::GameClientBenchmarkHooks const hooks{
            .deadline = m_deadline,
            .should_stop = [this] { return finishAfterReconciliation(); },
            .is_uncapped_phase = {},
            .input_override = {},
            .start_input_override = [this, &start_requested](uint64_t) {
                client::WorldLoadingStatus const status = worldLoadingStatus();
                if (!start_requested
                    && status.state == client::WorldLoadingState::ReadyForStart
                    && status.ready_prompt_frame_rendered_at.has_value()) {
                    start_requested = true;
                    return true;
                }
                return false;
            },
            .on_loop = {},
            .on_authoritative_player = [this, character](shared::Player const& player) {
                if (player.ch == character && !m_initial_local_player) {
                    m_initial_local_player = player;
                }
            },
            .on_input_sent = {},
            .on_authoritative_state = {},
        };
        GameClient::run(server_address, character, &hooks);
    }

private:
    shared::Direction input() override
    {
        if (!m_world.playerByCharacter(m_local_character).has_value()) {
            return { };
        }
        if (m_inputs_sent < FIXED_INPUT_COUNT) {
            m_movement_sequences[m_inputs_sent] = m_next_input_sequence;
            ++m_inputs_sent;
            return m_direction;
        }
        return { };
    }

    void render() override
    {
        client::WorldLoadingStatus const loading = worldLoadingStatus();
        if (loading.state == client::WorldLoadingState::ReadyForStart) {
            markLoadingPromptRendered(std::chrono::steady_clock::now());
        }
        for (size_t index{ 0 }; index < PLAYERS.size(); ++index) {
            m_seen_players[index] = m_seen_players[index]
                || m_world.playerByCharacter(PLAYERS[index]).has_value();
        }
        if (auto const revision = m_state_revisions.find(m_local_character);
            revision != m_state_revisions.end()) {
            m_highest_local_revision = std::max(m_highest_local_revision, revision->second);
        }
        if (std::chrono::steady_clock::now() >= m_deadline) {
            m_deadline_expired = true;
            m_running = false;
            return;
        }
        if (finishAfterReconciliation()) {
            m_running = false;
        }
    }

    [[nodiscard]] bool presentationSucceeded() const override
    {
        return true;
    }

private:
    shared::Direction m_direction;
    std::chrono::steady_clock::time_point m_deadline;
    std::optional<shared::Player> m_initial_local_player;
    std::array<bool, PLAYERS.size()> m_seen_players{ };
    std::array<uint32_t, FIXED_INPUT_COUNT> m_movement_sequences{ };
    uint32_t m_inputs_sent = 0;
    uint32_t m_highest_local_revision = 0;
    bool m_completed = false;
    bool m_deadline_expired = false;
};

std::shared_ptr<server::WorldPreparation> boundedTestPreparation(
    shared::WorldConfiguration const& world_configuration
)
{
    generation::GenerationConfiguration configuration;
    configuration.seed = world_configuration.seed;
    configuration.epochs = 1U;
    configuration.erosion_steps = 0U;
    configuration.initial_climate_iterations = 1U;
    configuration.climate_iterations = 1U;
    configuration.currents = false;
    configuration.coastal_elaboration = false;
    return std::make_shared<server::WorldPreparation>(configuration, world_configuration.algorithm_version);
}

void establishTerrain(PredictionClient& client, uint64_t const revision = 1U)
{
    static_cast<void>(client.applyHeightTileDescriptor(testsupport::makeHeightTileDescriptor(
        shared::World::canonicalConfiguration(),
        revision,
        shared::HEIGHT_TILE_INTEREST_COUNT
    )));
}

[[nodiscard]]
shared::ServerHeightTileMessage terrainTile(
    shared::HeightTileKey const key,
    uint64_t const revision,
    uint64_t const token
)
{
    return testsupport::makeServerHeightTile(
        key,
        testsupport::makeTerrainIdentity(shared::World::canonicalConfiguration().seed, revision),
        testsupport::HeightTileRequestMetadata{ .revision = revision, .token = token }
    );
}

void appendTerrainTile(
    shared::ServerHeightTileBatchMessage& batch,
    shared::HeightTileKey const key,
    uint64_t const revision,
    uint64_t const token
)
{
    batch.tiles.push_back(terrainTile(key, revision, token));
}

class DelayedServerClient final : public client::GameClient {
public:
    explicit DelayedServerClient(std::chrono::steady_clock::time_point const deadline)
        : GameClient{
            shared::WorldMode::Flat,
            shared::World::canonicalConfiguration(),
            true,
            std::make_unique<ImmediateAudioOutput>(),
        }
        , m_deadline(deadline)
    { }

    [[nodiscard]] bool joined() const noexcept
    {
        return m_joined;
    }

    [[nodiscard]] bool deadlineExpired() const noexcept
    {
        return m_deadline_expired;
    }

private:
    shared::Direction input() override
    {
        return { };
    }

    void render() override
    {
        if (m_world.playerByCharacter(m_local_character).has_value()) {
            m_joined = true;
            m_running = false;
        } else if (std::chrono::steady_clock::now() >= m_deadline) {
            m_deadline_expired = true;
            m_running = false;
        }
    }

private:
    std::chrono::steady_clock::time_point m_deadline;
    bool m_joined = false;
    bool m_deadline_expired = false;
};

class PortReservation final : public core::Server {
public:
    PortReservation()
        : Server{ core::Address::localhost(0), 1U, 2U }
    { }

private:
    void onConnected(core::ServerConnectEvent const) override {}
    void onDisconnected(core::ServerDisconnectEvent const) override {}
    void onReceived(core::ServerReceiveEvent) override {}
};

[[nodiscard]]
shared::ServerPlayerPositionMessage position(
    char const character,
    uint8_t const x,
    uint16_t const x_subcell,
    uint32_t const acknowledged_input_sequence,
    uint32_t const state_revision
)
{
    return {
        .ch = character,
        .x = x,
        .y = 0U,
        .x_subcell = x_subcell,
        .y_subcell = 0U,
        .acknowledged_input_sequence = acknowledged_input_sequence,
        .state_revision = state_revision,
    };
}

[[nodiscard]]
shared::ServerPlayerPositionMessage flightPosition(
    char const character,
    int32_t const x,
    int32_t const y,
    int32_t const z,
    uint16_t const x_subcell,
    uint16_t const y_subcell,
    uint16_t const z_subcell,
    uint32_t const acknowledged_input_sequence,
    uint32_t const state_revision
)
{
    return {
        .ch = character,
        .movement_capabilities = { .bits = 3U },
        .x = x,
        .y = y,
        .z = z,
        .x_subcell = x_subcell,
        .y_subcell = y_subcell,
        .z_subcell = z_subcell,
        .acknowledged_input_sequence = acknowledged_input_sequence,
        .state_revision = state_revision,
    };
}

[[nodiscard]]
shared::ServerPlayerPositionMessage collisionPosition(
    char const character,
    int32_t const x,
    int32_t const y,
    int32_t const z,
    uint16_t const z_subcell,
    int32_t const vertical_velocity_subcells,
    uint32_t const acknowledged_input_sequence,
    uint32_t const state_revision
)
{
    return {
        .ch = character,
        .movement_capabilities = {},
        .x = x,
        .y = y,
        .z = z,
        .x_subcell = 0U,
        .y_subcell = 0U,
        .z_subcell = z_subcell,
        .vertical_velocity_subcells = vertical_velocity_subcells,
        .acknowledged_input_sequence = acknowledged_input_sequence,
        .state_revision = state_revision,
    };
}

TEST(GameClientPredictionTest, RejectsMalformedAndConflictingRenderDistanceDescriptorsBeforeMutation)
{
    static constexpr uint32_t RADIUS = 128U;
    PredictionClient client;
    shared::ServerHeightTileDescriptorMessage const descriptor = testsupport::makeHeightTileDescriptor(
        shared::World::canonicalConfiguration(),
        7U,
        shared::heightTileInterestCount(RADIUS)
    );
    auto malformed = descriptor;
    ++malformed.max_height_tiles;
    EXPECT_FALSE(client.applyHeightTileDescriptor(malformed));
    EXPECT_FALSE(client.renderDistance());
    EXPECT_EQ(client.heightTileRevision(), (client::HeightTileRevision{1U, 1U}));
    static_cast<void>(client.applyHeightTileDescriptor(descriptor));
    EXPECT_EQ(client.renderDistance(), RADIUS);
    EXPECT_EQ(client.requiredHeightTileCount(), 51'433U);
    EXPECT_EQ(client.heightTileRevision(), (client::HeightTileRevision{7U, 7U}));
    EXPECT_TRUE(client.applyHeightTile(testsupport::makeServerHeightTile(
        { .x = 0, .y = 0 },
        testsupport::makeTerrainIdentity(shared::World::canonicalConfiguration().seed, 7U),
        testsupport::HeightTileRequestMetadata{ .revision = 7U, .token = 1U }
    )));
    auto incompatible = descriptor;
    incompatible.max_height_tiles = shared::HEIGHT_TILE_INTEREST_COUNT;
    incompatible.world_revision = 8U;
    incompatible.world_identity.revision = incompatible.world_revision;
    EXPECT_FALSE(client.applyHeightTileDescriptor(incompatible));
    EXPECT_EQ(client.renderDistance(), RADIUS);
    EXPECT_EQ(client.heightTileRevision(), (client::HeightTileRevision{7U, 7U}));
    EXPECT_TRUE(client.heightTileResidency().resident({0, 0}));
    client.resetConnectionState();
    EXPECT_FALSE(client.renderDistance());
    EXPECT_EQ(client.requiredHeightTileCount(), 0U);
    static_cast<void>(client.applyHeightTileDescriptor(incompatible));
    EXPECT_EQ(client.renderDistance(), shared::HEIGHT_TILE_INTEREST_RADIUS);
}

TEST(GameClientPredictionTest, RejectsDescriptorWorldAndPayloadMismatches)
{
    PredictionClient client;
    auto wrong_world = shared::ServerHeightTileDescriptorMessage{};
    ++wrong_world.configuration.seed;
    EXPECT_FALSE(client.applyHeightTileDescriptor(wrong_world));
    EXPECT_FALSE(client.renderDistance());
    auto wrong_payload = shared::ServerHeightTileDescriptorMessage{};
    ++wrong_payload.max_height_tile_bytes;
    EXPECT_FALSE(client.applyHeightTileDescriptor(wrong_payload));
    EXPECT_FALSE(client.renderDistance());
    auto wrong_revision = shared::ServerHeightTileDescriptorMessage{.world_revision = 0U};
    EXPECT_FALSE(client.applyHeightTileDescriptor(wrong_revision));
    EXPECT_FALSE(client.renderDistance());
}

TEST(GameClientPredictionTest, RejectsDescriptorWithTerrainSeedMismatchBeforeMutation)
{
    PredictionClient client;
    shared::ServerHeightTileDescriptorMessage descriptor = testsupport::makeHeightTileDescriptor(
        shared::World::canonicalConfiguration(),
        7U,
        shared::heightTileInterestCount(shared::HEIGHT_TILE_INTEREST_RADIUS)
    );
    ++descriptor.world_identity.seed;

    static_cast<void>(client.applyHeightTileDescriptor(descriptor));

    EXPECT_FALSE(client.renderDistance());
    EXPECT_EQ(client.heightTileRevision(), (client::HeightTileRevision{1U, 1U}));
    EXPECT_EQ(client.heightTileCreditRevision(), 0U);
    EXPECT_EQ(client.heightTileResidency().stats().resident_tiles, 0U);
}

TEST(GameClientPredictionTest, RejectsHeightTileWithDifferentTerrainIdentityBeforeMutation)
{
    static constexpr uint64_t WORLD_REVISION = 7U;
    static constexpr shared::HeightTileKey KEY{ .x = 4, .y = 9 };
    PredictionClient client;
    static_cast<void>(client.applyHeightTileDescriptor(testsupport::makeHeightTileDescriptor(
        shared::World::canonicalConfiguration(),
        WORLD_REVISION,
        shared::heightTileInterestCount(shared::HEIGHT_TILE_INTEREST_RADIUS)
    )));
    shared::ServerHeightTileMessage tile = testsupport::makeServerHeightTile(
        KEY,
        testsupport::makeTerrainIdentity(shared::World::canonicalConfiguration().seed, WORLD_REVISION),
        testsupport::HeightTileRequestMetadata{ .revision = WORLD_REVISION, .token = 1U }
    );
    generation::GenerationConfiguration different_generation;
    different_generation.seed = shared::World::canonicalConfiguration().seed;
    different_generation.resolution += 256U;
    tile.surface.identity.configuration_identity = generation::configurationIdentity(different_generation);

    EXPECT_FALSE(client.applyHeightTile(tile));

    EXPECT_EQ(client.heightTileResidency().resident(KEY), nullptr);
    EXPECT_EQ(client.heightTileResidency().stats().resident_bytes, 0U);
}

TEST(GameClientPredictionTest, RejectsHeightTileWhenHeightIsNotOneAboveSurface)
{
    static constexpr uint64_t WORLD_REVISION = 7U;
    static constexpr shared::HeightTileKey KEY{ .x = 4, .y = 9 };
    PredictionClient client;
    static_cast<void>(client.applyHeightTileDescriptor(testsupport::makeHeightTileDescriptor(
        shared::World::canonicalConfiguration(),
        WORLD_REVISION,
        shared::heightTileInterestCount(shared::HEIGHT_TILE_INTEREST_RADIUS)
    )));
    shared::ServerHeightTileMessage tile = testsupport::makeServerHeightTile(
        KEY,
        testsupport::makeTerrainIdentity(shared::World::canonicalConfiguration().seed, WORLD_REVISION),
        testsupport::HeightTileRequestMetadata{ .revision = WORLD_REVISION, .token = 1U }
    );
    ++tile.heights[0U];

    EXPECT_FALSE(client.applyHeightTile(tile));

    EXPECT_EQ(client.heightTileResidency().resident(KEY), nullptr);
    EXPECT_EQ(client.heightTileResidency().stats().resident_bytes, 0U);
}

TEST(GameClientPredictionTest, AcceptedRadiusSetsTheResidencyHysteresisBudget)
{
    static constexpr uint32_t RADIUS = 1U;
    static constexpr uint32_t CAPACITY = shared::heightTileInterestCount(RADIUS)
        + client::PreviewResidencyLimits::HYSTERESIS_TILES;
    PredictionClient client;
    static_cast<void>(client.applyHeightTileDescriptor(testsupport::makeHeightTileDescriptor(
        shared::World::canonicalConfiguration(),
        1U,
        shared::heightTileInterestCount(RADIUS)
    )));
    for (uint32_t index = 0U; index <= CAPACITY; ++index) {
        EXPECT_TRUE(client.applyHeightTile(testsupport::makeServerHeightTile(
            { .x = static_cast<int32_t>(index), .y = 0 },
            testsupport::makeTerrainIdentity(shared::World::canonicalConfiguration().seed, 1U),
            testsupport::HeightTileRequestMetadata{ .revision = 1U, .token = index + 1U }
        )));
    }
    EXPECT_EQ(client.heightTileResidency().stats().resident_tiles, CAPACITY);
    ASSERT_NE(client.heightTileResidency().resident({1, 0}), nullptr);
    EXPECT_EQ(client.heightTileResidency().stats().resident_bytes,
        static_cast<uint64_t>(CAPACITY) * client.heightTileResidency().resident({1, 0})->byteSize());
    EXPECT_EQ(client.heightTileResidency().stats().eviction_count, 1U);
    EXPECT_FALSE(client.heightTileResidency().resident({0, 0}));
}

TEST(GameClientPredictionTest, PredictsImmediatelyWithoutMutatingTheAuthoritativeWorld)
{
    static constexpr shared::Direction RIGHT{ .x = 127U, .y = 0U };
    PredictionClient client;
    client.setLocalCharacter('@');
    ASSERT_TRUE(client.applyServerPosition(position('@', 0U, 0U, 0U, 1U)));

    std::optional<shared::ClientInputMessage> const input = client.predictInput(RIGHT);

    ASSERT_TRUE(input.has_value());
    auto const authoritative_player = client.authoritativePlayer('@');
    auto const predicted_player = client.predictedLocalPlayer();
    ASSERT_TRUE(authoritative_player.has_value());
    ASSERT_TRUE(predicted_player.has_value());
    EXPECT_EQ(input->sequence, 1U);
    EXPECT_EQ(authoritative_player->x_subcell, 0U);
    EXPECT_EQ(predicted_player->x_subcell, shared::MOVEMENT_SUBCELLS_PER_TICK);
}

TEST(GameClientPredictionTest, ServerPaletteIdentityUpdatesTheReplicatedPlayer)
{
    PredictionClient client;
    client.setLocalCharacter('@');

    ASSERT_TRUE(client.applyServerPosition({
        .ch = '@',
        .palette_index = 11U,
        .x = 2,
        .y = 3,
        .x_subcell = 0U,
        .y_subcell = 0U,
        .acknowledged_input_sequence = 0U,
        .state_revision = 1U,
    }));
    auto const first_authoritative_player = client.authoritativePlayer('@');
    ASSERT_TRUE(first_authoritative_player.has_value());
    EXPECT_EQ(first_authoritative_player->palette_index, 11U);

    ASSERT_TRUE(client.applyServerPosition({
        .ch = '@',
        .palette_index = 4U,
        .x = 2,
        .y = 3,
        .x_subcell = 0U,
        .y_subcell = 0U,
        .acknowledged_input_sequence = 0U,
        .state_revision = 2U,
    }));
    auto const second_authoritative_player = client.authoritativePlayer('@');
    ASSERT_TRUE(second_authoritative_player.has_value());
    EXPECT_EQ(second_authoritative_player->palette_index, 4U);
}

TEST(GameClientPredictionTest, OverlappingJoinSnapshotsPreserveCapabilitiesAcrossReconnect)
{
    for (uint8_t const capabilities : std::array<uint8_t, 3>{ 0U, 1U, 3U }) {
        SCOPED_TRACE(capabilities);
        shared::World server_world{ shared::WorldMode::Flight };
        server_world.spawnPlayer(1U, '@', shared::PlayerPosition{ .x = 99, .y = 100, .z = 2'000 });
        server_world.spawnPlayer(2U, '#', shared::PlayerPosition{ .x = 100, .y = 100, .z = 2'000 });
        ASSERT_TRUE(server_world.setPlayerMovementCapabilities(2U, { .bits = capabilities }));
        ASSERT_TRUE(server_world.movePlayer(1U, { .x = 127U, .y = 0U }));

        for (char const local_character : std::array<char, 2>{ '@', '#' }) {
            SCOPED_TRACE(local_character);
            PredictionClient client{ shared::WorldMode::Flight };
            for (uint32_t join = 0U; join < 2U; ++join) {
                client.setLocalCharacter(local_character);
                for (auto const& player : server_world.players()) {
                    ASSERT_TRUE(client.applyServerPosition({
                        .ch = player.ch,
                        .palette_index = player.palette_index,
                        .movement_capabilities = player.movement_capabilities,
                        .x = player.x,
                        .y = player.y,
                        .z = player.z,
                        .x_subcell = player.x_subcell,
                        .y_subcell = player.y_subcell,
                        .z_subcell = player.z_subcell,
                        .vertical_velocity_subcells = player.vertical_velocity_subcells,
                        .state_revision = 1U,
                    }));
                }
                auto const walker = client.authoritativePlayer('#');
                auto const bypass = client.authoritativePlayer('@');
                auto const predicted = client.predictedLocalPlayer();
                ASSERT_TRUE(walker.has_value());
                ASSERT_TRUE(bypass.has_value());
                ASSERT_TRUE(predicted.has_value());
                EXPECT_EQ(walker->movement_capabilities.bits, capabilities);
                EXPECT_EQ(bypass->movement_capabilities.bits, 3U);
                EXPECT_EQ(predicted->movement_capabilities.bits, local_character == '#' ? capabilities : 3U);
                client.resetConnectionState();
            }
        }
    }
}

TEST(GameClientPredictionTest, OverlappingReconciliationAppliesTheCompleteAuthoritativeState)
{
    for (uint8_t const capabilities : std::array<uint8_t, 3>{ 0U, 1U, 3U }) {
        SCOPED_TRACE(capabilities);
        PredictionClient client{ shared::WorldMode::Flight };
        client.setLocalCharacter('#');
        ASSERT_TRUE(client.applyServerPosition(flightPosition('@', 99, 100, 2'000, 5'600U, 0U, 0U, 0U, 1U)));
        ASSERT_TRUE(client.applyServerPosition(flightPosition('#', 100, 100, 2'000, 0U, 0U, 0U, 0U, 1U)));
        auto const input = client.predictInput({ .x = 0U, .y = 0U, .z = 127U });
        ASSERT_TRUE(input.has_value());
        auto correction = collisionPosition('#', 100, 100, 2'000, 1'000U, -2'500, input->sequence, 2U);
        correction.movement_capabilities.bits = capabilities;
        correction.palette_index = 7U;

        ASSERT_TRUE(client.applyServerPosition(correction));

        auto const authoritative = client.authoritativePlayer('#');
        auto const predicted = client.predictedLocalPlayer();
        ASSERT_TRUE(authoritative.has_value());
        ASSERT_TRUE(predicted.has_value());
        EXPECT_EQ(authoritative->movement_capabilities.bits, capabilities);
        EXPECT_EQ(predicted->movement_capabilities.bits, capabilities);
        EXPECT_EQ(predicted->z, 2'000);
        EXPECT_EQ(predicted->z_subcell, 1'000U);
        EXPECT_EQ(predicted->vertical_velocity_subcells, -2'500);
        EXPECT_EQ(predicted->palette_index, 7U);
        EXPECT_EQ(client.m_last_accepted_input_ack, input->sequence);
        auto const other_authoritative = client.authoritativePlayer('@');
        ASSERT_TRUE(other_authoritative.has_value());
        EXPECT_EQ(other_authoritative->movement_capabilities.bits, 3U);
    }
}

TEST(GameClientPredictionTest, InvalidAuthoritativeStateDoesNotConsumeItsRevisionOrAcknowledgeInput)
{
    PredictionClient client{ shared::WorldMode::Flight };
    client.setLocalCharacter('@');
    ASSERT_TRUE(client.applyServerPosition(flightPosition('@', 100, 100, 2'000, 0U, 0U, 0U, 0U, 1U)));
    auto const input = client.predictInput({ .x = 127U, .y = 0U });
    ASSERT_TRUE(input.has_value());
    auto correction = flightPosition('@', 101, 100, 2'000, 0U, 0U, 0U, input->sequence, 2U);
    correction.movement_capabilities.bits = 2U;

    EXPECT_FALSE(client.applyServerPosition(correction));

    auto const rejected_authoritative = client.authoritativePlayer('@');
    ASSERT_TRUE(rejected_authoritative.has_value());
    EXPECT_EQ(rejected_authoritative->x, 100);
    EXPECT_EQ(client.m_last_accepted_input_ack, 0U);
    EXPECT_EQ(client.m_last_accepted_position_revision, 1U);
    EXPECT_FALSE(client.m_last_received_position_accepted);
    correction.movement_capabilities.bits = 1U;
    ASSERT_TRUE(client.applyServerPosition(correction));
    auto const corrected_authoritative = client.authoritativePlayer('@');
    ASSERT_TRUE(corrected_authoritative.has_value());
    EXPECT_EQ(corrected_authoritative->x, 101);
    auto const corrected_prediction = client.predictedLocalPlayer();
    ASSERT_TRUE(corrected_prediction.has_value());
    EXPECT_EQ(corrected_prediction->movement_capabilities.bits, 1U);
    EXPECT_EQ(client.m_last_accepted_input_ack, input->sequence);
}

TEST(GameClientPredictionTest, ReconnectResetDropsOldPlayersAndTerrainEpoch)
{
    static constexpr shared::HeightTileKey KEY{ .x = 41, .y = 23 };
    PredictionClient client{ shared::WorldMode::Flight };
    client.setLocalCharacter('@');
    ASSERT_TRUE(client.applyServerPosition(flightPosition('@', 100, 100, 800, 0U, 0U, 0U, 0U, 1U)));
    ASSERT_TRUE(client.applyServerPosition(flightPosition('#', 104, 100, 800, 0U, 0U, 0U, 0U, 1U)));
    establishTerrain(client);
    ASSERT_TRUE(client.applyHeightTile(terrainTile(KEY, 1U, 1U)));
    ASSERT_TRUE(client.authoritativePlayer('#').has_value());
    ASSERT_EQ(client.heightTileResidency().stats().resident_tiles, 1U);

    client.resetConnectionState();

    EXPECT_FALSE(client.authoritativePlayer('@').has_value());
    EXPECT_FALSE(client.authoritativePlayer('#').has_value());
    EXPECT_EQ(client.predictedLocalPresentation(std::chrono::steady_clock::now()), std::nullopt);
    EXPECT_EQ(client.heightTileResidency().stats().resident_tiles, 0U);
    EXPECT_EQ(client.heightTileRevision(), (client::HeightTileRevision{ .generation = 0U, .revision = 0U }));
    EXPECT_EQ(client.heightTileCreditRevision(), 0U);
}

TEST(GameClientPredictionTest, HeightTileRemovalRejectsStaleTileAndEvictsTheResidentTile)
{
    static constexpr shared::HeightTileKey KEY{ .x = 41, .y = 23 };
    static constexpr uint64_t INITIAL_TOKEN = 4U;
    static constexpr uint64_t REMOVAL_TOKEN = 5U;
    PredictionClient client;
    establishTerrain(client);
    shared::ServerHeightTileMessage const tile = terrainTile(KEY, 1U, INITIAL_TOKEN);

    ASSERT_TRUE(client.applyHeightTile(tile));
    ASSERT_NE(client.heightTileResidency().resident(KEY), nullptr);
    ASSERT_TRUE(client.applyHeightTileRemoval({
        .key = KEY,
        .revision = 1U,
        .token = REMOVAL_TOKEN,
    }));

    EXPECT_EQ(client.heightTileResidency().resident(KEY), nullptr);
    EXPECT_FALSE(client.applyHeightTile(tile));
}

TEST(GameClientPredictionTest, HeightTileBatchDispatchesEveryTile)
{
    static constexpr shared::HeightTileKey FIRST_KEY{ .x = 41, .y = 23 };
    static constexpr shared::HeightTileKey SECOND_KEY{ .x = 42, .y = 23 };
    PredictionClient client;
    establishTerrain(client);
    shared::ServerHeightTileBatchMessage batch{
        .delivery_token = 1U,
        .tiles = {},
        .removals = {},
    };
    batch.tiles.reserve(2U);
    appendTerrainTile(batch, FIRST_KEY, 1U, 4U);
    appendTerrainTile(batch, SECOND_KEY, 1U, 5U);
    static_cast<void>(client.applyHeightTileBatch(batch));

    EXPECT_NE(client.heightTileResidency().resident(FIRST_KEY), nullptr);
    EXPECT_NE(client.heightTileResidency().resident(SECOND_KEY), nullptr);
}

TEST(GameClientPredictionTest, AppliesRemovalsBeforeAdditionsToPreserveNeededResidents)
{
    static constexpr client::PreviewResidencyLimits LIMITS{
        .max_resident_tiles = 2U,
        .max_resident_bytes = static_cast<uint64_t>(2U) * client::MAX_HEIGHT_TILE_RESIDENT_BYTES,
    };
    static constexpr shared::HeightTileKey NEEDED_KEY{ .x = 1, .y = 1 };
    static constexpr shared::HeightTileKey REMOVED_KEY{ .x = 2, .y = 1 };
    static constexpr shared::HeightTileKey ADDED_KEY{ .x = 3, .y = 1 };
    PredictionClient client;
    establishTerrain(client);
    client.setHeightTileResidencyLimits(LIMITS);
    ASSERT_TRUE(client.applyHeightTile(terrainTile(NEEDED_KEY, 1U, 1U)));
    ASSERT_TRUE(client.applyHeightTile(terrainTile(REMOVED_KEY, 1U, 2U)));

    std::vector<shared::HeightTileKey> const not_resident_keys = client.applyHeightTileBatch({
        .delivery_token = 3U,
        .tiles = {terrainTile(ADDED_KEY, 1U, 3U)},
        .removals = {{ .key = REMOVED_KEY, .revision = 1U, .token = 4U }},
    });

    EXPECT_TRUE(not_resident_keys.empty());
    EXPECT_NE(client.heightTileResidency().resident(NEEDED_KEY), nullptr);
    EXPECT_EQ(client.heightTileResidency().resident(REMOVED_KEY), nullptr);
    EXPECT_NE(client.heightTileResidency().resident(ADDED_KEY), nullptr);
}

TEST(GameClientPredictionTest, InvalidBatchTileLeavesResidencyAndRemovalsUntouched)
{
    static constexpr client::PreviewResidencyLimits LIMITS{
        .max_resident_tiles = 2U,
        .max_resident_bytes = static_cast<uint64_t>(2U) * client::MAX_HEIGHT_TILE_RESIDENT_BYTES,
    };
    static constexpr shared::HeightTileKey RETAINED_KEY{ .x = 1, .y = 1 };
    static constexpr shared::HeightTileKey REMOVED_KEY{ .x = 2, .y = 1 };
    static constexpr shared::HeightTileKey VALID_ADDITION_KEY{ .x = 3, .y = 1 };
    static constexpr shared::HeightTileKey INVALID_KEY{ .x = 4, .y = 1 };
    PredictionClient client;
    establishTerrain(client);
    client.setHeightTileResidencyLimits(LIMITS);
    shared::ServerHeightTileBatchMessage initial_tiles{ .delivery_token = 0U, .tiles = {}, .removals = {} };
    initial_tiles.tiles.reserve(2U);
    appendTerrainTile(initial_tiles, RETAINED_KEY, 1U, 1U);
    ASSERT_TRUE(client.applyHeightTile(initial_tiles.tiles.back()));
    appendTerrainTile(initial_tiles, REMOVED_KEY, 1U, 2U);
    ASSERT_TRUE(client.applyHeightTile(initial_tiles.tiles.back()));

    shared::ServerHeightTileBatchMessage batch{ .delivery_token = 5U, .tiles = {}, .removals = {} };
    batch.tiles.reserve(2U);
    appendTerrainTile(batch, VALID_ADDITION_KEY, 1U, 3U);
    appendTerrainTile(batch, INVALID_KEY, 1U, 4U);
    generation::GenerationConfiguration different_generation;
    different_generation.seed = shared::World::canonicalConfiguration().seed;
    different_generation.resolution += 256U;
    batch.tiles.back().surface.identity.configuration_identity = generation::configurationIdentity(
        different_generation
    );
    batch.removals.push_back({ .key = REMOVED_KEY, .revision = 1U, .token = 5U });
    std::vector<shared::HeightTileKey> const not_resident_keys = client.applyHeightTileBatch(batch);

    EXPECT_EQ(not_resident_keys, (std::vector<shared::HeightTileKey>{ VALID_ADDITION_KEY, INVALID_KEY }));
    EXPECT_NE(client.heightTileResidency().resident(RETAINED_KEY), nullptr);
    EXPECT_NE(client.heightTileResidency().resident(REMOVED_KEY), nullptr);
    EXPECT_EQ(client.heightTileResidency().resident(VALID_ADDITION_KEY), nullptr);
    EXPECT_EQ(client.heightTileResidency().resident(INVALID_KEY), nullptr);
    EXPECT_EQ(client.heightTileResidency().stats().resident_tiles, 2U);
}

TEST(GameClientPredictionTest, ReportsImplicitEvictionsInDeliveryReceipt)
{
    static constexpr client::PreviewResidencyLimits LIMITS{
        .max_resident_tiles = 2U,
        .max_resident_bytes = static_cast<uint64_t>(2U) * client::MAX_HEIGHT_TILE_RESIDENT_BYTES,
    };
    static constexpr shared::HeightTileKey EVICTED_KEY{ .x = 1, .y = 1 };
    static constexpr shared::HeightTileKey RETAINED_KEY{ .x = 2, .y = 1 };
    static constexpr shared::HeightTileKey ADDED_KEY{ .x = 3, .y = 1 };
    PredictionClient client;
    establishTerrain(client);
    client.setHeightTileResidencyLimits(LIMITS);
    ASSERT_TRUE(client.applyHeightTile(terrainTile(EVICTED_KEY, 1U, 1U)));
    ASSERT_TRUE(client.applyHeightTile(terrainTile(RETAINED_KEY, 1U, 2U)));

    std::vector<shared::HeightTileKey> const not_resident_keys = client.applyHeightTileBatch({
        .delivery_token = 3U,
        .tiles = {terrainTile(ADDED_KEY, 1U, 3U)},
        .removals = {},
    });

    EXPECT_EQ(not_resident_keys, (std::vector<shared::HeightTileKey>{ EVICTED_KEY }));
    EXPECT_EQ(client.heightTileResidency().resident(EVICTED_KEY), nullptr);
    EXPECT_NE(client.heightTileResidency().resident(RETAINED_KEY), nullptr);
    EXPECT_NE(client.heightTileResidency().resident(ADDED_KEY), nullptr);
}

TEST(GameClientPredictionTest, ReportsBudgetRejectedDeliveryTiles)
{
    static constexpr client::PreviewResidencyLimits LIMITS{
        .max_resident_tiles = 2U,
        .max_resident_bytes = 0U,
    };
    static constexpr shared::HeightTileKey REJECTED_KEY{ .x = 1, .y = 1 };
    PredictionClient client;
    establishTerrain(client);
    client.setHeightTileResidencyLimits(LIMITS);

    std::vector<shared::HeightTileKey> const not_resident_keys = client.applyHeightTileBatch({
        .delivery_token = 1U,
        .tiles = {terrainTile(REJECTED_KEY, 1U, 1U)},
        .removals = {},
    });

    EXPECT_EQ(not_resident_keys, (std::vector<shared::HeightTileKey>{ REJECTED_KEY }));
    EXPECT_EQ(client.heightTileResidency().resident(REJECTED_KEY), nullptr);
}

TEST(GameClientPredictionTest, HeightTileResidencyEvictsWithinItsBoundedHysteresisBudget)
{
    static constexpr client::HeightTileRevision REVISION{ .generation = 1U, .revision = 1U };
    static constexpr shared::HeightTileKey FIRST_KEY{ .x = 1, .y = 1 };
    static constexpr shared::HeightTileKey SECOND_KEY{ .x = 2, .y = 1 };
    static constexpr shared::HeightTileKey THIRD_KEY{ .x = 3, .y = 1 };
    std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> const heights = testsupport::makeHeightSamples();
    shared::SurfaceTile first_surface = testsupport::makeSurfaceTile(
        FIRST_KEY,
        testsupport::makeTerrainIdentity(42U, REVISION.revision),
        heights
    );
    uint64_t const resident_bytes_per_tile = static_cast<uint64_t>(sizeof(client::HeightTile))
        + static_cast<uint64_t>(first_surface.identity.configuration_identity.capacity()) + 1U;
    client::PreviewResidencyLimits const limits{
        .max_resident_tiles = 2U,
        .max_resident_bytes = 2U * resident_bytes_per_tile,
    };
    client::PreviewResidency residency{ REVISION, limits };

    ASSERT_EQ(
        residency.accept(FIRST_KEY, REVISION, 1U, heights, std::move(first_surface)).replacement,
        client::HeightTileReplacement::Published
    );
    ASSERT_EQ(
        residency.accept(SECOND_KEY, REVISION, 2U, heights, testsupport::makeSurfaceTile(
            SECOND_KEY,
            testsupport::makeTerrainIdentity(42U, REVISION.revision),
            heights
        )).replacement,
        client::HeightTileReplacement::Published
    );
    auto const third = residency.accept(
        THIRD_KEY,
        REVISION,
        3U,
        heights,
        testsupport::makeSurfaceTile(
            THIRD_KEY,
            testsupport::makeTerrainIdentity(42U, REVISION.revision),
            heights
        )
    );
    ASSERT_EQ(third.replacement, client::HeightTileReplacement::Published);
    ASSERT_TRUE(third.evicted_key.has_value());
    EXPECT_EQ(*third.evicted_key, FIRST_KEY);

    EXPECT_EQ(residency.stats().resident_tiles, limits.max_resident_tiles);
    EXPECT_EQ(residency.resident(FIRST_KEY), nullptr);
    EXPECT_NE(residency.resident(THIRD_KEY), nullptr);
    EXPECT_FALSE(residency.evict(FIRST_KEY, REVISION, 1U));
}

TEST(GameClientPredictionTest, ResidentByteCountIncludesRetainedTerrainIdentityCapacity)
{
    static constexpr client::HeightTileRevision REVISION{ .generation = 1U, .revision = 1U };
    static constexpr shared::HeightTileKey KEY{ .x = 1, .y = 1 };
    std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> const heights = testsupport::makeHeightSamples();
    shared::SurfaceTile surface = testsupport::makeSurfaceTile(
        KEY,
        testsupport::makeTerrainIdentity(42U, REVISION.revision),
        heights
    );
    uint64_t const expected_bytes = sizeof(client::HeightTile)
        + surface.identity.configuration_identity.capacity() + 1U;
    client::PreviewResidency residency{
        REVISION,
        {
            .max_resident_tiles = 1U,
            .max_resident_bytes = expected_bytes,
        },
    };

    client::HeightTileReplacementResult const accepted = residency.accept(
        KEY,
        REVISION,
        1U,
        heights,
        std::move(surface)
    );

    ASSERT_EQ(accepted.replacement, client::HeightTileReplacement::Published);
    ASSERT_TRUE(accepted.handle);
    EXPECT_EQ(accepted.handle->byteSize(), expected_bytes);
    EXPECT_EQ(residency.stats().resident_bytes, expected_bytes);
}

TEST(GameClientPredictionTest, FlightPredictionAndAcknowledgementReconcileAllThreeAxes)
{
    static constexpr shared::Direction ASCEND{ .x = 0U, .y = 0U, .z = 127U };
    std::chrono::steady_clock::time_point const STARTED_AT{};
    PredictionClient client{ shared::WorldMode::Flight };
    client.setLocalCharacter('@');
    ASSERT_TRUE(client.applyServerPosition(
        flightPosition('@', 8, 8, 12, 0U, 0U, 0U, 0U, 1U),
        STARTED_AT
    ));

    std::optional<shared::ClientInputMessage> const input = client.predictInput(ASCEND, STARTED_AT);

    ASSERT_TRUE(input.has_value());
    auto const authoritative_player = client.authoritativePlayer('@');
    auto const predicted_player = client.predictedLocalPlayer();
    ASSERT_TRUE(authoritative_player.has_value());
    ASSERT_TRUE(predicted_player.has_value());
    EXPECT_EQ(authoritative_player->z, 12);
    EXPECT_EQ(authoritative_player->z_subcell, 0U);
    EXPECT_EQ(predicted_player->z, 12);
    EXPECT_EQ(predicted_player->z_subcell, shared::MOVEMENT_SUBCELLS_PER_TICK);

    ASSERT_TRUE(client.applyServerPosition(
        flightPosition(
            '@',
            8,
            8,
            12,
            0U,
            0U,
            shared::MOVEMENT_SUBCELLS_PER_TICK,
            input->sequence,
            2U
        ),
        STARTED_AT + std::chrono::milliseconds{ 100 }
    ));
    ASSERT_TRUE(client.predictedLocalPlayer().has_value());
    EXPECT_EQ(client.predictedLocalPlayer()->x, 8);
    EXPECT_EQ(client.predictedLocalPlayer()->y, 8);
    EXPECT_EQ(client.predictedLocalPlayer()->z, 12);
    EXPECT_EQ(client.predictedLocalPlayer()->z_subcell, shared::MOVEMENT_SUBCELLS_PER_TICK);
    auto const predicted_presentation = client.predictedLocalPresentation(
        STARTED_AT + std::chrono::milliseconds{ 150 }
    );
    ASSERT_TRUE(predicted_presentation.has_value());
    EXPECT_DOUBLE_EQ(
        predicted_presentation->z,
        12.0 + static_cast<double>(shared::MOVEMENT_SUBCELLS_PER_TICK)
            / static_cast<double>(shared::SUBCELLS_PER_CELL)
    );
}

TEST(GameClientPredictionTest, ContinuationForecastRetainsFullInputAndReconcilesWithoutMovingAgain)
{
    static constexpr shared::Direction DIRECTION{
        .x = 90U, .y = 90U, .z = 40U, .accelerated = true, .speedup = 200U,
        .cycle_movement_capabilities = true,
    };
    PredictionClient client{ shared::WorldMode::Flight };
    client.setLocalCharacter('@');
    EXPECT_FALSE(client.forecastedLocalPlayerPosition());
    ASSERT_TRUE(client.applyServerPosition(
        flightPosition('@', 65'530, 65'534, 900, 9'000U, 2'000U, 0U, 0U, 1U)));
    auto const input = client.predictInput(DIRECTION);
    ASSERT_TRUE(input);
    auto const before_forecast = client.predictedLocalPlayer();
    ASSERT_TRUE(before_forecast);
    auto const forecast = client.forecastedLocalPlayerPosition();
    ASSERT_TRUE(forecast);
    EXPECT_EQ(client.predictedLocalPlayer()->x, before_forecast->x);
    EXPECT_EQ(client.predictedLocalPlayer()->x_subcell, before_forecast->x_subcell);
    shared::Direction continuation = DIRECTION;
    continuation.cycle_movement_capabilities = false;
    auto const next_input = client.predictInput(continuation);
    ASSERT_TRUE(next_input);
    auto const next = client.predictedLocalPlayer();
    ASSERT_TRUE(next);
    EXPECT_EQ(next->x, forecast->x);
    EXPECT_EQ(next->y, forecast->y);
    EXPECT_EQ(next->z, forecast->z);
    EXPECT_EQ(next->x_subcell, forecast->x_subcell);
    EXPECT_EQ(next->y_subcell, forecast->y_subcell);
    EXPECT_EQ(next->z_subcell, forecast->z_subcell);

    ASSERT_TRUE(client.applyServerPosition(
        flightPosition('@', 100, 120, 900, 7'000U, 8'000U, 0U, next_input->sequence, 2U)));
    auto const reconciled_forecast = client.forecastedLocalPlayerPosition();
    ASSERT_TRUE(reconciled_forecast);
    ASSERT_TRUE(client.predictInput(continuation));
    auto const reconciled_next = client.predictedLocalPlayer();
    ASSERT_TRUE(reconciled_next);
    EXPECT_EQ(reconciled_next->x, reconciled_forecast->x);
    EXPECT_EQ(reconciled_next->y, reconciled_forecast->y);
    EXPECT_EQ(reconciled_next->x_subcell, reconciled_forecast->x_subcell);
    EXPECT_EQ(reconciled_next->y_subcell, reconciled_forecast->y_subcell);
    client.resetConnectionState();
    EXPECT_FALSE(client.forecastedLocalPlayerPosition());
}

TEST(GameClientPredictionTest, DiscardingTheOnlyInputClearsItsContinuationForecast)
{
    static constexpr shared::Direction DIRECTION{ .x = 127U, .y = 0U, .accelerated = true, .speedup = 200U };
    PredictionClient client{ shared::WorldMode::Flight };
    client.setLocalCharacter('@');
    ASSERT_TRUE(client.applyServerPosition(flightPosition('@', 32'880, 32'768, 900, 0U, 0U, 0U, 0U, 1U)));
    auto const input = client.predictInput(DIRECTION);
    ASSERT_TRUE(input);
    ASSERT_TRUE(client.forecastedLocalPlayerPosition());
    client.discardPredictedInput(input->sequence);
    EXPECT_FALSE(client.forecastedLocalPlayerPosition());
}

TEST(GameClientPredictionTest, DelayedCollisionJumpAcknowledgementDoesNotCreateVerticalOvershoot)
{
    static constexpr int32_t X = 100;
    static constexpr int32_t Y = 100;
    static constexpr shared::Direction JUMP{ .x = 0U, .y = 0U, .z = 127U };
    std::chrono::steady_clock::time_point const started_at{};
    shared::TerrainGenerator const terrain;
    uint16_t const surface = terrain.heightAt(X, Y);
    PredictionClient client{ shared::WorldMode::Flight };
    client.setLocalCharacter('@');

    ASSERT_TRUE(client.applyServerPosition(
        collisionPosition('@', X, Y, surface, 0U, 0, 0U, 1U),
        started_at
    ));
    ASSERT_TRUE(client.predictInput(JUMP, started_at).has_value());
    ASSERT_TRUE(client.predictInput({}, started_at + std::chrono::milliseconds{ 100 }).has_value());

    ASSERT_TRUE(client.applyServerPosition(
        collisionPosition(
            '@',
            X,
            Y,
            surface,
            shared::MOVEMENT_SUBCELLS_PER_TICK,
            5'250,
            1U,
            2U
        ),
        started_at + std::chrono::milliseconds{ 110 }
    ));
    auto const authoritative_player = client.authoritativePlayer('@');
    ASSERT_TRUE(authoritative_player.has_value());
    EXPECT_EQ(authoritative_player->vertical_velocity_subcells, 5'250);

    auto const predicted_presentation = client.predictedLocalPresentation(
        started_at + std::chrono::milliseconds{ 160 }
    );
    ASSERT_TRUE(predicted_presentation.has_value());
    EXPECT_LT(
        predicted_presentation->z,
        static_cast<double>(surface) + 1.3
    );
}

TEST(GameClientPredictionTest, HeldFlightPresentationStaysContinuousAcrossJitteredAcknowledgements)
{
    static constexpr std::chrono::steady_clock::time_point STARTED_AT{};
    static constexpr shared::Direction ASCEND{ .x = 0U, .y = 0U, .z = 127U };
    static constexpr shared::Direction DESCEND{ .x = 0U, .y = 0U, .z = 129U };
    static constexpr std::array<int, 12> ACK_DELAYS_MS{
        180, 80, 190, 90, 175, 85, 160, 70, 150, 60, 140, 50,
    };
    static constexpr double STEP = static_cast<double>(shared::MOVEMENT_SUBCELLS_PER_TICK)
        / static_cast<double>(shared::SUBCELLS_PER_CELL);
    PredictionClient client{ shared::WorldMode::Flight };
    client.setLocalCharacter('@');
    ASSERT_TRUE(client.applyServerPosition(
        flightPosition('@', 8, 8, 12, 0U, 0U, 0U, 0U, 1U), STARTED_AT
    ));

    std::array<shared::Player, ACK_DELAYS_MS.size()> acknowledged_states{};
    size_t next_acknowledgement = 0U;
    double const subcell = 1.0 / static_cast<double>(shared::SUBCELLS_PER_CELL);
    auto const expected_z = [](int const elapsed_ms) {
        double const progress = std::clamp(static_cast<double>(elapsed_ms) / 100.0 - 1.0, 0.0, 12.0);
        double const net_ticks = std::min(progress, 6.0) - std::max(progress - 6.0, 0.0);
        return 12.0 + STEP * net_ticks;
    };
    for (int elapsed_ms = 0; elapsed_ms <= 1'500; ++elapsed_ms) {
        if (elapsed_ms > 0 && elapsed_ms % 100 == 0 && elapsed_ms <= 1'200) {
            size_t const input_index = static_cast<size_t>(elapsed_ms / 100 - 1);
            auto const input = client.predictInput(
                input_index < 6U ? ASCEND : DESCEND,
                STARTED_AT + std::chrono::milliseconds{ elapsed_ms }
            );
            ASSERT_TRUE(input.has_value());
            EXPECT_EQ(input->sequence, input_index + 1U);
            auto const predicted = client.predictedLocalPlayer();
            ASSERT_TRUE(predicted.has_value());
            acknowledged_states[input_index] = *predicted;
        }

        while (next_acknowledgement < ACK_DELAYS_MS.size()
            && static_cast<int>((next_acknowledgement + 1U) * 100U)
                + ACK_DELAYS_MS[next_acknowledgement] == elapsed_ms) {
            shared::Player const& acknowledged = acknowledged_states[next_acknowledgement];
            int32_t authoritative_z = acknowledged.z;
            uint16_t authoritative_z_subcell = acknowledged.z_subcell;
            if (next_acknowledgement == 3U) {
                uint32_t corrected_z = static_cast<uint32_t>(authoritative_z_subcell) + 1U;
                authoritative_z += static_cast<int32_t>(corrected_z / shared::SUBCELLS_PER_CELL);
                authoritative_z_subcell = static_cast<uint16_t>(corrected_z % shared::SUBCELLS_PER_CELL);
            }
            auto const before_ack = client.predictedLocalPresentation(
                STARTED_AT + std::chrono::milliseconds{ elapsed_ms - 1 }
            );
            ASSERT_TRUE(before_ack.has_value());
            ASSERT_TRUE(client.applyServerPosition(
                flightPosition(
                    '@',
                    acknowledged.x,
                    acknowledged.y,
                    authoritative_z,
                    acknowledged.x_subcell,
                    acknowledged.y_subcell,
                    authoritative_z_subcell,
                    static_cast<uint32_t>(next_acknowledgement + 1U),
                    static_cast<uint32_t>(next_acknowledgement + 2U)
                ),
                STARTED_AT + std::chrono::milliseconds{ elapsed_ms }
            ));
            auto const after_ack = client.predictedLocalPresentation(
                STARTED_AT + std::chrono::milliseconds{ elapsed_ms }
            );
            ASSERT_TRUE(after_ack.has_value());
            EXPECT_LE(std::abs(after_ack->z - before_ack->z), STEP / 100.0 + 1.0e-9);
            if (next_acknowledgement == 3U) {
                auto const corrected_prediction = client.predictedLocalPlayer();
                ASSERT_TRUE(corrected_prediction.has_value());
                EXPECT_EQ(corrected_prediction->z_subcell, authoritative_z_subcell);
            }
            ++next_acknowledgement;
        }

        if (elapsed_ms % 17 == 0) {
            auto const presentation = client.predictedLocalPresentation(
                STARTED_AT + std::chrono::milliseconds{ elapsed_ms }
            );
            ASSERT_TRUE(presentation.has_value());
            EXPECT_TRUE(std::isfinite(presentation->z));
            EXPECT_NEAR(presentation->z, expected_z(elapsed_ms), subcell * 1.1);
        }
    }
    EXPECT_EQ(next_acknowledgement, ACK_DELAYS_MS.size());
}

TEST(GameClientPredictionTest, LateAscendingPredictionRetainsFlightPresentationSpeed)
{
    static constexpr std::chrono::steady_clock::time_point STARTED_AT{};
    static constexpr std::chrono::milliseconds LATE_INPUT{ 111 };
    static constexpr std::chrono::milliseconds NEXT_INPUT{ 200 };
    static constexpr std::chrono::milliseconds SAMPLE_AFTER_INPUT{ 201 };
    static constexpr shared::Direction ASCEND{ .x = 0U, .y = 0U, .z = 127U };
    static constexpr double STEP = static_cast<double>(shared::MOVEMENT_SUBCELLS_PER_TICK)
        / static_cast<double>(shared::SUBCELLS_PER_CELL);
    PredictionClient client{ shared::WorldMode::Flight };
    client.setLocalCharacter('@');
    ASSERT_TRUE(client.applyServerPosition(
        flightPosition('@', 8, 8, 12, 0U, 0U, 0U, 0U, 1U), STARTED_AT
    ));

    ASSERT_TRUE(client.predictInput(ASCEND, STARTED_AT).has_value());
    ASSERT_TRUE(client.predictInput(ASCEND, STARTED_AT + LATE_INPUT).has_value());
    auto const before_retarget = client.predictedLocalPresentation(STARTED_AT + NEXT_INPUT);
    ASSERT_TRUE(before_retarget.has_value());

    ASSERT_TRUE(client.predictInput(ASCEND, STARTED_AT + NEXT_INPUT).has_value());

    auto const at_retarget = client.predictedLocalPresentation(STARTED_AT + NEXT_INPUT);
    auto const after_retarget = client.predictedLocalPresentation(STARTED_AT + SAMPLE_AFTER_INPUT);
    ASSERT_TRUE(at_retarget.has_value());
    ASSERT_TRUE(after_retarget.has_value());
    EXPECT_NEAR(at_retarget->z, before_retarget->z, 1.0e-9);
    EXPECT_NEAR(after_retarget->z - at_retarget->z, STEP / 100.0, STEP / 100.0 * 0.25);
}

TEST(GameClientPredictionTest, LateDescendingPredictionRetainsFlightPresentationSpeed)
{
    static constexpr std::chrono::steady_clock::time_point STARTED_AT{};
    static constexpr std::chrono::milliseconds LATE_INPUT{ 111 };
    static constexpr std::chrono::milliseconds NEXT_INPUT{ 200 };
    static constexpr std::chrono::milliseconds SAMPLE_AFTER_INPUT{ 201 };
    static constexpr shared::Direction DESCEND{ .x = 0U, .y = 0U, .z = 129U };
    static constexpr double STEP = static_cast<double>(shared::MOVEMENT_SUBCELLS_PER_TICK)
        / static_cast<double>(shared::SUBCELLS_PER_CELL);
    PredictionClient client{ shared::WorldMode::Flight };
    client.setLocalCharacter('@');
    ASSERT_TRUE(client.applyServerPosition(
        flightPosition('@', 8, 8, 12, 0U, 0U, 0U, 0U, 1U), STARTED_AT
    ));

    ASSERT_TRUE(client.predictInput(DESCEND, STARTED_AT).has_value());
    ASSERT_TRUE(client.predictInput(DESCEND, STARTED_AT + LATE_INPUT).has_value());
    auto const before_retarget = client.predictedLocalPresentation(STARTED_AT + NEXT_INPUT);
    ASSERT_TRUE(before_retarget.has_value());

    ASSERT_TRUE(client.predictInput(DESCEND, STARTED_AT + NEXT_INPUT).has_value());

    auto const at_retarget = client.predictedLocalPresentation(STARTED_AT + NEXT_INPUT);
    auto const after_retarget = client.predictedLocalPresentation(STARTED_AT + SAMPLE_AFTER_INPUT);
    ASSERT_TRUE(at_retarget.has_value());
    ASSERT_TRUE(after_retarget.has_value());
    EXPECT_NEAR(at_retarget->z, before_retarget->z, 1.0e-9);
    EXPECT_NEAR(at_retarget->z - after_retarget->z, STEP / 100.0, STEP / 100.0 * 0.25);
}

TEST(GameClientPredictionTest, AcknowledgementReconcilesAndReplaysWithoutCorrectPredictionSnap)
{
    static constexpr shared::Direction RIGHT{ .x = 127U, .y = 0U };
    PredictionClient client;
    client.setLocalCharacter('@');
    ASSERT_TRUE(client.applyServerPosition(position('@', 0U, 0U, 0U, 1U)));
    ASSERT_TRUE(client.predictInput(RIGHT).has_value());

    ASSERT_TRUE(client.predictedLocalPlayer().has_value());
    uint16_t const first_prediction = client.predictedLocalPlayer()->x_subcell;
    ASSERT_TRUE(client.applyServerPosition(position('@', 0U, shared::MOVEMENT_SUBCELLS_PER_TICK, 1U, 2U)));
    ASSERT_TRUE(client.predictedLocalPlayer().has_value());
    EXPECT_EQ(client.predictedLocalPlayer()->x_subcell, first_prediction);

    ASSERT_TRUE(client.predictInput(RIGHT).has_value());
    ASSERT_TRUE(client.predictInput(RIGHT).has_value());
    ASSERT_TRUE(client.predictedLocalPlayer().has_value());
    uint16_t const predicted_before_acknowledgement = client.predictedLocalPlayer()->x_subcell;
    ASSERT_TRUE(client.applyServerPosition(position(
        '@',
        1U,
        2U * shared::MOVEMENT_SUBCELLS_PER_TICK - shared::SUBCELLS_PER_CELL,
        2U,
        3U
    )));
    ASSERT_TRUE(client.predictedLocalPlayer().has_value());
    EXPECT_EQ(client.predictedLocalPlayer()->x_subcell, predicted_before_acknowledgement);
}

TEST(GameClientPredictionTest, PredictedLocalPresentationInterpolatesFrameSamplesWithoutRestartingCorrectAck)
{
    static constexpr shared::Direction RIGHT{ .x = 127U, .y = 0U };
    static constexpr double STEP = static_cast<double>(shared::MOVEMENT_SUBCELLS_PER_TICK)
        / static_cast<double>(shared::SUBCELLS_PER_CELL);
    std::chrono::steady_clock::time_point const STARTED_AT{};
    PredictionClient sampled_each_frame;
    PredictionClient sampled_once;
    sampled_each_frame.setLocalCharacter('@');
    sampled_once.setLocalCharacter('@');
    ASSERT_TRUE(sampled_each_frame.applyServerPosition(position('@', 0U, 0U, 0U, 1U), STARTED_AT));
    ASSERT_TRUE(sampled_once.applyServerPosition(position('@', 0U, 0U, 0U, 1U), STARTED_AT));
    ASSERT_TRUE(sampled_each_frame.predictInput(RIGHT, STARTED_AT).has_value());
    ASSERT_TRUE(sampled_once.predictInput(RIGHT, STARTED_AT).has_value());

    auto const sample_25 = sampled_each_frame.predictedLocalPresentation(
        STARTED_AT + std::chrono::milliseconds{ 25 }
    );
    auto const sample_50 = sampled_each_frame.predictedLocalPresentation(
        STARTED_AT + std::chrono::milliseconds{ 50 }
    );
    auto const sample_75 = sampled_each_frame.predictedLocalPresentation(
        STARTED_AT + std::chrono::milliseconds{ 75 }
    );
    ASSERT_TRUE(sample_25.has_value());
    ASSERT_TRUE(sample_50.has_value());
    ASSERT_TRUE(sample_75.has_value());
    EXPECT_DOUBLE_EQ(
        sample_25->x,
        STEP * 0.25
    );
    EXPECT_DOUBLE_EQ(
        sample_50->x,
        STEP * 0.5
    );
    EXPECT_DOUBLE_EQ(
        sample_75->x,
        STEP * 0.75
    );
    auto const once_sample = sampled_once.predictedLocalPresentation(
        STARTED_AT + std::chrono::milliseconds{ 75 }
    );
    ASSERT_TRUE(once_sample.has_value());
    EXPECT_DOUBLE_EQ(
        sample_75->x,
        once_sample->x
    );

    ASSERT_TRUE(sampled_each_frame.applyServerPosition(
        position('@', 0U, shared::MOVEMENT_SUBCELLS_PER_TICK, 1U, 2U),
        STARTED_AT + std::chrono::milliseconds{ 50 }
    ));
    auto const reconciled_sample_75 = sampled_each_frame.predictedLocalPresentation(
        STARTED_AT + std::chrono::milliseconds{ 75 }
    );
    ASSERT_TRUE(reconciled_sample_75.has_value());
    EXPECT_DOUBLE_EQ(
        reconciled_sample_75->x,
        STEP * 0.75
    );
    auto const sample_200 = sampled_each_frame.predictedLocalPresentation(
        STARTED_AT + std::chrono::milliseconds{ 200 }
    );
    ASSERT_TRUE(sample_200.has_value());
    EXPECT_DOUBLE_EQ(
        sample_200->x,
        STEP
    );

    client::CameraPose const frame_camera = client::localPlayerThirdPersonPose(
        *reconciled_sample_75,
        { }
    );
    EXPECT_DOUBLE_EQ(frame_camera.position.x, 0.3125 + STEP * 0.75);
}

TEST(GameClientPredictionTest, AuthoritativeWorldWrapKeepsPresentedCameraAtTheSeam)
{
    std::chrono::steady_clock::time_point const STARTED_AT{};
    PredictionClient client{ shared::WorldMode::Flight };
    client.setLocalCharacter('@');
    ASSERT_TRUE(client.applyServerPosition(
        flightPosition('@', 65'535, 20, 12, 9'000U, 0U, 0U, 0U, 1U),
        STARTED_AT
    ));
    ASSERT_TRUE(client.applyServerPosition(
        flightPosition('@', 0, 20, 12, 1'000U, 0U, 0U, 0U, 2U),
        STARTED_AT + std::chrono::milliseconds{ 100 }
    ));

    auto const presented = client.predictedLocalPresentation(STARTED_AT + std::chrono::milliseconds{ 150 });
    ASSERT_TRUE(presented.has_value());
    EXPECT_NEAR(presented->x, 0.0, 1e-9);
    EXPECT_NEAR(client::localPlayerFirstPersonPose(*presented, {}).position.x, 0.3125, 1e-9);
    ASSERT_TRUE(client.predictedLocalPlayer().has_value());
    EXPECT_EQ(client.predictedLocalPlayer()->x, 0U);
}

TEST(GameClientPredictionTest, DiscardedAndCollisionRejectedInputsDoNotLeavePhantomPrediction)
{
    static constexpr shared::Direction RIGHT{ .x = 127U, .y = 0U };
    PredictionClient client;
    client.setLocalCharacter('@');
    ASSERT_TRUE(client.applyServerPosition(position('@', 30U, 0U, 0U, 1U)));

    std::optional<shared::ClientInputMessage> const rejected_input = client.predictInput(RIGHT);
    ASSERT_TRUE(rejected_input.has_value());
    ASSERT_TRUE(client.predictedLocalPlayer().has_value());
    EXPECT_EQ(client.predictedLocalPlayer()->x, 30U);
    ASSERT_TRUE(client.applyServerPosition(position('@', 30U, 0U, 1U, 2U)));
    ASSERT_TRUE(client.predictedLocalPlayer().has_value());
    EXPECT_EQ(client.predictedLocalPlayer()->x, 30U);

    std::optional<shared::ClientInputMessage> const unsent_input = client.predictInput({ .x = 129U, .y = 0U });
    ASSERT_TRUE(unsent_input.has_value());
    client.discardPredictedInput(unsent_input->sequence);
    ASSERT_TRUE(client.predictedLocalPlayer().has_value());
    EXPECT_EQ(client.predictedLocalPlayer()->x, 30U);
}

TEST(GameClientPredictionTest, AckDiagnosticsDistinguishRejectedReceiptFromAcceptedProgress)
{
    PredictionClient client;
    client.setLocalCharacter('@');
    auto const accepted_at = std::chrono::steady_clock::time_point{} + std::chrono::seconds{1};
    auto const rejected_at = accepted_at + std::chrono::seconds{1};
    ASSERT_TRUE(client.applyServerPosition(position('@', 2U, 0U, 7U, 10U), accepted_at));
    ASSERT_FALSE(client.applyServerPosition(position('@', 9U, 0U, 99U, 9U), rejected_at));

    EXPECT_EQ(client.m_last_received_input_ack, 99U);
    EXPECT_EQ(client.m_last_received_position_revision, 9U);
    EXPECT_EQ(client.m_last_received_position_at, rejected_at);
    EXPECT_FALSE(client.m_last_received_position_accepted);
    EXPECT_EQ(client.m_last_accepted_input_ack, 7U);
    EXPECT_EQ(client.m_last_accepted_position_revision, 10U);
    EXPECT_EQ(client.m_last_accepted_position_at, accepted_at);
    EXPECT_EQ(client.m_rejected_local_position_count, 1U);
    auto const authoritative = client.authoritativePlayer('@');
    ASSERT_TRUE(authoritative.has_value());
    EXPECT_EQ(authoritative->x, 2U);

    ASSERT_TRUE(client.applyServerPosition(position('@', 3U, 0U, 8U, 11U), rejected_at));
    EXPECT_TRUE(client.m_last_received_position_accepted);
    EXPECT_EQ(client.m_last_accepted_input_ack, 8U);
    EXPECT_EQ(client.m_rejected_local_position_count, 1U);
}

TEST(GameClientPredictionTest, ForeignPositionCannotOverwriteLocalAckDiagnostics)
{
    PredictionClient client;
    client.setLocalCharacter('@');
    auto const local_at = std::chrono::steady_clock::time_point{} + std::chrono::seconds{1};
    ASSERT_TRUE(client.applyServerPosition(position('@', 2U, 0U, 7U, 10U), local_at));
    ASSERT_TRUE(client.applyServerPosition(position('#', 9U, 0U, 99U, 20U), local_at + std::chrono::seconds{1}));
    ASSERT_FALSE(client.applyServerPosition(position('#', 8U, 0U, 98U, 19U), local_at + std::chrono::seconds{2}));

    EXPECT_EQ(client.m_last_received_input_ack, 7U);
    EXPECT_EQ(client.m_last_received_position_at, local_at);
    EXPECT_EQ(client.m_last_accepted_input_ack, 7U);
    EXPECT_TRUE(client.m_last_received_position_accepted);
    EXPECT_EQ(client.m_rejected_local_position_count, 0U);
}

TEST(GameClientPredictionTest, ReconnectResetClearsAckDiagnosticsBeforeNewSession)
{
    PredictionClient client;
    client.setLocalCharacter('@');
    auto const old_at = std::chrono::steady_clock::time_point{} + std::chrono::seconds{1};
    ASSERT_TRUE(client.applyServerPosition(position('@', 2U, 0U, 7U, 10U), old_at));
    ASSERT_FALSE(client.applyServerPosition(position('@', 9U, 0U, 99U, 9U), old_at));
    client.resetConnectionState();

    EXPECT_EQ(client.m_last_received_position_at, std::chrono::steady_clock::time_point{});
    EXPECT_EQ(client.m_last_accepted_position_at, std::chrono::steady_clock::time_point{});
    EXPECT_EQ(client.m_last_received_input_ack, 0U);
    EXPECT_EQ(client.m_last_accepted_input_ack, 0U);
    EXPECT_FALSE(client.m_last_received_position_accepted);
    EXPECT_EQ(client.m_rejected_local_position_count, 0U);
    client.setLocalCharacter('@');
    ASSERT_TRUE(client.applyServerPosition(position('@', 1U, 0U, 1U, 1U), old_at + std::chrono::seconds{1}));
    EXPECT_TRUE(client.m_last_received_position_accepted);
    EXPECT_EQ(client.m_last_accepted_input_ack, 1U);
}

TEST(GameClientPredictionTest, RejectsStaleRemoteStatesAndAllowsRevisionWrapAfterRemoval)
{
    PredictionClient client;
    client.setLocalCharacter('@');
    ASSERT_TRUE(client.applyServerPosition(position('@', 0U, 0U, 0U, 1U)));
    ASSERT_TRUE(client.applyServerPosition(position('#', 2U, 0U, 0U, 5U)));
    EXPECT_FALSE(client.applyServerPosition(position('#', 10U, 0U, 0U, 5U)));
    EXPECT_FALSE(client.applyServerPosition(position('#', 10U, 0U, 0U, 4U)));
    auto const stale_authoritative_player = client.authoritativePlayer('#');
    ASSERT_TRUE(stale_authoritative_player.has_value());
    EXPECT_EQ(stale_authoritative_player->x, 2U);

    client.applyServerRemoval('#');
    EXPECT_FALSE(client.authoritativePlayer('#').has_value());
    EXPECT_TRUE(client.applyServerPosition(position('#', 10U, 0U, 0U, 1U)));
    client.applyServerRemoval('#');
    EXPECT_TRUE(client.applyServerPosition(position('#', 10U, 0U, 0U, UINT32_MAX - 1U)));
    EXPECT_TRUE(client.applyServerPosition(position('#', 11U, 0U, 0U, UINT32_MAX)));
    EXPECT_TRUE(client.applyServerPosition(position('#', 12U, 0U, 0U, 0U)));
    auto const wrapped_authoritative_player = client.authoritativePlayer('#');
    ASSERT_TRUE(wrapped_authoritative_player.has_value());
    EXPECT_EQ(wrapped_authoritative_player->x, 12U);
}

TEST(GameClientPredictionTest, ReconcilesThreeProductionCadenceClientsOverRealTransport)
{
    static constexpr std::chrono::seconds CLIENT_DEADLINE{ 15 };
    shared::WorldConfiguration const world_configuration = shared::World::canonicalConfiguration();
    auto preparation = boundedTestPreparation(world_configuration);
    server::GameServer server{
        0U,
        {},
        shared::WorldMode::Flight,
        world_configuration,
        shared::HEIGHT_TILE_INTEREST_RADIUS,
        nullptr,
        preparation,
    };
    std::atomic_bool stop_server{ false };
    std::thread server_thread{ [&server, &stop_server] {
        server.run(stop_server);
    } };

    auto const deadline = std::chrono::steady_clock::now() + CLIENT_DEADLINE;
    ProductionPredictionClient alice{ { .x = 127U, .y = 0U }, deadline };
    ProductionPredictionClient bob{ { .x = 129U, .y = 0U }, deadline };
    ProductionPredictionClient charlie{ { .x = 0U, .y = 127U }, deadline };
    std::thread alice_thread{ [&alice, &server] {
        alice.runWithPreparedStart(core::Address::localhost(server.port()), '@');
    } };
    std::thread bob_thread{ [&bob, &server] {
        bob.runWithPreparedStart(core::Address::localhost(server.port()), '#');
    } };
    std::thread charlie_thread{ [&charlie, &server] {
        charlie.runWithPreparedStart(core::Address::localhost(server.port()), '$');
    } };

    alice_thread.join();
    bob_thread.join();
    charlie_thread.join();
    stop_server.store(true, std::memory_order_relaxed);
    server_thread.join();

    auto const expect_reconciled = [](ProductionPredictionClient const& client) {
        SCOPED_TRACE(client.reconciliationDiagnostic());
        EXPECT_TRUE(client.completed());
        EXPECT_FALSE(client.deadlineExpired());
        EXPECT_EQ(client.inputsSent(), ProductionPredictionClient::FIXED_INPUT_COUNT);
        EXPECT_GE(client.highestLocalRevision(), ProductionPredictionClient::FIXED_INPUT_COUNT + 1U);
        EXPECT_TRUE(client.observedAllPlayers());
        EXPECT_TRUE(client.movementInputsAcknowledged());
        EXPECT_TRUE(client.pendingInputsAreNeutral());
        EXPECT_TRUE(client.predictedPositionMatchesAuthoritative());
        ASSERT_TRUE(client.initialAuthoritativeLocalPlayer().has_value());
        ASSERT_TRUE(client.authoritativeLocalPlayer().has_value());
        ASSERT_TRUE(client.predictedLocalPlayerForTest().has_value());
        shared::Player const authoritative = *client.authoritativeLocalPlayer();
        shared::Player const predicted = *client.predictedLocalPlayerForTest();
        EXPECT_EQ(predicted.id, authoritative.id);
        EXPECT_EQ(predicted.x, authoritative.x);
        EXPECT_EQ(predicted.y, authoritative.y);
        EXPECT_EQ(predicted.z, authoritative.z);
        EXPECT_EQ(predicted.x_subcell, authoritative.x_subcell);
        EXPECT_EQ(predicted.y_subcell, authoritative.y_subcell);
        EXPECT_EQ(predicted.z_subcell, authoritative.z_subcell);
        EXPECT_EQ(predicted.ch, authoritative.ch);
    };
    expect_reconciled(alice);
    expect_reconciled(bob);
    expect_reconciled(charlie);

    ASSERT_TRUE(alice.initialAuthoritativeLocalPlayer().has_value());
    ASSERT_TRUE(bob.initialAuthoritativeLocalPlayer().has_value());
    ASSERT_TRUE(charlie.initialAuthoritativeLocalPlayer().has_value());
    ASSERT_TRUE(alice.authoritativeLocalPlayer().has_value());
    ASSERT_TRUE(bob.authoritativeLocalPlayer().has_value());
    ASSERT_TRUE(charlie.authoritativeLocalPlayer().has_value());
    EXPECT_TRUE(
        alice.authoritativeLocalPlayer()->x != alice.initialAuthoritativeLocalPlayer()->x
        || alice.authoritativeLocalPlayer()->x_subcell != alice.initialAuthoritativeLocalPlayer()->x_subcell
    );
    EXPECT_TRUE(
        bob.authoritativeLocalPlayer()->x != bob.initialAuthoritativeLocalPlayer()->x
        || bob.authoritativeLocalPlayer()->x_subcell != bob.initialAuthoritativeLocalPlayer()->x_subcell
    );
    EXPECT_TRUE(
        charlie.authoritativeLocalPlayer()->y != charlie.initialAuthoritativeLocalPlayer()->y
        || charlie.authoritativeLocalPlayer()->y_subcell != charlie.initialAuthoritativeLocalPlayer()->y_subcell
    );
}

TEST(GameClientPredictionTest, RetriesUntilAnInitiallyUnavailableServerStarts)
{
    static constexpr std::chrono::milliseconds SERVER_START_DELAY{ 500 };
    static constexpr std::chrono::seconds CLIENT_DEADLINE{ 5 };
    uint16_t port = 0U;
    {
        PortReservation const reservation;
        port = reservation.port();
    }
    std::atomic_bool stop_server{ false };
    DelayedServerClient client{ std::chrono::steady_clock::now() + CLIENT_DEADLINE };
    std::thread client_thread{ [&client, port] {
        client.run(core::Address::localhost(port), '@');
    } };

    std::this_thread::sleep_for(SERVER_START_DELAY);
    server::GameServer server{ port };
    std::thread server_thread{ [&server, &stop_server] {
        server.run(stop_server);
    } };

    client_thread.join();
    stop_server.store(true, std::memory_order_relaxed);
    server_thread.join();

    EXPECT_TRUE(client.joined());
    EXPECT_FALSE(client.deadlineExpired());
}

} // namespace
