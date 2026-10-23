#include <client/GameClient.hpp>
#include <client/PlayerClientInputState.hpp>

#include <shared/net/Message.hpp>
#include <shared/world/World.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace client {
namespace {

class InputCreditClient final : public GameClient {
  public:
    InputCreditClient() : GameClient{ shared::WorldMode::Flight }
    {
        m_local_character = '@';
    }

    using GameClient::applyServerPosition;
    using GameClient::predictInput;

  private:
    shared::Direction input() override { return {}; }
    void render() override {}
};

TEST(PlayerClientInputStateTest, BlockedCreditRetainsCyclePressAfterReleaseUntilSubmission)
{
    InputCreditClient client;
    auto const now = std::chrono::steady_clock::now();
    shared::ServerPlayerPositionMessage position{
        .ch = '@',
        .movement_capabilities = { .bits = 3U },
        .x = 32'896,
        .y = 32'768,
        .z = 410,
        .x_subcell = 0U,
        .y_subcell = 0U,
        .state_revision = 1U,
    };
    ASSERT_TRUE(client.applyServerPosition(position, now));
    uint32_t last_sequence = 0U;
    for (uint32_t pending = 0U; pending < 64U; ++pending) {
        auto const predicted = client.predictInput(shared::Direction{}, now);
        ASSERT_TRUE(predicted.has_value());
        last_sequence = predicted->sequence;
    }
    PlayerClientInputState state;
    state.observeCycle(true);
    ASSERT_FALSE(client.predictInput(shared::Direction{ .x = 0U, .y = 0U,
        .cycle_movement_capabilities = state.cycleRequested() },
        now).has_value());
    state.observeCycle(false);
    EXPECT_TRUE(state.cycleRequested());
    ASSERT_FALSE(client.predictInput(shared::Direction{ .x = 0U, .y = 0U,
        .cycle_movement_capabilities = state.cycleRequested() },
        now).has_value());
    position.acknowledged_input_sequence = last_sequence;
    position.state_revision = 2U;
    ASSERT_TRUE(client.applyServerPosition(position, now));
    auto const accepted = client.predictInput(
        shared::Direction{ .x = 0U, .y = 0U,
            .cycle_movement_capabilities = state.cycleRequested() }, now);
    ASSERT_TRUE(accepted.has_value());
    EXPECT_TRUE(accepted->direction.cycle_movement_capabilities);
    state.submitted(accepted->direction);
    EXPECT_FALSE(state.cycleRequested());
}

TEST(PlayerClientInputStateTest, HeldCycleQueuesOnceAndDistinctPressesAreNotCollapsed)
{
    PlayerClientInputState state;
    state.observeCycle(true);
    state.observeCycle(true);
    EXPECT_EQ(state.pendingCycles(), 1U);
    state.submitted(shared::Direction{ .x = 0U, .y = 0U, .cycle_movement_capabilities = true });
    state.observeCycle(true);
    EXPECT_FALSE(state.cycleRequested());
    for (uint32_t press = 0U; press < 3U; ++press) {
        state.observeCycle(false);
        state.observeCycle(true);
    }
    EXPECT_EQ(state.pendingCycles(), 3U);
    state.submitted(shared::Direction{});
    EXPECT_EQ(state.pendingCycles(), 3U);
    for (uint32_t remaining = 3U; remaining != 0U; --remaining) {
        EXPECT_EQ(state.pendingCycles(), remaining);
        state.submitted(shared::Direction{ .x = 0U, .y = 0U, .cycle_movement_capabilities = true });
    }
    EXPECT_FALSE(state.cycleRequested());
}

TEST(PlayerClientInputStateTest, JumpPulseSurvivesBlockedCreditAndUnrelatedSubmissions)
{
    PlayerClientInputState state;
    state.observeJump(true);
    for (uint32_t blocked_sample = 0U; blocked_sample < 64U; ++blocked_sample) {
        state.observeJump(false);
        EXPECT_TRUE(state.jumpRequested(false));
    }
    state.submitted(shared::Direction{});
    EXPECT_TRUE(state.jumpRequested(false));
    state.submitted(shared::Direction{ .x = 0U, .y = 0U, .z = static_cast<uint8_t>(-1) });
    EXPECT_TRUE(state.jumpRequested(false));
    state.submitted(shared::Direction{ .x = 0U, .y = 0U, .z = 1U });
    EXPECT_FALSE(state.jumpRequested(false));
    EXPECT_TRUE(state.jumpRequested(true));
}

TEST(PlayerClientInputStateTest, ResetClearsPendingActionsAndHeldLatch)
{
    PlayerClientInputState state;
    state.observeCycle(true);
    state.observeJump(true);
    state.reset();
    EXPECT_FALSE(state.cycleRequested());
    EXPECT_FALSE(state.jumpRequested(false));
    state.observeCycle(true);
    EXPECT_TRUE(state.cycleRequested());
}

} // namespace
} // namespace client
