#include <client/ClientAudio.hpp>
#include <client/GameClient.hpp>

#include <server/GameServer.hpp>

#include <shared/world/World.hpp>

#include <generation/Generation.hpp>

#include <core/net/Address.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct AudioOutputState final {
    uint32_t start_count = 0U;
    uint32_t play_count = 0U;
    uint32_t stop_count = 0U;
};

class FakeAudioOutput final : public client::ClientAudioOutput {
public:
    explicit FakeAudioOutput(std::shared_ptr<AudioOutputState> state)
        : m_state{ std::move(state) }
    { }

    [[nodiscard]] bool start() noexcept override
    {
        ++m_state->start_count;
        return true;
    }

    void stop() noexcept override
    {
        ++m_state->stop_count;
    }

    [[nodiscard]] bool play(std::span<int16_t const> samples) noexcept override
    {
        if (!samples.empty()) {
            ++m_state->play_count;
        }
        return !samples.empty();
    }

private:
    std::shared_ptr<AudioOutputState> m_state;
};

class AudioIntegrationClient final : public client::GameClient {
public:
    explicit AudioIntegrationClient(std::shared_ptr<AudioOutputState> state, bool const forward_sounds = false)
        : GameClient{
            shared::WorldMode::Flat,
            shared::World::canonicalConfiguration(),
            false,
            std::make_unique<FakeAudioOutput>(state),
        }
        , m_audio_state{ std::move(state) }
        , m_forward_sounds{ forward_sounds }
    { }

private:
    [[nodiscard]] shared::Direction input() override
    {
        return {};
    }

    void render() override
    {
        if (m_forward_sounds && !m_sounds_forwarded && m_audio_state->play_count == 1U) {
            updateClientCreationAudio(client::CreationAudioStage::Molten, 1.0);
            updateClientCreationAudio(client::CreationAudioStage::Molten, 1.5);
            updateClientCreationAudio(client::CreationAudioStage::Water, 1.5);
            static_cast<void>(playClientBlockSound(client::BlockSoundKind::Chip, shared::Block::Dirt));
            static_cast<void>(playClientBlockSound(client::BlockSoundKind::Break, shared::Block::Stone));
            static_cast<void>(playClientBlockSound(client::BlockSoundKind::Place, shared::Block::Ice));
            static_cast<void>(playClientBlockSound(client::BlockSoundKind::Footstep, shared::Block::Grass));
            m_sounds_forwarded = true;
        }
        if (m_audio_state->play_count > 0U) {
            m_running = false;
        }
    }

    std::shared_ptr<AudioOutputState> m_audio_state;
    bool m_forward_sounds = false;
    bool m_sounds_forwarded = false;
};

} // namespace

TEST(GameClientAudioTest, PlaysJoinCueOnceAfterServerAcceptsClient)
{
    static constexpr std::chrono::seconds CLIENT_DEADLINE{ 3 };
    server::GameServer::SpawnPoint const spawn{
        .character = '@',
        .x = 2U,
        .y = 2U,
    };
    server::GameServer server{ 0, { spawn } };
    std::atomic_bool stop_server{ false };
    std::thread server_thread{ [&server, &stop_server] {
        server.run(stop_server);
    } };

    auto state = std::make_shared<AudioOutputState>();
    AudioIntegrationClient game_client{ state };
    client::GameClientBenchmarkHooks hooks{
        .deadline = std::chrono::steady_clock::now() + CLIENT_DEADLINE,
        .should_stop = {},
        .is_uncapped_phase = {},
        .input_override = {},
        .start_input_override = {},
        .on_loop = {},
        .on_authoritative_player = {},
        .on_input_sent = {},
        .on_authoritative_state = {},
    };
    game_client.run(core::Address::localhost(server.port()), '@', &hooks);
    stop_server.store(true, std::memory_order_relaxed);
    server_thread.join();

    EXPECT_EQ(state->start_count, 1U);
    EXPECT_EQ(state->play_count, 1U);
    EXPECT_EQ(state->stop_count, 1U);
}

