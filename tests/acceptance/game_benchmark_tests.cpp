#include <acceptance/GameBenchmark.hpp>

#include <gtest/gtest.h>

#include <chrono>

using namespace std::chrono_literals;

TEST(GameBenchmark, ClassifiesCompletedRequestsWithinHalfOpenIntervals)
{
    acceptance::GameBenchmarkOptions const options{};
    auto const first = std::chrono::steady_clock::time_point{};
    using acceptance::GameBenchmarkPhase;
    using acceptance::gameBenchmarkPhaseAt;

    EXPECT_EQ(gameBenchmarkPhaseAt(first - 1ns, first, options), GameBenchmarkPhase::Outside);
    EXPECT_EQ(gameBenchmarkPhaseAt(first, first, options), GameBenchmarkPhase::Cold);
    EXPECT_EQ(gameBenchmarkPhaseAt(first + 2s - 1ns, first, options), GameBenchmarkPhase::Cold);
    EXPECT_EQ(gameBenchmarkPhaseAt(first + 2s, first, options), GameBenchmarkPhase::Warm);
    EXPECT_EQ(gameBenchmarkPhaseAt(first + 5s - 1ns, first, options), GameBenchmarkPhase::Warm);
    EXPECT_EQ(gameBenchmarkPhaseAt(first + 5s, first, options), GameBenchmarkPhase::Uncapped);
    EXPECT_EQ(gameBenchmarkPhaseAt(first + 6s - 1ns, first, options), GameBenchmarkPhase::Uncapped);
    EXPECT_EQ(gameBenchmarkPhaseAt(first + 6s, first, options), GameBenchmarkPhase::Outside);
    EXPECT_EQ(gameBenchmarkPhaseAt(first + 6s + 1ns, first, options), GameBenchmarkPhase::Outside);
}
