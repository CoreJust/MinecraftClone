#include <client/TerrainLodRuntime.hpp>

#include <client/TerrainLodPresentation.hpp>
#include <client/TerrainLodSelection.hpp>
#include <client/render/StoneIndirectDraws.hpp>
#include <client/render/VulkanRenderer.hpp>
#include <shared/net/Message.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainLod.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>
#include <core/IO/Log.hpp>

#include <algorithm>
#include <chrono>
#include <compare>
#include <exception>
#include <future>
#include <memory>
#include <stop_token>
#include <utility>
#include <vector>

namespace client {

TerrainLodRuntime::~TerrainLodRuntime()
{
    m_cancellation.request_stop();
    if (m_job.valid()) {
        m_job.wait();
    }
}

void TerrainLodRuntime::reportDiagnostics()
{
    auto const now = std::chrono::steady_clock::now();
    if (m_diagnostic_records >= 64U || m_diagnostic_events == m_last_reported_events ||
        now - m_diagnostic_reported < std::chrono::seconds{1}) {
        return;
    }
    m_diagnostic_reported = now;
    m_last_reported_events = m_diagnostic_events;
    ++m_diagnostic_records;
    CORE_INFO(
        "Terrain LOD handoff: dispatched={} completed={} published={} source_rejected={} plan_rejected={} "
        "generation_rejected={} center_rejected={} presented_rejected={} uncaptured_rejected={} "
        "last_plan_reason={} missing_key={},{} job_us={} job_center={},{} current_center={},{} "
        "captured={} retained={} job_pending={} upload_pending={}",
        m_dispatched,
        m_completed,
        m_published,
        m_source_rejected,
        m_plan_rejected,
        m_plan_failure_counts[1],
        m_plan_failure_counts[2],
        m_plan_failure_counts[3],
        m_plan_failure_counts[4],
        static_cast<uint32_t>(m_last_publication_issue.failure),
        m_last_publication_issue.column.x,
        m_last_publication_issue.column.y,
        m_last_job_duration.count(),
        m_job_ticket.center_x,
        m_job_ticket.center_y,
        m_center_x,
        m_center_y,
        m_captured,
        m_columns.size(),
        m_job.valid(),
        m_pending != nullptr
    );
}

void TerrainLodRuntime::reset(VulkanRenderer* const renderer)
{
    m_cancellation.request_stop();
    m_actual_world = nullptr;
    m_world.reset();
    m_pending.reset();
    m_columns.clear();
    m_plan.reset();
    m_discovery_initialized = false;
    m_center_x = 0U;
    m_center_y = 0U;
    m_dirty = true;
    m_actual_reported = false;
    if (m_active && renderer != nullptr) {
        renderer->clearTerrainLodScene();
    }
    m_active = false;
}

void TerrainLodRuntime::update(
    std::shared_ptr<shared::TerrainWorld const> source,
    shared::SparseWorld const* const actual,
    std::optional<shared::Player> const& player,
    VulkanRenderer& renderer,
    std::chrono::steady_clock::time_point const deadline,
    std::optional<shared::TerrainSessionIdentity> session,
    std::optional<uint64_t> const coverage_epoch
)
{
    reportDiagnostics();
    if (actual != m_actual_world) {
        reset(&renderer);
        m_actual_world = actual;
    }
    if (source != m_world) {
        m_cancellation.request_stop();
        m_pending.reset();
        if (m_world) {
            m_columns.clear();
            m_plan.reset();
            m_discovery_initialized = false;
        }
        m_world = source;
        m_dirty = true;
        m_actual_reported = false;
        if (m_active) {
            renderer.clearTerrainLodScene();
            m_active = false;
        }
    }
    if (player) {
        uint32_t const x = shared::WorldBounds::wrapHorizontal(static_cast<int64_t>(shared::playerPositionX(*player)));
        uint32_t const y = shared::WorldBounds::wrapHorizontal(static_cast<int64_t>(shared::playerPositionY(*player)));
        m_plan.updateInterest(x, y);
        std::erase_if(m_columns, [this](auto const& column) { return !m_plan.relevant({column->x, column->y}); });
        if (x / 64U != m_center_x / 64U || y / 64U != m_center_y / 64U) {
            m_center_x = x / 64U * 64U;
            m_center_y = y / 64U * 64U;
            m_discovery_initialized = false;
            m_dirty = true;
        }
    }
    if (m_job.valid() && m_job.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
        try {
            auto candidate = m_job.get();
            ++m_completed;
            ++m_diagnostic_events;
            m_last_job_duration = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - m_job_started
            );
            if (source && m_job_world == source && candidate->identity == source->identity()) {
                m_pending = std::move(candidate);
                m_pending_ticket = m_job_ticket;
            }
        } catch (std::exception const& error) {
            if (!m_cancellation.stop_requested()) {
                m_dirty = true;
                CORE_ERROR("Terrain LOD scene preparation failed: {}", error.what());
            }
        }
    }
    if (m_pending && player) {
        std::vector<TerrainLodColumnKey> rendered;
        for (auto const leaf : m_pending->leaves) {
            if (leaf.level == shared::TerrainLodLevel::L3) {
                rendered.push_back({leaf.x, leaf.y});
            }
        }
        bool const sources_current = actual && terrainLodSourcesCurrent(*m_pending, *actual);
        auto const issue = m_plan.publicationIssue(m_pending_ticket, rendered);
        if (!sources_current || issue.failure != TerrainLodPublicationFailure::None) {
            m_source_rejected += sources_current ? 0U : 1U;
            m_plan_rejected += issue.failure == TerrainLodPublicationFailure::None ? 0U : 1U;
            if (issue.failure != TerrainLodPublicationFailure::None) {
                ++m_plan_failure_counts[static_cast<uint32_t>(issue.failure)];
            }
            m_last_publication_issue = issue;
            ++m_diagnostic_events;
            m_pending.reset();
            m_discovery_initialized = false;
            m_dirty = true;
        } else if (renderer.upsertTerrainLodScene(m_pending, deadline)) {
            if (!m_active || (!m_actual_reported && m_pending->actual_column_count != 0U)) {
                CORE_INFO(
                    "Terrain LOD scene active: model={} leaves={} faces={} "
                    "actual_columns={}",
                    shared::TERRAIN_LOD_MODEL,
                    m_pending->leaves.size(),
                    m_pending->faces.size(),
                    m_pending->actual_column_count
                );
            }
            m_actual_reported = m_actual_reported || m_pending->actual_column_count != 0U;
            ++m_published;
            ++m_diagnostic_events;
            m_plan.presented(rendered);
            m_pending.reset();
            m_active = true;
        }
    }
    if (!player) {
        return;
    }
    if (actual && (!m_discovery_initialized || actual->authoritativeSurfaceRevision() != m_discovery_revision)) {
        std::erase_if(m_columns, [this, actual](auto const& column) {
            if (terrainLodColumnSourcesCurrent(*column, *actual)) {
                return false;
            }
            m_plan.forget({column->x, column->y});
            m_dirty = true;
            return true;
        });
        auto const available = actual->availableAuthoritativeColumns();
        std::vector<TerrainLodColumnKey> keys;
        keys.reserve(available.size());
        for (auto const coordinate : available) {
            keys.push_back({static_cast<uint32_t>(coordinate.x), static_cast<uint32_t>(coordinate.y)});
        }
        m_plan.discover(keys);
        m_discovery_revision = actual->authoritativeSurfaceRevision();
        m_discovery_initialized = true;
    }
    uint32_t captures = 0U;
    while (actual && captures < 8U && std::chrono::steady_clock::now() < deadline) {
        auto const next = m_plan.nextDiscovery();
        if (!next) {
            break;
        }
        TerrainLodColumnKey const key = *next;
        ++captures;
        uint32_t const x = key.x;
        uint32_t const y = key.y;
        auto captured = captureTerrainLodColumn(*actual, x, y);
        auto const existing =
            std::ranges::find_if(m_columns, [x, y](std::shared_ptr<TerrainLodColumnInput const> const& column) {
                return column->x == x && column->y == y;
            });
        if (captured) {
            ++m_captured;
            m_plan.remember(key);
            if (existing == m_columns.end()) {
                m_columns.push_back(std::make_shared<TerrainLodColumnInput const>(std::move(*captured)));
                m_dirty = true;
            } else if (!terrainLodColumnSourcesCurrent(**existing, *actual)) {
                *existing = std::make_shared<TerrainLodColumnInput const>(std::move(*captured));
                m_dirty = true;
            }
        } else if (existing != m_columns.end()) {
            bool const current = terrainLodColumnSourcesCurrent(**existing, *actual);
            if (!current) {
                m_columns.erase(existing);
                m_plan.forget(key);
                m_dirty = true;
            }
        }
    }
    if (!source) {
        return;
    }
    if (m_job.valid() || m_pending || !m_dirty) {
        return;
    }
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> columns;
    columns.reserve(m_columns.size());
    if (actual) {
        for (auto const& column : m_columns) {
            bool const current = terrainLodColumnSourcesCurrent(*column, *actual);
            if (current) {
                columns.push_back(column);
            } else {
                m_plan.forget({column->x, column->y});
            }
        }
    }
    m_cancellation = std::stop_source{};
    auto const cancellation = m_cancellation.get_token();
    m_job_world = source;
    m_job_ticket = m_plan.ticket();
    uint32_t const center_x = m_job_ticket.center_x;
    uint32_t const center_y = m_job_ticket.center_y;
    m_dirty = false;
    ++m_dispatched;
    ++m_diagnostic_events;
    m_job_started = std::chrono::steady_clock::now();
    // NOLINTNEXTLINE(bugprone-exception-escape)
    m_job = std::async(std::launch::async,
        [source = std::move(source), center_x, center_y, session = std::move(session), coverage_epoch,
            columns = std::move(columns), cancellation] {
            auto scene = std::const_pointer_cast<TerrainLodScene>(
                buildTerrainLodSceneFromColumns(*source, center_x, center_y, columns, cancellation)
            );
            scene->session_join_epoch = session && session->terrain == scene->identity ? session->join_epoch : 0U;
            scene->coverage_epoch = coverage_epoch.value_or(0U);
            scene->center_x = center_x;
            scene->center_y = center_y;
            return std::shared_ptr<TerrainLodScene const>{std::move(scene)};
        });
}

} // namespace client
