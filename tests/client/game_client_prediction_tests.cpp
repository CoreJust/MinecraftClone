#include <client/GameClient.hpp>

#include <server/GameServer.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>

namespace {

class PredictionClient final : public client::GameClient {
public:
    using GameClient::applyServerPosition;
    using GameClient::applyServerRemoval;
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
        std::chrono::steady_clock::time_point const deadline,
        std::optional<char> const departed_character = std::nullopt
    )
        : m_direction(direction)
        , m_deadline(deadline)
        , m_departed_character(departed_character)
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

    [[nodiscard]] bool observedDeparture() const noexcept { return m_observed_departure; }

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
        if (!m_completed && m_inputs_sent == FIXED_INPUT_COUNT
            && m_highest_local_revision >= FIXED_INPUT_COUNT + 1U
            && m_pending_inputs.empty()
            && observedAllPlayers()) {
            m_completed = true;
            if (!m_departed_character) {
                m_running = false;
            }
        }
        if (m_completed && m_departed_character
            && !m_world.playerByCharacter(*m_departed_character).has_value()) {
            m_observed_departure = true;
            m_running = false;
        }
    }

private:
    shared::Direction m_direction;
    std::chrono::steady_clock::time_point m_deadline;
    std::optional<char> m_departed_character;
    std::array<bool, PLAYERS.size()> m_seen_players{ };
    uint32_t m_inputs_sent = 0;
    uint32_t m_highest_local_revision = 0;
    bool m_completed = false;
    bool m_observed_departure = false;
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
    EXPECT_EQ(client.predictedLocalPlayer()->x, 1U);
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

    auto alice = std::make_unique<ProductionPredictionClient>(shared::Direction{ .x = 127U, .y = 0U }, deadline);
    ProductionPredictionClient bob{ { .x = 129U, .y = 0U }, deadline, '@' };
    ProductionPredictionClient charlie{ { .x = 0U, .y = 127U }, deadline, '@' };
    std::thread alice_thread{ [&alice, &server] {
        alice->run(core::Address::localhost(server.port()), '@');
    } };
    std::thread bob_thread{ [&bob, &server] {
        bob.run(core::Address::localhost(server.port()), '#');
    } };
    std::thread charlie_thread{ [&charlie, &server] {
        charlie.run(core::Address::localhost(server.port()), '$');
    } };

    alice_thread.join();
    bool const alice_completed = alice->completed();
    bool const alice_deadline_expired = alice->deadlineExpired();
    bool const alice_pending_inputs = alice->hasPendingInputs();
    std::optional<shared::Player> const alice_authoritative = alice->authoritativeLocalPlayer();
    std::optional<shared::Player> const alice_predicted = alice->predictedLocalPlayerForTest();
    alice.reset();
    bob_thread.join();
    charlie_thread.join();
    stop_server.store(true, std::memory_order_relaxed);
    server_thread.join();
    EXPECT_TRUE(bob.observedDeparture());
    EXPECT_TRUE(charlie.observedDeparture());
    EXPECT_TRUE(alice_completed);
    EXPECT_FALSE(alice_deadline_expired);
    EXPECT_FALSE(alice_pending_inputs);
    ASSERT_TRUE(alice_authoritative.has_value());
    ASSERT_TRUE(alice_predicted.has_value());
    EXPECT_EQ(alice_predicted->id, alice_authoritative->id);
    EXPECT_EQ(alice_predicted->x, alice_authoritative->x);
    EXPECT_EQ(alice_predicted->y, alice_authoritative->y);
    EXPECT_EQ(alice_predicted->x_subcell, alice_authoritative->x_subcell);
    EXPECT_EQ(alice_predicted->y_subcell, alice_authoritative->y_subcell);
    EXPECT_EQ(alice_predicted->ch, alice_authoritative->ch);

    ASSERT_TRUE(bob.authoritativeLocalPlayer().has_value());
    ASSERT_TRUE(charlie.authoritativeLocalPlayer().has_value());
    EXPECT_NE(bob.authoritativeLocalPlayer()->x, BOB_SPAWN.x);
    EXPECT_NE(charlie.authoritativeLocalPlayer()->y, CHARLIE_SPAWN.y);
}

} // namespace
