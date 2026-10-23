#include <client/render/PresentationFrameProgress.hpp>
#include <core/graphics/vulkan/Vulkan.hpp>

#include <gtest/gtest.h>
#include <vulkan/vulkan_core.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <ratio>
#include <string>
#include <vector>

TEST(PresentationFrameProgressTest, CompletesOnlyTheMatchingSubmissionAfterItsFenceSignals)
{
    static constexpr uint32_t FRAME_SLOT = 7U;
    static constexpr uint64_t SWAPCHAIN_GENERATION = 3U;
    client::detail::PresentationFrameProgressTracker tracker;
    tracker.prepareSlot(FRAME_SLOT);
    client::RendererFrameIdentity const submitted = tracker.recordSubmission(
        FRAME_SLOT,
        SWAPCHAIN_GENERATION
    );

    core::graphics::vulkan::PresentationAcquireDiagnostics diagnostics{
        .captured = true,
        .frame_slot = FRAME_SLOT,
        .stage_count = 1U,
        .image_index = {},
    };
    diagnostics.stages[0] = {
        .stage = core::graphics::vulkan::PresentationAcquireStage::SubmittedFrameFence,
        .result = VK_TIMEOUT,
    };
    tracker.observeAcquire(diagnostics);
    EXPECT_FALSE(tracker.progress().completed.has_value());

    diagnostics.frame_slot = FRAME_SLOT + 1U;
    diagnostics.stages[0].result = VK_SUCCESS;
    tracker.observeAcquire(diagnostics);
    EXPECT_FALSE(tracker.progress().completed.has_value());

    diagnostics.frame_slot = FRAME_SLOT;
    diagnostics.stages[0].stage = core::graphics::vulkan::PresentationAcquireStage::AcquireFence;
    tracker.observeAcquire(diagnostics);
    EXPECT_FALSE(tracker.progress().completed.has_value());

    diagnostics.stages[0].stage = core::graphics::vulkan::PresentationAcquireStage::SubmittedFrameFence;
    tracker.observeAcquire(diagnostics);
    client::RendererFrameProgress const progress = tracker.progress();
    ASSERT_TRUE(progress.submitted.has_value());
    ASSERT_TRUE(progress.completed.has_value());
    EXPECT_EQ(progress.submitted->sequence, submitted.sequence);
    EXPECT_EQ(progress.completed->sequence, submitted.sequence);
    EXPECT_EQ(progress.completed->swapchain_generation, SWAPCHAIN_GENERATION);

    tracker.resetProgress();
    EXPECT_FALSE(tracker.progress().submitted.has_value());
    EXPECT_FALSE(tracker.progress().completed.has_value());
}

TEST(PresentationFrameProgressTest, PreservesEveryRealRetirementFenceReceiptBeforeSlotReuse)
{
    client::detail::PresentationFrameProgressTracker tracker;
    for (uint32_t slot = 0U; slot < 3U; ++slot) {
        tracker.prepareSlot(slot);
        static_cast<void>(tracker.recordSubmission(slot, 2U));
    }
    auto const observed_at = std::chrono::steady_clock::time_point{} + std::chrono::seconds{ 4 };
    tracker.observeRecreationCompleted(observed_at);
    ASSERT_EQ(tracker.retiredCompletions().size(), 3U);
    for (auto const& completion : tracker.retiredCompletions()) {
        EXPECT_EQ(completion.submission.swapchain_generation, 2U);
        EXPECT_EQ(completion.observed_at, observed_at);
    }
    client::RendererFrameProgress const progress = tracker.progress();
    ASSERT_TRUE(progress.completed.has_value());
    EXPECT_EQ(progress.completed.value().sequence, 3U);
    tracker.observeRecreationCompleted(observed_at);
    EXPECT_EQ(tracker.retiredCompletions().size(), 3U);
    static_cast<void>(tracker.recordSubmission(0U, 3U));
    tracker.observeRecreationCompleted(observed_at + std::chrono::seconds{ 1 });
    ASSERT_EQ(tracker.retiredCompletions().size(), 4U);
    EXPECT_EQ(tracker.retiredCompletions().back().submission.swapchain_generation, 3U);
    EXPECT_EQ(tracker.retiredCompletions().back().submission.sequence, 4U);
    EXPECT_EQ(tracker.retiredCompletions().back().frame_slot, 0U);
    tracker.resetProgress();
    EXPECT_TRUE(tracker.retiredCompletions().empty());
}
