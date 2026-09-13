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

TEST(RendererBenchmark, MapsContextAndRendererOptionsFromOneBenchmarkConfiguration)
{
    acceptance::RendererBenchmarkOptions const benchmark_options{
        .require_immediate_present_mode = true,
        .require_validation = true,
    };

    client::VulkanRendererOptions const renderer_options =
        acceptance::makeRendererBenchmarkVulkanOptions(benchmark_options);

    EXPECT_TRUE(renderer_options.require_immediate_present_mode);
    EXPECT_TRUE(renderer_options.require_validation);
    EXPECT_FALSE(renderer_options.enable_frame_capture);
}

TEST(RendererBenchmark, DefaultsToValidationDisabledAndHudDisabled)
{
    acceptance::RendererBenchmarkOptions const benchmark_options{ };
    client::VulkanRendererOptions const renderer_options =
        acceptance::makeRendererBenchmarkVulkanOptions(benchmark_options);

    EXPECT_FALSE(renderer_options.require_validation);
    EXPECT_FALSE(benchmark_options.debug_hud_enabled);
}

TEST(RendererBenchmark, RejectsFifoWhenImmediatePresentationWasRequested)
{
    acceptance::RendererBenchmarkOptions const immediate_options{
        .require_immediate_present_mode = true,
    };
    acceptance::RendererBenchmarkOptions const default_options{ };

    EXPECT_TRUE(acceptance::rendererBenchmarkPresentModeSatisfied(
        immediate_options,
        client::RendererPresentMode::Immediate
    ));
    EXPECT_FALSE(acceptance::rendererBenchmarkPresentModeSatisfied(
        immediate_options,
        client::RendererPresentMode::FIFO
    ));
    EXPECT_TRUE(acceptance::rendererBenchmarkPresentModeSatisfied(
        default_options,
        client::RendererPresentMode::FIFO
    ));
}

} // namespace