TEST(GameClientAudioTest, ForwardsCreationStagesAndMaterialSoundsToFakeOutput)
{
    static constexpr std::chrono::seconds CLIENT_DEADLINE{ 3 };
    server::GameServer server{ 0, {{ .character = '@', .x = 2U, .y = 2U }} };
    std::atomic_bool stop_server{ false };
    std::thread server_thread{ [&server, &stop_server] {
        server.run(stop_server);
    } };

    auto state = std::make_shared<AudioOutputState>();
    AudioIntegrationClient game_client{ state, true };
    client::GameClientBenchmarkHooks hooks{
        .deadline = std::chrono::steady_clock::now() + CLIENT_DEADLINE,
        .should_stop = {},
        .is_uncapped_phase = {},
        .input_override = {},
        .start_input_override = {},
        .on_loop = {},
        .on_authoritative_player = {},
        .on_input_sent = {},
        .on_authoritative_state = {},
    };
    game_client.run(core::Address::localhost(server.port()), '@', &hooks);
    stop_server.store(true, std::memory_order_relaxed);
    server_thread.join();

    EXPECT_EQ(state->start_count, 1U);
    EXPECT_EQ(state->play_count, 7U);
    EXPECT_EQ(state->stop_count, 1U);
}

namespace {

class BatchedLandingAudioClient final : public client::GameClient {
public:
    explicit BatchedLandingAudioClient(std::shared_ptr<AudioOutputState> const& state)
        : GameClient{shared::WorldMode::Flight, shared::World::canonicalConfiguration(), false,
            std::make_unique<FakeAudioOutput>(state)}
    { }

    bool prepare()
    {
        m_local_character = '@';
        m_accepted = true;
        m_world_loading_identity.join_epoch = 1U;
        generation::GenerationConfiguration configuration;
        configuration.seed = m_world.configuration().seed;
        m_terrain_session = shared::TerrainSessionIdentity{
            .join_epoch = 1U,
            .terrain = {.terrain_version = generation::terrainVersion(configuration.model_tag),
                .revision = 1U, .seed = configuration.seed,
                .configuration_identity = generation::configurationIdentity(configuration)},
            .configuration = configuration,
        };
        m_block_action_status = shared::ServerBlockActionResultMessage{
            .session = *m_terrain_session, .mode = shared::GameMode::Survival};
        m_authoritative_world = std::make_unique<shared::SparseWorld>(
            shared::SparseWorldOptions{.seed = configuration.seed},
            shared::SparseWorldGenerationMode::MaterializedOnly);
        shared::Chunk support{{.x = 32, .y = 32, .z = 0}};
        return support.setBlock({.x = 0U, .y = 0U, .z = 10U}, shared::Block::Stone)
            && m_authoritative_world->publishMaterializedChunk(support, 1U, configuration.seed)
            && m_client_audio.start();
    }

    bool accept(shared::ServerPlayerPositionMessage const& message)
    {
        return applyServerPosition(message);
    }

    bool changeMode(shared::GameMode const mode)
    {
        return applyBlockActionResult({.session = *m_terrain_session,
            .server_tick = ++m_mode_tick, .mode = mode});
    }

    void resetPresentation() { resetLandingTransitions(); }
    void resetJoin() { resetConnectionState(); }
    void present() { render(); }

private:
    shared::Direction input() override { return {}; }
    void render() override
    {
        if (auto const material = consumeLandingMaterial()) {
            static_cast<void>(playClientBlockSound(client::BlockSoundKind::Landing, *material));
        }
    }

    uint64_t m_mode_tick = 0U;
};

shared::ServerPlayerPositionMessage landingPosition(uint32_t const revision, bool const grounded)
{
    return {.ch = '@', .movement_capabilities = {.bits = 0U},
        .x = 512, .y = 512, .z = grounded ? 11 : 12,
        .x_subcell = 0U, .y_subcell = 0U,
        .vertical_velocity_subcells = grounded ? 0 : -1,
        .state_revision = revision};
}

} // namespace

TEST(GameClientAudioTest, RetainsLandingBetweenAcceptedPositionMessages)
{
    auto state = std::make_shared<AudioOutputState>();
    BatchedLandingAudioClient game_client{state};
    ASSERT_TRUE(game_client.prepare());
    // A network poll can accept every pose before the next presentation.
    ASSERT_TRUE(game_client.accept(landingPosition(1U, false)));
    ASSERT_TRUE(game_client.accept(landingPosition(2U, true)));
    ASSERT_TRUE(game_client.accept(landingPosition(3U, false)));
    EXPECT_EQ(state->play_count, 0U);
    game_client.present();
    EXPECT_EQ(state->play_count, 1U);
    game_client.present();
    EXPECT_EQ(state->play_count, 1U);
}

