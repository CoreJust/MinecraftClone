#include <client/GameClient.hpp>

#include <server/GameServer.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <optional>

namespace {

class PredictionClient final : public client::GameClient {
public:
    using GameClient::applyServerPosition;
    using GameClient::applyServerRemoval;
    using GameClient::discardPredictedInput;
    using GameClient::predictInput;
    using GameClient::predictedLocalPlayer;

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
        2U * shared::MOVEMENT_SUBCELLS_PER_TICK,
        2U,
        3U
    )));
    ASSERT_TRUE(client.predictedLocalPlayer().has_value());
    EXPECT_EQ(client.predictedLocalPlayer()->x_subcell, predicted_before_acknowledgement);
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

} // namespace
