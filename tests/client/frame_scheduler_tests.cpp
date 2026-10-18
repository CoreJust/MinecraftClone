#include <client/FrameScheduler.hpp>

#include <gtest/gtest.h>

#include <chrono>
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
    EXPECT_FALSE(scheduler.simulationDue(started_at + std::chrono::milliseconds{ 350 }));
    EXPECT_FALSE(scheduler.simulationDue(started_at + std::chrono::milliseconds{ 449 }));
    EXPECT_TRUE(scheduler.simulationDue(started_at + std::chrono::milliseconds{ 450 }));
}

TEST(FrameSchedulerTest, SuccessfulPresentationSkipsIdleDelayWithoutChangingSimulationCadence)
{
    static constexpr auto PERIOD = std::chrono::milliseconds{ 100 };
    std::chrono::steady_clock::time_point const started_at{};
    client::FrameScheduler scheduler{ started_at, PERIOD };

    ASSERT_TRUE(scheduler.simulationDue(started_at));
    EXPECT_EQ(
        scheduler.idleDelay(started_at + std::chrono::milliseconds{ 10 }, true),
        std::chrono::milliseconds::zero()
    );
    EXPECT_FALSE(scheduler.simulationDue(started_at + std::chrono::milliseconds{ 10 }));
    EXPECT_TRUE(scheduler.simulationDue(started_at + PERIOD));
}

TEST(FrameSchedulerTest, IdleDelayIsBoundedToAvoidBusySpinWithoutSuccessfulPresentation)
{
    static constexpr auto PERIOD = std::chrono::milliseconds{ 100 };
    std::chrono::steady_clock::time_point const started_at{};
    client::FrameScheduler scheduler{ started_at, PERIOD };

    ASSERT_TRUE(scheduler.simulationDue(started_at));
    EXPECT_EQ(
        scheduler.idleDelay(started_at + std::chrono::milliseconds{ 10 }, false),
        std::chrono::milliseconds{ 1 }
    );
    EXPECT_EQ(scheduler.idleDelay(started_at + PERIOD, false), std::chrono::milliseconds{ 1 });
}

} // namespace
