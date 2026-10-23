#pragma once

#include <core/graphics/vulkan/Vulkan.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace client {

struct RendererFrameIdentity final {
    uint64_t sequence = 0U;
    uint64_t swapchain_generation = 0U;
};

struct RendererFrameProgress final {
    std::optional<RendererFrameIdentity> submitted;
    std::optional<RendererFrameIdentity> completed;
};

struct RendererRetiredFrameCompletion final {
    RendererFrameIdentity submission;
    uint32_t frame_slot = 0U;
    std::chrono::steady_clock::time_point observed_at;
};

namespace detail {

class PresentationFrameProgressTracker final {
public:
    [[nodiscard]] RendererFrameIdentity recordSubmission(
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
        uint32_t frame_slot,
        uint64_t swapchain_generation
    )
    {
        auto const slot = m_submissions_by_slot.find(frame_slot);
        if (slot == m_submissions_by_slot.end()) {
            throw std::logic_error("renderer frame slot was not prepared for completion tracking");
        }
        if (m_next_sequence == 0U) {
            throw std::overflow_error("renderer frame submission sequence exhausted");
        }
        RendererFrameIdentity const identity{
            .sequence = m_next_sequence,
            .swapchain_generation = swapchain_generation,
        };
        if (m_next_sequence == std::numeric_limits<uint64_t>::max()) {
            m_next_sequence = 0U;
        } else {
            ++m_next_sequence;
        }
        m_progress.submitted = identity;
        slot->second = identity;
        return identity;
    }

    void prepareSlot(uint32_t const frame_slot)
    {
        m_submissions_by_slot.try_emplace(frame_slot);
    }

    // Only call after the presentation backend has completed every submitted fence.
    void observeRecreationCompleted(std::chrono::steady_clock::time_point const observed_at)
    {
        size_t const first_retired = m_retired_completions.size();
        for (auto& [frame_slot, submission] : m_submissions_by_slot) {
            if (!submission) {
                continue;
            }
            m_retired_completions.push_back({ *submission, frame_slot, observed_at });
            observeCompletedSlot(frame_slot);
        }
        using CompletionOffset = std::vector<RendererRetiredFrameCompletion>::difference_type;
        std::sort(
            m_retired_completions.begin() + static_cast<CompletionOffset>(first_retired),
            m_retired_completions.end(),
            [](auto const& left, auto const& right) {
                return left.submission.sequence < right.submission.sequence;
            }
        );
    }

    [[nodiscard]] std::vector<RendererRetiredFrameCompletion> const& retiredCompletions() const noexcept
    {
        return m_retired_completions;
    }

    void resetProgress() noexcept
    {
        m_progress = {};
        m_submissions_by_slot.clear();
        m_retired_completions.clear();
    }

    void observeCompletedSlot(uint32_t const frame_slot) noexcept
    {
        auto const submission = m_submissions_by_slot.find(frame_slot);
        if (submission == m_submissions_by_slot.end()) {
            return;
        }
        if (!submission->second.has_value()) {
            return;
        }
        RendererFrameIdentity const completed_submission = *submission->second;
        submission->second.reset();
        if (!m_progress.completed.has_value()) {
            m_progress.completed = completed_submission;
            return;
        }
        if (completed_submission.sequence > m_progress.completed->sequence) {
            m_progress.completed = completed_submission;
        }
    }

    void observeAcquire(core::graphics::vulkan::PresentationAcquireDiagnostics const& diagnostics)
    {
        if (!diagnostics.captured) {
            return;
        }
        uint32_t const stage_count = std::min(
            diagnostics.stage_count,
            static_cast<uint32_t>(diagnostics.stages.size())
        );
        for (uint32_t index = 0U; index < stage_count; ++index) {
            core::graphics::vulkan::PresentationAcquireStageTiming const& stage = diagnostics.stages[index];
            if (stage.stage != core::graphics::vulkan::PresentationAcquireStage::SubmittedFrameFence
                || stage.result != VK_SUCCESS) {
                continue;
            }
            observeCompletedSlot(diagnostics.frame_slot);
            return;
        }
    }

    [[nodiscard]] RendererFrameProgress progress() const noexcept
    {
        return m_progress;
    }

private:
    RendererFrameProgress m_progress;
    std::vector<RendererRetiredFrameCompletion> m_retired_completions;
    std::unordered_map<uint32_t, std::optional<RendererFrameIdentity>> m_submissions_by_slot;
    uint64_t m_next_sequence = 1U;
};

} // namespace detail
} // namespace client
