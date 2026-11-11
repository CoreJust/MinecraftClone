#include <client/CameraController.hpp>
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

TEST(PlayerClientInputStateTest, VerticalChordReevaluatesBothKeysInEitherReleaseOrder)
{
    PlayerClientInputState state;
    auto const vertical = [&state](bool const space_held, bool const shift_held) {
        state.observeJump(space_held, shift_held);
        int8_t const direction = CameraController::verticalMovement(
            state.jumpRequested(space_held), shift_held
        );
        state.submitted(shared::Direction{ .x = 0U, .y = 0U, .z = static_cast<uint8_t>(direction) });
        return direction;
    };

    EXPECT_EQ(vertical(true, false), 127);
    EXPECT_EQ(vertical(true, true), 0);
    EXPECT_EQ(vertical(false, true), -127);
    EXPECT_EQ(vertical(false, false), 0);

    state.reset();
    EXPECT_EQ(vertical(false, true), -127);
    EXPECT_EQ(vertical(true, true), 0);
    EXPECT_EQ(vertical(true, false), 127);
    EXPECT_EQ(vertical(false, false), 0);

    state.reset(); // Focus loss discards any queued jump before keys are resampled.
    EXPECT_EQ(vertical(false, true), -127);
    EXPECT_EQ(vertical(false, false), 0);
}

TEST(PlayerClientInputStateTest, HeldBreakIntentResumesOnlyAfterTargetReturns)
{
    PlayerClientInputState state;

    EXPECT_FALSE(state.observeBreakTarget(true, true));
    EXPECT_FALSE(state.observeBreakTarget(true, false));
    EXPECT_TRUE(state.observeBreakTarget(true, true));
    EXPECT_FALSE(state.observeBreakTarget(false, false));
    EXPECT_FALSE(state.observeBreakTarget(true, true));

    EXPECT_FALSE(state.observeBreakTarget(true, false));
    state.cancelBreakTargetIntent();
    EXPECT_FALSE(state.observeBreakTarget(true, true));
}

TEST(PlayerClientInputStateTest, EscapeClosesMapOrRequiresExplicitExitConfirmation)
{
    PlayerClientExitConfirmationState state;

    EXPECT_EQ(state.observe(true, true, true), PlayerClientExitInputAction::CloseMap);
    EXPECT_FALSE(state.pending());
    EXPECT_EQ(state.observe(false, false, false), PlayerClientExitInputAction::None);

    EXPECT_EQ(state.observe(false, true, false), PlayerClientExitInputAction::OpenConfirmation);
    EXPECT_TRUE(state.pending());
    EXPECT_EQ(state.observe(false, false, false), PlayerClientExitInputAction::None);
    EXPECT_EQ(state.observe(false, true, false), PlayerClientExitInputAction::CancelConfirmation);
    EXPECT_FALSE(state.pending());

    state.request();
    EXPECT_EQ(state.observe(false, false, true), PlayerClientExitInputAction::ConfirmExit);
    EXPECT_FALSE(state.pending());
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
