#pragma once

#include <core/graphics/vulkan/Vulkan.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace client {

struct StoneFaceGrowthDiagnostics final {
    uint64_t pre_growth_deadline_rejection_count = 0U;
    uint64_t growth_attempt_count = 0U;
    uint64_t before_recreate_count = 0U;
    uint64_t deferred_recreation_count = 0U;
    uint64_t recreation_exception_count = 0U;
    uint64_t recreation_unknown_exception_count = 0U;
    uint64_t recreation_exception_before_hook_count = 0U;
    uint64_t growth_success_count = 0U;
    uint64_t rollback_count = 0U;
    uint64_t rollback_exception_count = 0U;
    uint64_t post_growth_deadline_rejection_count = 0U;
    uint64_t publication_retry_count = 0U;
    uint32_t last_required_capacity = 0U;
    uint32_t last_target_capacity = 0U;
    uint32_t required_capacity_high_water = 0U;
    uint32_t actual_capacity = 0U;
    uint32_t actual_capacity_high_water = 0U;
    std::chrono::nanoseconds growth_work_duration{ 0 };
    static constexpr uint32_t MAXIMUM_EXCEPTION_REASON_BYTES = 256U;
    std::array<char, MAXIMUM_EXCEPTION_REASON_BYTES + 1U> first_recreation_exception_reason{};
    std::array<char, MAXIMUM_EXCEPTION_REASON_BYTES + 1U> last_recreation_exception_reason{};

    void recordRecreationExceptionReason(std::string_view const reason) noexcept
    {
        std::string_view bounded_reason = reason;
        if (bounded_reason.size() > MAXIMUM_EXCEPTION_REASON_BYTES) {
            bounded_reason.remove_suffix(bounded_reason.size() - MAXIMUM_EXCEPTION_REASON_BYTES);
        }
        if (recreation_exception_count == recreation_unknown_exception_count) {
            std::copy(bounded_reason.begin(), bounded_reason.end(), first_recreation_exception_reason.data());
            first_recreation_exception_reason[bounded_reason.size()] = '\0';
        }
        if (std::string_view{ last_recreation_exception_reason.data() } != bounded_reason) {
            std::copy(bounded_reason.begin(), bounded_reason.end(), last_recreation_exception_reason.data());
            last_recreation_exception_reason[bounded_reason.size()] = '\0';
        }
    }
};

namespace detail {

class StoneFaceRecreationDeferred final : public std::runtime_error {
public:
    StoneFaceRecreationDeferred()
        : std::runtime_error("stone face arena recreation was deferred")
    {}
};

enum class StoneFaceRecreationFailure : uint8_t {
    Deferred,
    DrainPending,
    Failed,
};

class PendingStoneFaceGrowth final {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::seconds MAXIMUM_PENDING_AGE{ 5 };
    static constexpr std::string_view DRAIN_TIMEOUT_REASON = "Vulkan presentation recreation fence wait timed out";
    static constexpr std::string_view PRESENT_DRAIN_TIMEOUT_REASON = "Vulkan presentation present fence wait timed out";

    void clear() noexcept
    {
        m_target_capacity = 0U;
        m_started_at = {};
        m_drain_timeout_reason = {};
    }

    [[nodiscard]] uint32_t targetCapacity() const noexcept { return m_target_capacity; }

    void coalesce(
        uint32_t const required_capacity,
        Clock::time_point const observed_at = Clock::now()
    ) noexcept
    {
        if (m_target_capacity == 0U && required_capacity != 0U) {
            m_started_at = observed_at;
            m_drain_timeout_reason = "stone face arena growth exceeded the upload deadline";
        }
        m_target_capacity = std::max(m_target_capacity, required_capacity);
    }

    [[nodiscard]] StoneFaceRecreationFailure recordFailure(
        std::exception_ptr const& failure,
        uint32_t const target_capacity,
        bool const resources_destroyed,
        Clock::time_point const observed_at
    )
    {
        try {
            std::rethrow_exception(failure);
        } catch (StoneFaceRecreationDeferred const&) {
            if (!resources_destroyed) {
                coalesce(target_capacity, observed_at);
                return StoneFaceRecreationFailure::Deferred;
            }
            clear();
            return StoneFaceRecreationFailure::Failed;
        } catch (core::graphics::vulkan::VulkanError const& error) {
            std::string_view retryable_timeout_reason;
            if (!resources_destroyed) {
                std::string_view const reason{ error.what() };
                if (reason == DRAIN_TIMEOUT_REASON) {
                    retryable_timeout_reason = DRAIN_TIMEOUT_REASON;
                } else if (reason == PRESENT_DRAIN_TIMEOUT_REASON) {
                    retryable_timeout_reason = PRESENT_DRAIN_TIMEOUT_REASON;
                }
            }
            if (!retryable_timeout_reason.empty()) {
                if (m_target_capacity == 0U) {
                    m_started_at = observed_at;
                }
                m_target_capacity = std::max(m_target_capacity, target_capacity);
                m_drain_timeout_reason = retryable_timeout_reason;
                return StoneFaceRecreationFailure::DrainPending;
            }
        } catch (...) {
            clear();
            return StoneFaceRecreationFailure::Failed;
        }
        clear();
        return StoneFaceRecreationFailure::Failed;
    }

