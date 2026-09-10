#include <acceptance/RendererBenchmark.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

namespace {

TEST(RendererBenchmark, SummarizesOrderedCpuPresentationRequestDurations)
{
    std::vector<std::chrono::nanoseconds> const samples{
        std::chrono::nanoseconds{ 4 },
        std::chrono::nanoseconds{ 1 },
        std::chrono::nanoseconds{ 3 },
        std::chrono::nanoseconds{ 2 },
    };

    acceptance::FrameTimingSummary const summary = acceptance::summarizeFrameTimings(samples);

    EXPECT_EQ(summary.sample_count, 4U);
    EXPECT_EQ(summary.p50, std::chrono::nanoseconds{ 2 });
    EXPECT_EQ(summary.p95, std::chrono::nanoseconds{ 4 });
    EXPECT_EQ(summary.p99, std::chrono::nanoseconds{ 4 });
    EXPECT_EQ(summary.maximum, std::chrono::nanoseconds{ 4 });
    EXPECT_EQ(summary.mean, std::chrono::nanoseconds{ 2 });
}

TEST(RendererBenchmark, ReportsNoStatisticsForNoFrames)
{
    acceptance::FrameTimingSummary const summary = acceptance::summarizeFrameTimings({ });

    EXPECT_EQ(summary.sample_count, 0U);
    EXPECT_EQ(summary.p50, std::chrono::nanoseconds::zero());
    EXPECT_EQ(summary.p95, std::chrono::nanoseconds::zero());
    EXPECT_EQ(summary.p99, std::chrono::nanoseconds::zero());
    EXPECT_EQ(summary.maximum, std::chrono::nanoseconds::zero());
    EXPECT_EQ(summary.mean, std::chrono::nanoseconds::zero());
}

} // namespace
