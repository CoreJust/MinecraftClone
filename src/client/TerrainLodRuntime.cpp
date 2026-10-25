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
#include <span>
#include <stop_token>
#include <utility>
#include <vector>

namespace client {
namespace {

bool capturedTerrainLodColumnCurrent(
    std::shared_ptr<TerrainLodCapturedColumn const> const& column,
    shared::SparseWorld const& world,
    TerrainLodAuthorityBatch const& batch
)
{
    return staleCapturedTerrainLodColumns(std::span{&column, 1U}, world, batch).empty();
}

} // namespace

uint64_t TerrainLodSourceIndex::key(uint32_t const x, uint32_t const y) noexcept
{
    return (static_cast<uint64_t>(x) << 32U) | y;
}

void TerrainLodSourceIndex::clear()
{
    m_columns.clear();
    m_dependents.clear();
    m_changes.coordinates.clear();
    m_residents.clear();
    m_previous_residents.clear();
    m_cursor = 0U;
    m_initialized = false;
}

void TerrainLodSourceIndex::remove(TerrainLodColumnKey const coordinate)
{
    uint64_t const column_key = key(coordinate.x, coordinate.y);
    auto const entry = m_columns.find(column_key);
    if (entry == m_columns.end()) {
        return;
    }
    for (uint64_t const dependency : entry->second.dependencies) {
        auto dependents = m_dependents.find(dependency);
        dependents->second.erase(column_key);
        if (dependents->second.empty()) {
            m_dependents.erase(dependents);
        }
    }
    m_columns.erase(entry);
}

void TerrainLodSourceIndex::upsert(std::shared_ptr<TerrainLodCapturedColumn const> column)
{
    auto const& input = column->input();
    remove({input.x, input.y});
    Entry entry{ .column = std::move(column), .dependencies = {key(input.x, input.y)} };
    for (auto const& source : input.authority_sources) {
        entry.dependencies.push_back(key(static_cast<uint32_t>(source.coordinate.x),
            static_cast<uint32_t>(source.coordinate.y)));
    }
    for (auto const& source : input.sources) {
        entry.dependencies.push_back(key(static_cast<uint32_t>(source.coordinate.x),
            static_cast<uint32_t>(source.coordinate.y)));
    }
    std::ranges::sort(entry.dependencies);
    entry.dependencies.erase(std::unique(entry.dependencies.begin(), entry.dependencies.end()), entry.dependencies.end());
    uint64_t const column_key = key(input.x, input.y);
    for (uint64_t const dependency : entry.dependencies) {
        m_dependents[dependency].insert(column_key);
    }
    m_columns.emplace(column_key, std::move(entry));
}

void TerrainLodSourceIndex::select(
    shared::SparseWorld const& world,
    bool const scan_residents,
    Selection& output)
{
    output.columns.clear();
    output.discovery.clear();
    world.readAuthoritativeColumnChanges(m_cursor, m_changes);
    m_cursor = m_changes.cursor;
    output.full_resync = !m_initialized || m_changes.full_resync;
    m_initialized = true;
    if (scan_residents || !m_changes.coordinates.empty() || output.full_resync) {
        world.residentChunkContents(m_residents);
        auto const less = [](auto const& a, auto const& b) {
            auto const& x = a.coordinate;
            auto const& y = b.coordinate;
            return x.x != y.x ? x.x < y.x : x.y != y.y ? x.y < y.y : x.z < y.z;
        };
        size_t before = 0U;
        size_t after = 0U;
        while (before < m_previous_residents.size() || after < m_residents.size()) {
            if (after == m_residents.size()
                || (before < m_previous_residents.size() && less(m_previous_residents[before], m_residents[after]))) {
                m_changes.coordinates.push_back(m_previous_residents[before++].coordinate);
            } else if (before == m_previous_residents.size() || less(m_residents[after], m_previous_residents[before])) {
                m_changes.coordinates.push_back(m_residents[after++].coordinate);
            } else {
                if (m_previous_residents[before].content != m_residents[after].content) {
                    m_changes.coordinates.push_back(m_residents[after].coordinate);
                }
                ++before;
                ++after;
            }
        }
        m_previous_residents.swap(m_residents);
    }
    std::unordered_set<uint64_t> affected;
    std::unordered_set<uint64_t> discovery;
    for (auto const coordinate : m_changes.coordinates) {
        uint32_t const x = static_cast<uint32_t>(coordinate.x);
        uint32_t const y = static_cast<uint32_t>(coordinate.y);
        auto const dependents = m_dependents.find(key(x, y));
        if (dependents != m_dependents.end()) {
            affected.insert(dependents->second.begin(), dependents->second.end());
        }
        for (auto const offset : {std::array{0, 0}, std::array{-1, 0}, std::array{1, 0},
                std::array{0, -1}, std::array{0, 1}}) {
            uint32_t const cx = static_cast<uint32_t>((static_cast<int32_t>(x) + offset[0] + 4'096) % 4'096);
            uint32_t const cy = static_cast<uint32_t>((static_cast<int32_t>(y) + offset[1] + 4'096) % 4'096);
            discovery.insert(key(cx, cy));
        }
    }
    if (output.full_resync) {
        for (auto const& [column_key, entry] : m_columns) {
            static_cast<void>(column_key);
            output.columns.push_back(entry.column);
        }
    } else {
        for (uint64_t const column_key : affected) {
            output.columns.push_back(m_columns.at(column_key).column);
        }
    }
    std::ranges::sort(output.columns, [](auto const& a, auto const& b) {
        return key(a->input().x, a->input().y) < key(b->input().x, b->input().y);
    });
    for (uint64_t const coordinate : discovery) {
        uint32_t const x = static_cast<uint32_t>(coordinate >> 32U);
        uint32_t const y = static_cast<uint32_t>(coordinate);
        if (world.hasAuthoritativeSurface({static_cast<int32_t>(x), static_cast<int32_t>(y), 0})) {
            output.discovery.push_back({x, y});
        }
    }
    std::ranges::sort(output.discovery, [](auto const a, auto const b) {
        return a.y != b.y ? a.y < b.y : a.x < b.x;
    });
}

TerrainLodRuntime::~TerrainLodRuntime()
{
    m_cancellation.request_stop();
    if (m_job.valid()) {
        m_job.wait();
    }
}

void TerrainLodRuntime::updateEditedColumns(
    shared::SparseWorld const& actual,
    VulkanRenderer& renderer,
    std::chrono::steady_clock::time_point const deadline,
    uint64_t const join_epoch,
    uint64_t const coverage_epoch)
{
    if (m_patch_job.valid()) {
        if (m_patch_job.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
            return;
        }
        try {
            m_patch = m_patch_job.get();
        } catch (std::exception const& error) {
            CORE_ERROR("Terrain edit preparation failed: {}", error.what());
            m_patch_columns.clear();
        }
    }
    if (m_patch) {
        bool const current = m_active && m_world && m_patch->identity == m_world->identity()
            && m_patch->session_join_epoch == join_epoch && m_patch->coverage_epoch == coverage_epoch
            && terrainLodSourcesCurrent(*m_patch, actual);
        if (current && !renderer.replaceTerrainLodGroups(m_patch, deadline)) {
            return;
        }
        if (current) {
            for (auto const& column : m_patch_columns) {
                auto const previous = std::ranges::find_if(m_presented_columns, [&](auto const& entry) {
                    return entry->x == column->x && entry->y == column->y;
                });
                if (previous != m_presented_columns.end()) {
                    *previous = column;
                }
            }
            CORE_INFO("Terrain edit groups published: columns={} surface_revision={}",
                m_patch_columns.size(), actual.authoritativeSurfaceRevision());
            m_dirty = true;
        }
        m_patch.reset();
        m_patch_columns.clear();
    }
    if (!m_active || !m_world || m_presented_join_epoch != join_epoch
        || m_presented_coverage_epoch != coverage_epoch) {
        return;
    }
    uint64_t const sparse_edit_revision = actual.sparseEditRevision();
    if (m_edit_checked_sparse_revision == sparse_edit_revision) {
        return;
    }
    // Only edits enter this lane. Ordinary streamed surface replacement keeps
    // using the background scene path, regardless of its discovery backlog.
    for (auto const& column : m_presented_columns) {
        bool const edited = std::ranges::any_of(column->edit_sources, [&](auto const& stamp) {
            return actual.editChunkRevision(stamp.coordinate) != stamp.revision;
        }) || std::ranges::any_of(column->sources, [&](auto const& stamp) {
            return actual.editChunkRevision(stamp.coordinate) != stamp.edit_revision;
        });
        if (!edited) {
            continue;
        }
        auto captured = captureTerrainLodColumnForBatch(
            actual, column->x, column->y, m_columns_authority_batch
        );
        if (!captured) {
            m_patch_columns.clear();
            return;
        }
        m_patch_columns.emplace_back(captured, &captured->input());
    }
    if (m_patch_columns.empty()) {
        m_edit_checked_sparse_revision = sparse_edit_revision;
        return;
    }
    auto columns = m_patch_columns;
    auto const identity = m_world->identity();
    m_patch_job = std::async(std::launch::async,
        [columns = std::move(columns), identity, join_epoch, coverage_epoch] {
            auto patch = std::const_pointer_cast<TerrainLodScene>(buildTerrainLodColumnPatch(identity, columns));
            patch->session_join_epoch = join_epoch;
            patch->coverage_epoch = coverage_epoch;
            return std::shared_ptr<TerrainLodScene const>{std::move(patch)};
        });
}

void TerrainLodRuntime::refreshColumnSources(
    shared::SparseWorld const& actual,
    bool const scan_residents,
    bool const rediscover)
{
    m_source_index.select(actual, scan_residents, m_source_selection);
    std::vector<TerrainLodColumnCoordinate> stale;
    if (!m_source_selection.columns.empty()) {
        stale = staleCapturedTerrainLodColumns(m_source_selection.columns, actual, m_columns_authority_batch);
    }
    auto const coordinate_less = [](TerrainLodColumnCoordinate const left, TerrainLodColumnCoordinate const right) {
        return left.x != right.x ? left.x < right.x : left.y < right.y;
    };
    std::ranges::sort(stale, coordinate_less);
    for (TerrainLodColumnCoordinate const coordinate : stale) {
        m_plan.forget({coordinate.x, coordinate.y});
        m_source_index.remove({coordinate.x, coordinate.y});
    }
    if (!stale.empty()) {
        std::erase_if(m_columns, [&stale, &coordinate_less](auto const& column) {
            TerrainLodColumnInput const& input = column->input();
            return std::ranges::binary_search(stale, TerrainLodColumnCoordinate{input.x, input.y}, coordinate_less);
        });
        m_dirty = true;
        if (std::ranges::none_of(m_columns, [](auto const& column) {
                return !column->input().authority_sources.empty();
            })) {
            m_columns_authority_batch.reset();
        }
    }
    if (rediscover || m_source_selection.full_resync) {
        auto const available = actual.availableAuthoritativeColumns();
        std::vector<TerrainLodColumnKey> keys;
        keys.reserve(available.size());
        for (auto const coordinate : available) {
            keys.push_back({static_cast<uint32_t>(coordinate.x), static_cast<uint32_t>(coordinate.y)});
        }
        m_plan.discover(keys);
    } else {
        m_plan.discover(m_source_selection.discovery);
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
    m_source_index.clear();
    m_source_selection.columns.clear();
    m_source_selection.discovery.clear();
    m_columns_authority_batch.reset();
    m_presented_columns.clear();
    m_edit_checked_sparse_revision = 0U;
    m_patch.reset();
    m_patch_columns.clear();
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
        m_presented_columns.clear();
        m_patch.reset();
        if (m_world) {
            m_columns.clear();
            m_source_index.clear();
            m_source_selection.columns.clear();
            m_source_selection.discovery.clear();
            m_columns_authority_batch.reset();
            m_plan.reset();
            m_discovery_initialized = false;
        } else if (source && m_columns_authority_batch.hasIdentity()
            && !m_columns_authority_batch.matches(source->identity())) {
            m_columns.clear();
            m_source_index.clear();
            m_source_selection.columns.clear();
            m_source_selection.discovery.clear();
            m_columns_authority_batch.reset();
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
        std::erase_if(m_columns, [this](auto const& column) {
            TerrainLodColumnInput const& input = column->input();
            bool const remove = !m_plan.relevant({input.x, input.y});
            if (remove) {
                m_source_index.remove({input.x, input.y});
            }
            return remove;
        });
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
                m_pending_columns = m_job_columns;
            }
        } catch (std::exception const& error) {
            if (!m_cancellation.stop_requested()) {
                m_dirty = true;
                CORE_ERROR("Terrain LOD scene preparation failed: {}", error.what());
            }
        }
    }
    uint64_t const join_epoch = session && source && session->terrain == source->identity() ? session->join_epoch : 0U;
    if (actual && player) {
        updateEditedColumns(*actual, renderer, deadline, join_epoch, coverage_epoch.value_or(0U));
    }
    if (m_pending && player) {
        std::vector<TerrainLodColumnKey> rendered;
        for (auto const leaf : m_pending->leaves) {
            if (leaf.level == shared::TerrainLodLevel::L3) {
                rendered.push_back({leaf.x, leaf.y});
            }
        }
        bool const sources_current = actual && terrainLodSourcesCurrent(*m_pending, *actual)
            && m_pending->session_join_epoch == join_epoch
            && m_pending->coverage_epoch == coverage_epoch.value_or(0U);
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
        } else if (!m_patch_job.valid() && !m_patch && renderer.upsertTerrainLodScene(m_pending, deadline)) {
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
            m_presented_columns.clear();
            for (auto const& column : m_pending_columns) {
                if (std::ranges::find(rendered, TerrainLodColumnKey{column->x, column->y}) != rendered.end()) {
                    m_presented_columns.push_back(column);
                }
            }
            m_presented_join_epoch = m_pending->session_join_epoch;
            m_presented_coverage_epoch = m_pending->coverage_epoch;
            m_pending.reset();
            m_active = true;
        }
    }
    if (!player) {
        return;
    }
    if (actual) {
        bool const previous_trigger = !m_discovery_initialized
            || actual->authoritativeSurfaceRevision() != m_discovery_revision;
        refreshColumnSources(*actual, previous_trigger, !m_discovery_initialized);
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
        auto captured = captureTerrainLodColumnForBatch(*actual, x, y, m_columns_authority_batch);
        auto const existing =
            std::ranges::find_if(m_columns, [x, y](std::shared_ptr<TerrainLodCapturedColumn const> const& column) {
                return column->input().x == x && column->input().y == y;
            });
        if (captured) {
            ++m_captured;
            m_plan.remember(key);
            if (existing == m_columns.end()) {
                m_source_index.upsert(captured);
                m_columns.push_back(std::move(captured));
                m_dirty = true;
            } else if (!capturedTerrainLodColumnCurrent(*existing, *actual, m_columns_authority_batch)) {
                m_source_index.upsert(captured);
                *existing = std::move(captured);
                m_dirty = true;
            }
        } else if (existing != m_columns.end()) {
            bool const current = capturedTerrainLodColumnCurrent(*existing, *actual, m_columns_authority_batch);
            if (!current) {
                m_source_index.remove(key);
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
        // Dispatch was also an exact-validation boundary before incremental selection.
        refreshColumnSources(*actual, true, false);
        for (auto const& captured_column : m_columns) {
            columns.emplace_back(captured_column, &captured_column->input());
        }
    }
    m_job_columns = columns;
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
