#include <client/FrameScheduler.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <compare>
#include <cstdint>
#include <string>

namespace {

TEST(FrameSchedulerTest, RendersCanOccurRepeatedlyBeforeTheNextSimulationBoundary)
{
    static constexpr auto PERIOD = std::chrono::milliseconds{ 100 };
    std::chrono::steady_clock::time_point const started_at{};
    client::FrameScheduler scheduler{ started_at, PERIOD };

    EXPECT_TRUE(scheduler.simulationDue(started_at));
    EXPECT_FALSE(scheduler.simulationDue(started_at + std::chrono::milliseconds{ 20 }));
    EXPECT_FALSE(scheduler.simulationDue(started_at + std::chrono::milliseconds{ 50 }));
    EXPECT_FALSE(scheduler.simulationDue(started_at + std::chrono::milliseconds{ 99 }));
    EXPECT_TRUE(scheduler.simulationDue(started_at + PERIOD));
}

TEST(FrameSchedulerTest, LateFramesDoNotCreateAnInputCatchUpBurst)
{
    static constexpr auto PERIOD = std::chrono::milliseconds{ 100 };
    std::chrono::steady_clock::time_point const started_at{};
    client::FrameScheduler scheduler{ started_at, PERIOD };

    EXPECT_TRUE(scheduler.simulationDue(started_at));
    EXPECT_TRUE(scheduler.simulationDue(started_at + std::chrono::milliseconds{ 350 }));
    EXPECT_EQ(scheduler.lastSimulationBoundary(), started_at + std::chrono::milliseconds{ 300 });
    EXPECT_FALSE(scheduler.simulationDue(started_at + std::chrono::milliseconds{ 350 }));
    EXPECT_EQ(scheduler.lastSimulationBoundary(), started_at + std::chrono::milliseconds{ 300 });
    EXPECT_FALSE(scheduler.simulationDue(started_at + std::chrono::milliseconds{ 399 }));
    EXPECT_TRUE(scheduler.simulationDue(started_at + std::chrono::milliseconds{ 400 }));
    EXPECT_EQ(scheduler.lastSimulationBoundary(), started_at + std::chrono::milliseconds{ 400 });
}

TEST(FrameSchedulerTest, RenderingDelayDoesNotAccumulateIntoMovementSlowdown)
{
    static constexpr auto PERIOD = std::chrono::milliseconds{ 100 };
    static constexpr auto RENDER_PERIOD = std::chrono::milliseconds{ 9 };
    static constexpr auto DURATION = std::chrono::seconds{ 30 };
    static constexpr uint32_t EXPECTED_INPUTS = 300U;
    std::chrono::steady_clock::time_point const started_at{};
    client::FrameScheduler scheduler{ started_at, PERIOD };
    uint32_t inputs = 0U;

    for (auto elapsed = std::chrono::milliseconds::zero(); elapsed < DURATION; elapsed += RENDER_PERIOD) {
        if (scheduler.simulationDue(started_at + elapsed)) {
            ++inputs;
        }
    }

    EXPECT_EQ(inputs, EXPECTED_INPUTS);
}

TEST(FrameSchedulerTest, SuccessfulPresentationSkipsIdleDelayWithoutChangingSimulationCadence)
{
    static constexpr auto PERIOD = std::chrono::milliseconds{ 100 };
    std::chrono::steady_clock::time_point const started_at{};
    client::FrameScheduler scheduler{ started_at, PERIOD };

    ASSERT_TRUE(scheduler.simulationDue(started_at));
    EXPECT_EQ(
        scheduler.idleDelay(
            started_at,
            started_at + std::chrono::milliseconds{ 10 },
            true
        ),
        std::chrono::steady_clock::duration::zero()
    );
    EXPECT_FALSE(scheduler.simulationDue(started_at + std::chrono::milliseconds{ 10 }));
    EXPECT_TRUE(scheduler.simulationDue(started_at + PERIOD));
}

TEST(FrameSchedulerTest, FailedPresentationRetainsOnlyTheRemainingBackoffForShortIterations)
{
    static constexpr auto PERIOD = std::chrono::milliseconds{ 100 };
    static constexpr auto ITERATION_DURATION = std::chrono::microseconds{ 400 };
    std::chrono::steady_clock::time_point const started_at{};
    client::FrameScheduler scheduler{ started_at, PERIOD };

    ASSERT_TRUE(scheduler.simulationDue(started_at));
    EXPECT_EQ(
        scheduler.idleDelay(started_at, started_at + ITERATION_DURATION, false),
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::microseconds{ 600 })
    );
}

