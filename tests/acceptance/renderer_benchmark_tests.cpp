#include <shared/net/Message.hpp>

#include <acceptance/RendererBenchmark.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
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

TEST(RendererBenchmark, RequiresBoth120FpsAndStableFrameTiming)
{
    acceptance::FrameTimingSummary timings{ .p99 = std::chrono::nanoseconds{ 8'000'000 } };
    EXPECT_TRUE(acceptance::rendererBenchmarkMeetsFrameTarget(120.0, timings));
    EXPECT_TRUE(acceptance::rendererBenchmarkMeetsFrameTarget(118.8, timings));
    EXPECT_FALSE(acceptance::rendererBenchmarkMeetsFrameTarget(118.79, timings));
    timings.p99 = std::chrono::nanoseconds{ 10'000'001 };
    EXPECT_FALSE(acceptance::rendererBenchmarkMeetsFrameTarget(150.0, timings));
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

TEST(RendererBenchmark, StreamsOneFullColumnWhenMovingEast)
{
    shared::HeightTileCoordinate const center{ .x = 125, .y = -17 };

    acceptance::RendererBenchmarkStreamUpdate const update =
        acceptance::makeRendererBenchmarkStreamUpdate(center, 0U);

    EXPECT_EQ(update.direction, acceptance::RendererBenchmarkStreamDirection::PositiveX);
    EXPECT_EQ(update.center, (shared::HeightTileCoordinate{ .x = 126, .y = -17 }));
    ASSERT_EQ(update.removals.size(), shared::HEIGHT_TILE_INTEREST_WIDTH);
    ASSERT_EQ(update.additions.size(), shared::HEIGHT_TILE_INTEREST_WIDTH);
    for (uint32_t row = 0U; row < shared::HEIGHT_TILE_INTEREST_WIDTH; ++row) {
        EXPECT_EQ(
            update.removals[row],
            (shared::HeightTileCoordinate{ .x = 80, .y = -62 + static_cast<int32_t>(row) })
        );
        EXPECT_EQ(
            update.additions[row],
            (shared::HeightTileCoordinate{ .x = 170, .y = -62 + static_cast<int32_t>(row) })
        );
        EXPECT_EQ(std::ranges::count(update.removals, update.removals[row]), 1);
        EXPECT_EQ(std::ranges::count(update.additions, update.additions[row]), 1);
    }
}

TEST(RendererBenchmark, RotatesStreamingAcrossBothAxesAndReturnsToStart)
{
    shared::HeightTileCoordinate center{ .x = 0, .y = 0 };
    std::array<acceptance::RendererBenchmarkStreamDirection, 4U> const expected_directions{
        acceptance::RendererBenchmarkStreamDirection::PositiveX,
        acceptance::RendererBenchmarkStreamDirection::PositiveY,
        acceptance::RendererBenchmarkStreamDirection::NegativeX,
        acceptance::RendererBenchmarkStreamDirection::NegativeY,
    };

    for (uint64_t update_index = 0U;
         update_index < 4U * shared::HEIGHT_TILE_INTEREST_WIDTH;
         ++update_index) {
        acceptance::RendererBenchmarkStreamUpdate const update =
            acceptance::makeRendererBenchmarkStreamUpdate(center, update_index);
        EXPECT_EQ(
            update.direction,
            expected_directions[update_index / shared::HEIGHT_TILE_INTEREST_WIDTH]
        );
        ASSERT_EQ(update.removals.size(), shared::HEIGHT_TILE_INTEREST_WIDTH);
        ASSERT_EQ(update.additions.size(), shared::HEIGHT_TILE_INTEREST_WIDTH);
        for (shared::HeightTileCoordinate const removal : update.removals) {
            EXPECT_EQ(std::ranges::count(update.removals, removal), 1);
            EXPECT_EQ(std::ranges::count(update.additions, removal), 0);
        }
        center = update.center;
    }

    EXPECT_EQ(center, (shared::HeightTileCoordinate{ .x = 0, .y = 0 }));
}

} // namespace
