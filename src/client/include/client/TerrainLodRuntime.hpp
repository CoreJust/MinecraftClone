#pragma once

#include <client/TerrainLodPresentation.hpp>

#include <shared/net/Message.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <unordered_set>
#include <utility>
#include <vector>

namespace client {

class VulkanRenderer;

struct TerrainLodColumnKey final {
    uint32_t x = 0U;
    uint32_t y = 0U;
    bool operator==(TerrainLodColumnKey const&) const = default;
};

struct TerrainLodWorkTicket final {
    uint64_t generation = 0U;
    uint32_t center_x = 0U;
    uint32_t center_y = 0U;
    std::vector<TerrainLodColumnKey> columns;
};

enum class TerrainLodPublicationFailure : uint8_t {
    None,
    Generation,
    MissingCenter,
    MissingPresented,
    UncapturedColumn,
};

struct TerrainLodPublicationIssue final {
    TerrainLodPublicationFailure failure = TerrainLodPublicationFailure::None;
    TerrainLodColumnKey column;
};

class TerrainLodRuntimePlan final {
public:
    void reset()
    {
        ++m_generation;
        m_columns.clear();
        m_presented.clear();
        m_discovery.clear();
        m_priority_discovery.clear();
        m_queued.clear();
        m_priority_queued.clear();
        m_priority_streak = 0U;
    }

