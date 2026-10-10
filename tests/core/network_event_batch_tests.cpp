#include <server/detail/NetworkEventBatch.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <thread>

TEST(ServerNetworkEventBatchTest, CapsReadyEventsPerSchedulingTurn)
{
    using namespace std::chrono_literals;
    uint32_t poll_calls = 0U;
    uint64_t const events = server::detail::pollNetworkEventBatch(
        [&poll_calls](std::chrono::milliseconds) {
            ++poll_calls;
            return 1U;
        },
        0ms,
        [] { return false; },
        10s
    );

    EXPECT_EQ(events, server::detail::MAX_NETWORK_EVENTS_PER_BATCH);
    EXPECT_EQ(poll_calls, server::detail::MAX_NETWORK_EVENTS_PER_BATCH);
}

TEST(ServerNetworkEventBatchTest, ChecksCancellationBetweenReadyEvents)
{
    using namespace std::chrono_literals;
    bool stop_requested = false;
    uint32_t poll_calls = 0U;
    uint64_t const events = server::detail::pollNetworkEventBatch(
        [&poll_calls, &stop_requested](std::chrono::milliseconds) {
            ++poll_calls;
            stop_requested = poll_calls == 4U;
            return 1U;
        },
        0ms,
        [&stop_requested] { return stop_requested; },
        10s
    );

    EXPECT_EQ(events, 4U);
    EXPECT_EQ(poll_calls, 4U);
}

TEST(ServerNetworkEventBatchTest, DoesNotPollWhenAlreadyCancelled)
{
    using namespace std::chrono_literals;
    uint32_t poll_calls = 0U;
    uint64_t const events = server::detail::pollNetworkEventBatch(
        [&poll_calls](std::chrono::milliseconds) {
            ++poll_calls;
            return 1U;
        },
        0ms,
        [] { return true; },
        10s
    );

    EXPECT_EQ(events, 0U);
    EXPECT_EQ(poll_calls, 0U);
}

TEST(ServerNetworkEventBatchTest, BoundsDrainTimeWhenEventsArriveContinuously)
{
    using namespace std::chrono_literals;
    uint32_t poll_calls = 0U;
    uint64_t const events = server::detail::pollNetworkEventBatch(
        [&poll_calls](std::chrono::milliseconds) {
            ++poll_calls;
            std::this_thread::sleep_for(2ms);
            return 1U;
        },
        0ms,
        [] { return false; },
        1ms
    );

    EXPECT_EQ(events, poll_calls);
    EXPECT_GE(poll_calls, 1U);
    EXPECT_LE(poll_calls, 2U);
}