    template <typename Grow, typename Render>
        requires std::invocable<Grow&, uint32_t, Clock::time_point> && std::invocable<Render&>
    [[nodiscard]] bool render(
        uint32_t const actual_capacity,
        Clock::time_point const deadline,
        Grow&& grow,
        Clock::time_point const observed_at,
        Render&& render_ready
    )
    {
        if (actual_capacity >= m_target_capacity) {
            clear();
        }
        if (m_target_capacity != 0U) {
            if (observed_at - m_started_at >= MAXIMUM_PENDING_AGE) {
                throw std::runtime_error(
                    "stone face arena growth exceeded its pending drain horizon: actual="
                    + std::to_string(actual_capacity) + ", target=" + std::to_string(m_target_capacity)
                    + ", reason=" + std::string{ m_drain_timeout_reason }
                );
            }
            uint32_t const installed_capacity = std::invoke(grow, m_target_capacity, deadline);
            if (installed_capacity >= m_target_capacity) {
                clear();
            }
            if (m_target_capacity != 0U) {
                return false;
            }
        }
        return std::invoke(render_ready);
    }

private:
    uint32_t m_target_capacity = 0U;
    Clock::time_point m_started_at{};
    std::string_view m_drain_timeout_reason;
};

template <typename Recreate>
    requires std::invocable<Recreate&, std::chrono::steady_clock::time_point>
[[nodiscard]] bool tryRecreatePresentationWithinDeadline(
    std::chrono::steady_clock::time_point const deadline,
    Recreate&& recreate
)
{
    try {
        std::invoke(recreate, deadline);
    } catch (core::graphics::vulkan::VulkanError const& error) {
        std::string_view const reason{ error.what() };
        if (reason == PendingStoneFaceGrowth::DRAIN_TIMEOUT_REASON
            || reason == PendingStoneFaceGrowth::PRESENT_DRAIN_TIMEOUT_REASON) {
            return false;
        }
        throw;
    }
    return true;
}

template <typename Recreate>
[[nodiscard]] bool tryGrowStoneFaceCapacity(
    uint32_t& capacity,
    uint32_t const required_capacity,
    uint32_t const maximum_capacity,
    std::chrono::steady_clock::time_point const deadline,
    Recreate&& recreate,
    StoneFaceGrowthDiagnostics* const diagnostics = nullptr
)
{
    if (capacity == 0U || maximum_capacity == 0U || capacity > maximum_capacity
        || required_capacity > maximum_capacity) {
        throw std::invalid_argument("invalid stone face capacity request");
    }
    if (diagnostics != nullptr) {
        diagnostics->last_required_capacity = required_capacity;
        if (required_capacity > diagnostics->required_capacity_high_water) {
            diagnostics->required_capacity_high_water = required_capacity;
        }
    }
    if (required_capacity <= capacity) {
        return true;
    }

    uint32_t const previous_capacity = capacity;
    uint32_t next_capacity = capacity;
    while (next_capacity < required_capacity) {
        if (next_capacity > maximum_capacity / 2U) {
            next_capacity = maximum_capacity;
            break;
        }
        next_capacity *= 2U;
    }
    capacity = next_capacity;
    struct GrowthWorkTimer final {
        StoneFaceGrowthDiagnostics* diagnostics;
        std::chrono::steady_clock::time_point started_at;

        ~GrowthWorkTimer()
        {
            if (diagnostics != nullptr) {
                diagnostics->growth_work_duration += std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - started_at
                );
            }
        }
    } const timer{
        .diagnostics = diagnostics,
        .started_at = diagnostics != nullptr ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{},
    };
    uint64_t const before_recreate_count = diagnostics != nullptr ? diagnostics->before_recreate_count : 0U;
    if (diagnostics != nullptr) {
        ++diagnostics->growth_attempt_count;
        diagnostics->last_target_capacity = next_capacity;
    }
    try {
        std::invoke(recreate, deadline);
    } catch (...) {
        if (diagnostics != nullptr) {
            try {
                throw;
            } catch (StoneFaceRecreationDeferred const&) {
                ++diagnostics->deferred_recreation_count;
            } catch (std::exception const& error) {
                diagnostics->recordRecreationExceptionReason(error.what());
                ++diagnostics->recreation_exception_count;
                if (diagnostics->before_recreate_count == before_recreate_count) {
                    ++diagnostics->recreation_exception_before_hook_count;
                }
            } catch (...) {
                ++diagnostics->recreation_exception_count;
                ++diagnostics->recreation_unknown_exception_count;
                if (diagnostics->before_recreate_count == before_recreate_count) {
                    ++diagnostics->recreation_exception_before_hook_count;
                }
            }
            ++diagnostics->rollback_count;
        }
        capacity = previous_capacity;
        try {
            std::invoke(recreate, deadline);
        } catch (...) {
            if (diagnostics != nullptr) {
                ++diagnostics->rollback_exception_count;
            }
            throw;
        }
        return false;
    }
    if (diagnostics != nullptr) {
        ++diagnostics->growth_success_count;
    }
    return true;
}

} // namespace detail

} // namespace client