TEST(GameClientAudioTest, BatchedLandingRejectsStaleAndDuplicatePositionMessages)
{
    auto state = std::make_shared<AudioOutputState>();
    BatchedLandingAudioClient game_client{state};
    ASSERT_TRUE(game_client.prepare());
    ASSERT_TRUE(game_client.accept(landingPosition(1U, false)));
    ASSERT_TRUE(game_client.accept(landingPosition(2U, true)));
    ASSERT_TRUE(game_client.accept(landingPosition(3U, false)));
    EXPECT_FALSE(game_client.accept(landingPosition(2U, true)));
    EXPECT_FALSE(game_client.accept(landingPosition(3U, false)));
    game_client.present();
    EXPECT_EQ(state->play_count, 1U);
    game_client.present();
    EXPECT_EQ(state->play_count, 1U);
}

TEST(GameClientAudioTest, BatchedLandingsRetainAtMostOnePendingContact)
{
    auto state = std::make_shared<AudioOutputState>();
    BatchedLandingAudioClient game_client{state};
    ASSERT_TRUE(game_client.prepare());
    for (uint32_t revision = 1U; revision <= 9U; ++revision) {
        ASSERT_TRUE(game_client.accept(landingPosition(revision, revision % 2U == 0U)));
    }
    game_client.present();
    EXPECT_EQ(state->play_count, 1U);
    game_client.present();
    EXPECT_EQ(state->play_count, 1U);
}

TEST(GameClientAudioTest, GroundedJoinAndUnsupportedGroundDoNotQueueLanding)
{
    auto state = std::make_shared<AudioOutputState>();
    BatchedLandingAudioClient game_client{state};
    ASSERT_TRUE(game_client.prepare());
    ASSERT_TRUE(game_client.accept(landingPosition(1U, true)));
    game_client.present();
    EXPECT_EQ(state->play_count, 0U);
    ASSERT_TRUE(game_client.accept(landingPosition(2U, false)));
    auto unknown = landingPosition(3U, true);
    unknown.x = 528;
    ASSERT_TRUE(game_client.accept(unknown));
    game_client.present();
    EXPECT_EQ(state->play_count, 0U);
}

TEST(GameClientAudioTest, FlyingAndSpectatorMessagesDoNotQueueLanding)
{
    for (bool const spectator : {false, true}) {
        auto state = std::make_shared<AudioOutputState>();
        BatchedLandingAudioClient game_client{state};
        ASSERT_TRUE(game_client.prepare());
        if (spectator) {
            ASSERT_TRUE(game_client.changeMode(shared::GameMode::Spectator));
        }
        for (uint32_t revision = 1U; revision <= 3U; ++revision) {
            auto message = landingPosition(revision, revision == 2U);
            if (!spectator) {
                message.movement_capabilities.bits = static_cast<uint8_t>(shared::MovementCapability::Flight);
            }
            ASSERT_TRUE(game_client.accept(message));
        }
        game_client.present();
        EXPECT_EQ(state->play_count, 0U);
    }
}

TEST(GameClientAudioTest, ModeChangeClearsAnUnpresentedLanding)
{
    auto state = std::make_shared<AudioOutputState>();
    BatchedLandingAudioClient game_client{state};
    ASSERT_TRUE(game_client.prepare());
    ASSERT_TRUE(game_client.accept(landingPosition(1U, false)));
    ASSERT_TRUE(game_client.accept(landingPosition(2U, true)));
    ASSERT_TRUE(game_client.changeMode(shared::GameMode::Spectator));
    game_client.present();
    EXPECT_EQ(state->play_count, 0U);
    ASSERT_TRUE(game_client.changeMode(shared::GameMode::Survival));
    ASSERT_TRUE(game_client.accept(landingPosition(3U, true)));
    game_client.present();
    EXPECT_EQ(state->play_count, 0U);
}

TEST(GameClientAudioTest, PresentationAndJoinResetsClearUnpresentedLanding)
{
    for (bool const join : {false, true}) {
        auto state = std::make_shared<AudioOutputState>();
        BatchedLandingAudioClient game_client{state};
        ASSERT_TRUE(game_client.prepare());
        ASSERT_TRUE(game_client.accept(landingPosition(1U, false)));
        ASSERT_TRUE(game_client.accept(landingPosition(2U, true)));
        if (join) {
            game_client.resetJoin();
        } else {
            game_client.resetPresentation();
        }
        game_client.present();
        EXPECT_EQ(state->play_count, 0U);
    }
}
