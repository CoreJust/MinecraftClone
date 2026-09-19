#include <client/GameClient.hpp>

#include <server/GameServer.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

namespace {

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
    using GameClient::discardPredictedInput;
    using GameClient::predictInput;
    using GameClient::predictedLocalPlayer;
    using GameClient::predictedLocalPresentation;

    void setLocalCharacter(char const character) noexcept
    {
        m_local_character = character;
    }

    [[nodiscard]] std::optional<shared::Player> authoritativePlayer(char const character) const noexcept
    {
        return m_world.playerByCharacter(character);
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
        : m_direction(direction)
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

private:
    shared::Direction input() override
    {
        if (!m_world.playerByCharacter(m_local_character).has_value()) {
            return { };
        }
        if (m_inputs_sent < FIXED_INPUT_COUNT) {
            ++m_inputs_sent;
            return m_direction;
        }
        return { };
    }

    void render() override
    {
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
        if (m_inputs_sent == FIXED_INPUT_COUNT
            && m_highest_local_revision >= FIXED_INPUT_COUNT + 1U
            && m_pending_inputs.empty()
            && observedAllPlayers()) {
            m_completed = true;
            m_running = false;
        }
    }

private:
    shared::Direction m_direction;
    std::chrono::steady_clock::time_point m_deadline;
    std::array<bool, PLAYERS.size()> m_seen_players{ };
    uint32_t m_inputs_sent = 0;
    uint32_t m_highest_local_revision = 0;
    bool m_completed = false;
    bool m_deadline_expired = false;
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

TEST(GameClientPredictionTest, PredictsImmediatelyWithoutMutatingTheAuthoritativeWorld)
{
    static constexpr shared::Direction RIGHT{ .x = 127U, .y = 0U };
    PredictionClient client;
    client.setLocalCharacter('@');
    ASSERT_TRUE(client.applyServerPosition(position('@', 0U, 0U, 0U, 1U)));

    std::optional<shared::ClientInputMessage> const input = client.predictInput(RIGHT);

    ASSERT_TRUE(input.has_value());
    ASSERT_TRUE(client.authoritativePlayer('@').has_value());
    ASSERT_TRUE(client.predictedLocalPlayer().has_value());
    EXPECT_EQ(input->sequence, 1U);
    EXPECT_EQ(client.authoritativePlayer('@')->x_subcell, 0U);
    EXPECT_EQ(client.predictedLocalPlayer()->x_subcell, shared::MOVEMENT_SUBCELLS_PER_TICK);
}

TEST(GameClientPredictionTest, HeightTileRemovalRejectsStaleTileAndEvictsTheResidentTile)
{
    static constexpr shared::HeightTileKey KEY{ .x = 41, .y = 23 };
    static constexpr uint64_t INITIAL_TOKEN = 4U;
    static constexpr uint64_t REMOVAL_TOKEN = 5U;
    PredictionClient client;
    shared::ServerHeightTileMessage const tile{
        .key = KEY,
        .revision = 1U,
        .token = INITIAL_TOKEN,
    };

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
    client.applyHeightTileBatch({
        .tiles = {
            {
                .key = FIRST_KEY,
                .revision = 1U,
                .token = 4U,
            },
            {
                .key = SECOND_KEY,
                .revision = 1U,
                .token = 5U,
            },
        },
    });

    EXPECT_NE(client.heightTileResidency().resident(FIRST_KEY), nullptr);
    EXPECT_NE(client.heightTileResidency().resident(SECOND_KEY), nullptr);
}

TEST(GameClientPredictionTest, HeightTileResidencyEvictsWithinItsBoundedHysteresisBudget)
{
    static constexpr client::HeightTileRevision REVISION{ .generation = 1U, .revision = 1U };
    static constexpr client::PreviewResidencyLimits LIMITS{
        .max_resident_tiles = 2U,
        .max_resident_bytes = 2U * shared::HEIGHT_TILE_PAYLOAD_BYTES,
    };
    static constexpr shared::HeightTileKey FIRST_KEY{ .x = 1, .y = 1 };
    static constexpr shared::HeightTileKey SECOND_KEY{ .x = 2, .y = 1 };
    static constexpr shared::HeightTileKey THIRD_KEY{ .x = 3, .y = 1 };
    client::PreviewResidency residency{ REVISION, LIMITS };

    ASSERT_EQ(
        residency.accept(FIRST_KEY, REVISION, 1U, {}).replacement,
        client::HeightTileReplacement::Published
    );
    ASSERT_EQ(
        residency.accept(SECOND_KEY, REVISION, 2U, {}).replacement,
        client::HeightTileReplacement::Published
    );
    ASSERT_EQ(
        residency.accept(THIRD_KEY, REVISION, 3U, {}).replacement,
        client::HeightTileReplacement::Published
    );

    EXPECT_EQ(residency.stats().resident_tiles, LIMITS.max_resident_tiles);
    EXPECT_EQ(residency.resident(FIRST_KEY), nullptr);
    EXPECT_NE(residency.resident(THIRD_KEY), nullptr);
    EXPECT_FALSE(residency.evict(FIRST_KEY, REVISION, 1U));
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
    ASSERT_TRUE(client.authoritativePlayer('@').has_value());
    ASSERT_TRUE(client.predictedLocalPlayer().has_value());
    EXPECT_EQ(client.authoritativePlayer('@')->z, 12);
    EXPECT_EQ(client.authoritativePlayer('@')->z_subcell, 0U);
    EXPECT_EQ(client.predictedLocalPlayer()->z, 12);
    EXPECT_EQ(client.predictedLocalPlayer()->z_subcell, shared::MOVEMENT_SUBCELLS_PER_TICK);

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
    ASSERT_TRUE(client.predictedLocalPresentation(STARTED_AT + std::chrono::milliseconds{ 150 }).has_value());
    EXPECT_DOUBLE_EQ(
        client.predictedLocalPresentation(STARTED_AT + std::chrono::milliseconds{ 150 })->z,
        12.0 + static_cast<double>(shared::MOVEMENT_SUBCELLS_PER_TICK)
            / static_cast<double>(shared::SUBCELLS_PER_CELL)
    );
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

    ASSERT_TRUE(sampled_each_frame.predictedLocalPresentation(STARTED_AT + std::chrono::milliseconds{ 25 }).has_value());
    ASSERT_TRUE(sampled_each_frame.predictedLocalPresentation(STARTED_AT + std::chrono::milliseconds{ 50 }).has_value());
    ASSERT_TRUE(sampled_each_frame.predictedLocalPresentation(STARTED_AT + std::chrono::milliseconds{ 75 }).has_value());
    EXPECT_DOUBLE_EQ(
        sampled_each_frame.predictedLocalPresentation(STARTED_AT + std::chrono::milliseconds{ 25 })->x,
        STEP * 0.25
    );
    EXPECT_DOUBLE_EQ(
        sampled_each_frame.predictedLocalPresentation(STARTED_AT + std::chrono::milliseconds{ 50 })->x,
        STEP * 0.5
    );
    EXPECT_DOUBLE_EQ(
        sampled_each_frame.predictedLocalPresentation(STARTED_AT + std::chrono::milliseconds{ 75 })->x,
        STEP * 0.75
    );
    ASSERT_TRUE(sampled_once.predictedLocalPresentation(STARTED_AT + std::chrono::milliseconds{ 75 }).has_value());
    EXPECT_DOUBLE_EQ(
        sampled_each_frame.predictedLocalPresentation(STARTED_AT + std::chrono::milliseconds{ 75 })->x,
        sampled_once.predictedLocalPresentation(STARTED_AT + std::chrono::milliseconds{ 75 })->x
    );

    ASSERT_TRUE(sampled_each_frame.applyServerPosition(
        position('@', 0U, shared::MOVEMENT_SUBCELLS_PER_TICK, 1U, 2U),
        STARTED_AT + std::chrono::milliseconds{ 50 }
    ));
    ASSERT_TRUE(sampled_each_frame.predictedLocalPresentation(STARTED_AT + std::chrono::milliseconds{ 75 }).has_value());
    EXPECT_DOUBLE_EQ(
        sampled_each_frame.predictedLocalPresentation(STARTED_AT + std::chrono::milliseconds{ 75 })->x,
        STEP * 0.75
    );
    ASSERT_TRUE(sampled_each_frame.predictedLocalPresentation(STARTED_AT + std::chrono::milliseconds{ 200 }).has_value());
    EXPECT_DOUBLE_EQ(
        sampled_each_frame.predictedLocalPresentation(STARTED_AT + std::chrono::milliseconds{ 200 })->x,
        STEP
    );

    client::CameraPose const frame_camera = client::localPlayerThirdPersonPose(
        *sampled_each_frame.predictedLocalPresentation(STARTED_AT + std::chrono::milliseconds{ 75 }),
        { }
    );
    EXPECT_DOUBLE_EQ(frame_camera.position.x, 1.0 + STEP * 0.75);
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
    EXPECT_NEAR(client::localPlayerFirstPersonPose(*presented, {}).position.x, 1.0, 1e-9);
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

TEST(GameClientPredictionTest, RejectsStaleRemoteStatesAndAllowsRevisionWrapAfterRemoval)
{
    PredictionClient client;
    client.setLocalCharacter('@');
    ASSERT_TRUE(client.applyServerPosition(position('@', 0U, 0U, 0U, 1U)));
    ASSERT_TRUE(client.applyServerPosition(position('#', 2U, 0U, 0U, 5U)));
    EXPECT_FALSE(client.applyServerPosition(position('#', 10U, 0U, 0U, 5U)));
    EXPECT_FALSE(client.applyServerPosition(position('#', 10U, 0U, 0U, 4U)));
    ASSERT_TRUE(client.authoritativePlayer('#').has_value());
    EXPECT_EQ(client.authoritativePlayer('#')->x, 2U);

    client.applyServerRemoval('#');
    EXPECT_FALSE(client.authoritativePlayer('#').has_value());
    EXPECT_TRUE(client.applyServerPosition(position('#', 10U, 0U, 0U, 1U)));
    client.applyServerRemoval('#');
    EXPECT_TRUE(client.applyServerPosition(position('#', 10U, 0U, 0U, UINT32_MAX - 1U)));
    EXPECT_TRUE(client.applyServerPosition(position('#', 11U, 0U, 0U, UINT32_MAX)));
    EXPECT_TRUE(client.applyServerPosition(position('#', 12U, 0U, 0U, 0U)));
    ASSERT_TRUE(client.authoritativePlayer('#').has_value());
    EXPECT_EQ(client.authoritativePlayer('#')->x, 12U);
}

TEST(GameClientPredictionTest, ReconcilesThreeProductionCadenceClientsOverRealTransport)
{
    static constexpr std::chrono::seconds CLIENT_DEADLINE{ 4 };
    static constexpr server::GameServer::SpawnPoint ALICE_SPAWN{
        .character = '@',
        .x = 2U,
        .y = 2U,
    };
    static constexpr server::GameServer::SpawnPoint BOB_SPAWN{
        .character = '#',
        .x = 12U,
        .y = 10U,
    };
    static constexpr server::GameServer::SpawnPoint CHARLIE_SPAWN{
        .character = '$',
        .x = 22U,
        .y = 18U,
    };
    auto const deadline = std::chrono::steady_clock::now() + CLIENT_DEADLINE;
    server::GameServer server{ 0, { ALICE_SPAWN, BOB_SPAWN, CHARLIE_SPAWN } };
    std::atomic_bool stop_server{ false };
    std::thread server_thread{ [&server, &stop_server] {
        server.run(stop_server);
    } };

    ProductionPredictionClient alice{ { .x = 127U, .y = 0U }, deadline };
    ProductionPredictionClient bob{ { .x = 129U, .y = 0U }, deadline };
    ProductionPredictionClient charlie{ { .x = 0U, .y = 127U }, deadline };
    std::thread alice_thread{ [&alice, &server] {
        alice.run(core::Address::localhost(server.port()), '@');
    } };
    std::thread bob_thread{ [&bob, &server] {
        bob.run(core::Address::localhost(server.port()), '#');
    } };
    std::thread charlie_thread{ [&charlie, &server] {
        charlie.run(core::Address::localhost(server.port()), '$');
    } };

    alice_thread.join();
    bob_thread.join();
    charlie_thread.join();
    stop_server.store(true, std::memory_order_relaxed);
    server_thread.join();

    auto const expect_reconciled = [](ProductionPredictionClient const& client) {
        EXPECT_TRUE(client.completed());
        EXPECT_FALSE(client.deadlineExpired());
        EXPECT_EQ(client.inputsSent(), ProductionPredictionClient::FIXED_INPUT_COUNT);
        EXPECT_GE(client.highestLocalRevision(), ProductionPredictionClient::FIXED_INPUT_COUNT + 1U);
        EXPECT_TRUE(client.observedAllPlayers());
        EXPECT_FALSE(client.hasPendingInputs());
        ASSERT_TRUE(client.authoritativeLocalPlayer().has_value());
        ASSERT_TRUE(client.predictedLocalPlayerForTest().has_value());
        shared::Player const authoritative = *client.authoritativeLocalPlayer();
        shared::Player const predicted = *client.predictedLocalPlayerForTest();
        EXPECT_EQ(predicted.id, authoritative.id);
        EXPECT_EQ(predicted.x, authoritative.x);
        EXPECT_EQ(predicted.y, authoritative.y);
        EXPECT_EQ(predicted.x_subcell, authoritative.x_subcell);
        EXPECT_EQ(predicted.y_subcell, authoritative.y_subcell);
        EXPECT_EQ(predicted.ch, authoritative.ch);
    };
    expect_reconciled(alice);
    expect_reconciled(bob);
    expect_reconciled(charlie);

    ASSERT_TRUE(alice.authoritativeLocalPlayer().has_value());
    ASSERT_TRUE(bob.authoritativeLocalPlayer().has_value());
    ASSERT_TRUE(charlie.authoritativeLocalPlayer().has_value());
    EXPECT_NE(alice.authoritativeLocalPlayer()->x, ALICE_SPAWN.x);
    EXPECT_NE(bob.authoritativeLocalPlayer()->x, BOB_SPAWN.x);
    EXPECT_NE(charlie.authoritativeLocalPlayer()->y, CHARLIE_SPAWN.y);
}

} // namespace
