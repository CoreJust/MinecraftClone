#pragma once

#include <core/executor/Executor.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

namespace client {

template <typename Instance>
    requires std::is_nothrow_destructible_v<Instance>
class HeightTileInstanceReaper final {
public:
    // Absorb an R256 x200 frontier burst; the byte cap still limits retained storage.
    static constexpr uint32_t MAXIMUM_JOBS = 4'096U;
    static constexpr uint64_t MAXIMUM_BYTES = uint64_t{ 64U } * 1024U * 1024U;

    explicit HeightTileInstanceReaper(uint64_t const maximum_bytes = MAXIMUM_BYTES)
        : m_maximum_bytes(maximum_bytes)
        , m_executor({ .queue_capacity = MAXIMUM_JOBS, .result_capacity = MAXIMUM_JOBS }, 1U)
    {}

    ~HeightTileInstanceReaper()
    {
        drain();
        m_executor.shutdown();
    }

    HeightTileInstanceReaper(HeightTileInstanceReaper const&) = delete;
    HeightTileInstanceReaper& operator=(HeightTileInstanceReaper const&) = delete;

    [[nodiscard]]
    bool retire(std::vector<Instance>& instances, uint64_t const owned_storage_bytes = 0U) noexcept
    {
        if (instances.capacity() == 0U) {
            return true;
        }
        uint64_t const container_bytes = static_cast<uint64_t>(instances.capacity()) * sizeof(Instance);
        if (container_bytes > m_maximum_bytes || owned_storage_bytes > m_maximum_bytes - container_bytes) {
            recordFallback();
            return false;
        }
        uint64_t const bytes = container_bytes + owned_storage_bytes;
        collect();
        uint64_t const outstanding = m_outstanding_bytes.load(std::memory_order_acquire);
        if (bytes > m_maximum_bytes || outstanding > m_maximum_bytes - bytes
            || m_outstanding_jobs.load(std::memory_order_acquire) >= MAXIMUM_JOBS) {
            recordFallback();
            return false;
        }
        std::shared_ptr<std::vector<Instance>> retired;
        try {
            retired = std::make_shared<std::vector<Instance>>();
            retired->swap(instances);
            m_outstanding_bytes.fetch_add(bytes, std::memory_order_acq_rel);
            m_outstanding_jobs.fetch_add(1U, std::memory_order_acq_rel);
            core::executor::Submission const submission = m_executor.trySubmit(
                [this, retired, bytes](core::executor::CancellationToken const&) {
                    std::vector<Instance>{}.swap(*retired);
                    m_outstanding_bytes.fetch_sub(bytes, std::memory_order_acq_rel);
                    m_outstanding_jobs.fetch_sub(1U, std::memory_order_acq_rel);
                }
            );
            if (submission.status == core::executor::SubmissionStatus::Accepted) {
                m_high_water_bytes = std::max(m_high_water_bytes, outstanding + bytes);
                return true;
            }
        } catch (...) {
            if (!retired) {
                recordFallback();
                return false;
            }
        }
        retired->swap(instances);
        m_outstanding_bytes.fetch_sub(bytes, std::memory_order_acq_rel);
        m_outstanding_jobs.fetch_sub(1U, std::memory_order_acq_rel);
        recordFallback();
        return false;
    }

    void drain() noexcept
    {
        while (m_outstanding_jobs.load(std::memory_order_acquire) != 0U) {
            collect();
            std::this_thread::yield();
        }
        collect();
    }

    [[nodiscard]] uint64_t outstandingBytes() const noexcept
    {
        return m_outstanding_bytes.load(std::memory_order_acquire);
    }
    [[nodiscard]] uint64_t highWaterBytes() const noexcept { return m_high_water_bytes; }
    [[nodiscard]] uint64_t synchronousFallbackCount() const noexcept { return m_synchronous_fallback_count; }
    void recordSynchronousFallback() noexcept { recordFallback(); }

private:
    void recordFallback() noexcept
    {
        ++m_synchronous_fallback_count;
        if (m_synchronous_fallback_count == 1U) {
            std::fputs("height-tile instance reaper: synchronous storage retirement fallback\n", stderr);
        }
    }

    void collect() noexcept
    {
        while (m_executor.tryTakeResult().has_value()) {
            ;
        }
    }

    uint64_t m_maximum_bytes;
    std::atomic<uint64_t> m_outstanding_bytes{ 0U };
    std::atomic<uint32_t> m_outstanding_jobs{ 0U };
    uint64_t m_high_water_bytes = 0U;
    uint64_t m_synchronous_fallback_count = 0U;
    core::executor::Executor m_executor;
};

} // namespace client