    [[nodiscard]] bool relevant(TerrainLodColumnKey const key) const noexcept
    {
        if (key.x >= 4'096U || key.y >= 4'096U) {
            return false;
        }
        return distance(key.x, m_center_x / 16U) <= 41U && distance(key.y, m_center_y / 16U) <= 41U;
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void updateInterest(uint32_t const x, uint32_t const y)
    {
        m_center_x = x % 65'536U;
        m_center_y = y % 65'536U;
        std::erase_if(m_columns, [this](uint64_t const key) { return !relevant(decode(key)); });
        std::erase_if(m_presented, [this](uint64_t const key) { return !relevant(decode(key)); });
    }

    void remember(TerrainLodColumnKey const key)
    {
        if (relevant(key)) {
            m_columns.insert(encode(key));
        }
    }

    void forget(TerrainLodColumnKey const key) { m_columns.erase(encode(key)); }

    void discover(std::span<TerrainLodColumnKey const> const available)
    {
        TerrainLodColumnKey const center{m_center_x / 16U, m_center_y / 16U};
        for (auto const key : available) {
            uint64_t const encoded = encode(key);
            if (!relevant(key) || m_columns.contains(encoded)) {
                continue;
            }
            bool const inserted = m_queued.insert(encoded).second;
            bool const near = distance(key.x, center.x) <= 2U && distance(key.y, center.y) <= 2U;
            if (near && m_priority_queued.insert(encoded).second) {
                if (key == center) {
                    m_priority_discovery.push_front(key);
                } else {
                    m_priority_discovery.push_back(key);
                }
            } else if (inserted) {
                m_discovery.push_back(key);
            }
        }
    }

    [[nodiscard]] std::optional<TerrainLodColumnKey> nextDiscovery()
    {
        while (!m_discovery.empty() || !m_priority_discovery.empty()) {
            bool const priority = !m_priority_discovery.empty() && (m_priority_streak < 4U || m_discovery.empty());
            auto& queue = priority ? m_priority_discovery : m_discovery;
            auto const key = queue.front();
            queue.pop_front();
            if (priority) {
                m_priority_queued.erase(encode(key));
                ++m_priority_streak;
            } else {
                m_priority_streak = 0U;
                if (m_priority_queued.contains(encode(key))) {
                    continue;
                }
            }
            if (m_queued.erase(encode(key)) != 0U && relevant(key)) {
                return key;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] TerrainLodWorkTicket ticket() const
    {
        TerrainLodWorkTicket result{m_generation, m_center_x, m_center_y, {}};
        result.columns.reserve(m_columns.size());
        for (uint64_t const key : m_columns) {
            result.columns.push_back(decode(key));
        }
        return result;
    }

    [[nodiscard]] TerrainLodPublicationIssue
    publicationIssue(TerrainLodWorkTicket const& ticket, std::span<TerrainLodColumnKey const> const rendered) const
    {
        if (ticket.generation != m_generation) {
            return {TerrainLodPublicationFailure::Generation, {}};
        }
        std::unordered_set<uint64_t> included;
        included.reserve(rendered.size());
        for (auto const key : rendered) {
            included.insert(encode(key));
        }
        uint64_t const center = encode({m_center_x / 16U, m_center_y / 16U});
        if (m_columns.contains(center) && !included.contains(center)) {
            return {TerrainLodPublicationFailure::MissingCenter, decode(center)};
        }
        for (uint64_t const key : m_presented) {
            if (m_columns.contains(key) && !included.contains(key)) {
                return {TerrainLodPublicationFailure::MissingPresented, decode(key)};
            }
        }
        std::unordered_set<uint64_t> captured;
        captured.reserve(ticket.columns.size());
        for (auto const key : ticket.columns) {
            captured.insert(encode(key));
        }
        for (uint64_t const key : included) {
            if (!captured.contains(key)) {
                return {TerrainLodPublicationFailure::UncapturedColumn, decode(key)};
            }
        }
        return {};
    }

    [[nodiscard]] bool
    canPublish(TerrainLodWorkTicket const& ticket, std::span<TerrainLodColumnKey const> const rendered) const
    {
        return publicationIssue(ticket, rendered).failure == TerrainLodPublicationFailure::None;
    }

    void presented(std::span<TerrainLodColumnKey const> const rendered)
    {
        m_presented.clear();
        m_presented.reserve(rendered.size());
        for (auto const key : rendered) {
            if (relevant(key)) {
                m_presented.insert(encode(key));
            }
        }
    }

private:
    [[nodiscard]] static uint32_t distance(uint32_t const a, uint32_t const b) noexcept
    {
        uint32_t const delta = a > b ? a - b : b - a;
        return std::min(delta, 4'096U - delta);
    }
    [[nodiscard]] static uint64_t encode(TerrainLodColumnKey const key) noexcept
    {
        return (static_cast<uint64_t>(key.x) << 32U) | key.y;
    }
    [[nodiscard]] static TerrainLodColumnKey decode(uint64_t const key) noexcept
    {
        return {static_cast<uint32_t>(key >> 32U), static_cast<uint32_t>(key)};
    }
    uint64_t m_generation = 1U;
    uint32_t m_center_x = 0U;
    uint32_t m_center_y = 0U;
    std::unordered_set<uint64_t> m_columns;
    std::unordered_set<uint64_t> m_presented;
    uint32_t m_priority_streak = 0U;
    std::deque<TerrainLodColumnKey> m_discovery;
    std::deque<TerrainLodColumnKey> m_priority_discovery;
    std::unordered_set<uint64_t> m_priority_queued;
    std::unordered_set<uint64_t> m_queued;
};

class TerrainLodRuntime final {
public:
    ~TerrainLodRuntime();
    void update(
        std::shared_ptr<shared::TerrainWorld const> source,
        shared::SparseWorld const* actual,
        std::optional<shared::Player> const& player,
        VulkanRenderer& renderer,
        std::chrono::steady_clock::time_point deadline,
        std::optional<shared::TerrainSessionIdentity> session = std::nullopt,
        std::optional<uint64_t> coverage_epoch = std::nullopt
    );
    void reset(VulkanRenderer* renderer = nullptr);
    [[nodiscard]] bool active() const noexcept { return m_active; }

private:
    void reportDiagnostics();
    std::chrono::steady_clock::time_point m_diagnostic_reported{};
    std::chrono::steady_clock::time_point m_job_started{};
    std::chrono::microseconds m_last_job_duration{0};
    uint32_t m_diagnostic_records = 0U;
    uint64_t m_dispatched = 0U;
    uint64_t m_completed = 0U;
    uint64_t m_source_rejected = 0U;
    uint64_t m_plan_rejected = 0U;
    std::array<uint64_t, 5> m_plan_failure_counts{};
    uint64_t m_published = 0U;
    uint64_t m_captured = 0U;
    uint64_t m_diagnostic_events = 0U;
    uint64_t m_last_reported_events = 0U;
    TerrainLodPublicationIssue m_last_publication_issue;
    shared::SparseWorld const* m_actual_world = nullptr;
    std::shared_ptr<shared::TerrainWorld const> m_world;
    std::future<std::shared_ptr<TerrainLodScene const>> m_job;
    std::shared_ptr<shared::TerrainWorld const> m_job_world;
    std::shared_ptr<TerrainLodScene const> m_pending;
    std::stop_source m_cancellation;
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> m_columns;
    uint32_t m_center_x = 0U;
    uint32_t m_center_y = 0U;
    uint64_t m_discovery_revision = 0U;
    bool m_discovery_initialized = false;
    TerrainLodRuntimePlan m_plan;
    TerrainLodWorkTicket m_job_ticket;
    TerrainLodWorkTicket m_pending_ticket;
    bool m_active = false;
    bool m_actual_reported = false;
    bool m_dirty = true;
};

} // namespace client