TEST(FrameSchedulerTest, BlockedFailedPresentationDoesNotAddAnIdleDelay)
{
    static constexpr auto PERIOD = std::chrono::milliseconds{ 100 };
    static constexpr auto BLOCKED_ITERATION = std::chrono::microseconds{ 8248 };
    std::chrono::steady_clock::time_point const started_at{};
    client::FrameScheduler scheduler{ started_at, PERIOD };

    ASSERT_TRUE(scheduler.simulationDue(started_at));
    EXPECT_EQ(
        scheduler.idleDelay(started_at, started_at + BLOCKED_ITERATION, false),
        std::chrono::steady_clock::duration::zero()
    );
}

TEST(FrameSchedulerTest, FailedPresentationReachesZeroDelayAtTheOneMillisecondBoundary)
{
    static constexpr auto PERIOD = std::chrono::milliseconds{ 100 };
    static constexpr auto JUST_BELOW_BACKOFF = std::chrono::microseconds{ 999 };
    static constexpr auto BACKOFF = std::chrono::milliseconds{ 1 };
    std::chrono::steady_clock::time_point const started_at{};
    client::FrameScheduler scheduler{ started_at, PERIOD };

    ASSERT_TRUE(scheduler.simulationDue(started_at));
    EXPECT_EQ(
        scheduler.idleDelay(started_at, started_at + JUST_BELOW_BACKOFF, false),
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::microseconds{ 1 })
    );
    EXPECT_EQ(
        scheduler.idleDelay(started_at, started_at + BACKOFF, false),
        std::chrono::steady_clock::duration::zero()
    );
}

TEST(FrameSchedulerTest, FailedPresentationDoesNotIdlePastTheNextSimulationBoundary)
{
    static constexpr auto PERIOD = std::chrono::milliseconds{ 10 };
    static constexpr auto ITERATION_STARTED = std::chrono::microseconds{ 9600 };
    static constexpr auto ITERATION_COMPLETED = std::chrono::microseconds{ 9800 };
    std::chrono::steady_clock::time_point const started_at{};
    client::FrameScheduler scheduler{ started_at, PERIOD };

    ASSERT_TRUE(scheduler.simulationDue(started_at));
    EXPECT_EQ(
        scheduler.idleDelay(
            started_at + ITERATION_STARTED,
            started_at + ITERATION_COMPLETED,
            false
        ),
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::microseconds{ 200 })
    );
}

TEST(FrameSchedulerTest, ShortFailedIterationDoesNotDelayAnAlreadyDueSimulation)
{
    static constexpr auto PERIOD = std::chrono::milliseconds{ 10 };
    static constexpr auto ITERATION_STARTED = std::chrono::microseconds{ 9800 };
    static constexpr auto ITERATION_COMPLETED = std::chrono::microseconds{ 10'200 };
    std::chrono::steady_clock::time_point const started_at{};
    client::FrameScheduler scheduler{ started_at, PERIOD };

    ASSERT_TRUE(scheduler.simulationDue(started_at));
    EXPECT_EQ(
        scheduler.idleDelay(
            started_at + ITERATION_STARTED,
            started_at + ITERATION_COMPLETED,
            false
        ),
        std::chrono::steady_clock::duration::zero()
    );
}

} // namespace
