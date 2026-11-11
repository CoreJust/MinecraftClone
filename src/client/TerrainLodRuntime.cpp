#include <client/TerrainLodRuntime.hpp>

#include <client/TerrainLodPresentation.hpp>
#include <client/TerrainLodSelection.hpp>
#include <client/render/StoneIndirectDraws.hpp>
#include <client/render/TerrainLodUploadPlan.hpp>
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
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stop_token>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace client {
namespace {

bool streamingTraceEnabled()
{
    static bool const enabled = [] {
#if defined(_WIN32)
        char* value = nullptr;
        size_t size = 0U;
        bool const found = _dupenv_s(&value, &size, "MC_DIAGNOSTIC_STREAMING_TRACE") == 0
            && value != nullptr && std::string_view{value} == "1";
        std::free(value);
        return found;
#else
        char const* const value = std::getenv("MC_DIAGNOSTIC_STREAMING_TRACE");
        return value != nullptr && std::string_view{value} == "1";
#endif
    }();
    return enabled;
}

int64_t streamingTraceTime()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct TerrainLodColumnStaleSources final {
    bool chunks = false;
    bool authority = false;
    bool fallback_edits = false;
    bool authority_unavailable_or_incomplete = false;
    bool authority_missing_tile = false;
    bool authority_incomplete_edit_requirements = false;
    bool authority_revision_mismatch = false;
    bool authority_dense_hash_mismatch = false;
    bool authority_other = false;
};

struct CurrentFirstRetentionCounts final {
    uint64_t prior_presented = 0U;
    uint64_t urgent_overlap = 0U;
    uint64_t urgent_new = 0U;
    uint64_t old_retained_fresh = 0U;
    uint64_t retained_edited_visible_stale = 0U;
    uint64_t removed_source_stale = 0U;
    uint64_t removed_nonretained_recenterbase = 0U;
    uint64_t stale_chunk_columns = 0U;
    uint64_t stale_authority_columns = 0U;
    uint64_t stale_fallback_edit_columns = 0U;
    uint64_t authority_unavailable_or_incomplete_columns = 0U;
    uint64_t authority_missing_tile_columns = 0U;
    uint64_t authority_incomplete_edit_requirements_columns = 0U;
    uint64_t authority_revision_mismatch_columns = 0U;
    uint64_t authority_dense_hash_mismatch_columns = 0U;
    uint64_t authority_other_columns = 0U;
};

void recordStaleSources(
    CurrentFirstRetentionCounts& counts, TerrainLodColumnStaleSources const& stale, bool const retained_edited_visible)
{
    counts.stale_chunk_columns += stale.chunks ? 1U : 0U;
    counts.stale_authority_columns += stale.authority ? 1U : 0U;
    counts.stale_fallback_edit_columns += stale.fallback_edits ? 1U : 0U;
    counts.authority_unavailable_or_incomplete_columns +=
        stale.authority_unavailable_or_incomplete ? 1U : 0U;
    counts.authority_missing_tile_columns += stale.authority_missing_tile ? 1U : 0U;
    counts.authority_incomplete_edit_requirements_columns +=
        stale.authority_incomplete_edit_requirements ? 1U : 0U;
    counts.authority_revision_mismatch_columns +=
        stale.authority_revision_mismatch ? 1U : 0U;
    counts.authority_dense_hash_mismatch_columns +=
        stale.authority_dense_hash_mismatch ? 1U : 0U;
    counts.authority_other_columns += stale.authority_other ? 1U : 0U;
    if (retained_edited_visible) {
        ++counts.retained_edited_visible_stale;
    } else {
        ++counts.removed_source_stale;
    }
}

TerrainLodColumnStaleSources terrainLodColumnStaleSources(
    TerrainLodColumnInput const& column, shared::SparseWorld const& world)
{
    TerrainLodColumnStaleSources stale_sources;
    for (auto const& source : column.sources) {
        if (world.editChunkRevision(source.coordinate) != source.edit_revision) {
            stale_sources.chunks = true;
            break;
        }
        shared::SparseBlockQuery const probe = world.queryBlock({
            static_cast<int64_t>(source.coordinate.x) * shared::Chunk::SIDE_LENGTH,
            static_cast<int64_t>(source.coordinate.y) * shared::Chunk::SIDE_LENGTH,
            static_cast<int64_t>(source.coordinate.z) * shared::Chunk::SIDE_LENGTH,
        });
        if (probe.state != shared::GenerationState::Materialized || !probe.block) {
            stale_sources.chunks = true;
            break;
        }
        shared::Chunk const* const resident = world.residentChunk(source.coordinate);
        if (resident != nullptr) {
            stale_sources.chunks = resident->contentIdentity() != source.identity;
        } else {
            auto const snapshot = world.effectiveChunkSnapshot(source.coordinate);
            stale_sources.chunks = !snapshot || snapshot->contentIdentity() != source.identity;
        }
        if (stale_sources.chunks) {
            break;
        }
    }
    if (column.authority_sources.empty()) {
        stale_sources.fallback_edits = std::ranges::any_of(column.edit_sources, [&](auto const& source) {
            return world.editChunkRevision(source.coordinate) != source.revision;
        });
    } else {
        shared::TerrainIdentity const& identity = column.authority_sources.front().identity;
        stale_sources.authority_other = std::ranges::any_of(column.authority_sources, [&](auto const& source) {
            return source.identity != identity;
        });
        for (auto const& source : column.authority_sources) {
            auto const current = world.currentAuthoritativeTileVersionState(source.coordinate);
            if (!current) {
                stale_sources.authority_unavailable_or_incomplete = true;
                if (world.authoritativeTileIdentity(source.coordinate)) {
                    stale_sources.authority_incomplete_edit_requirements = true;
                } else {
                    stale_sources.authority_missing_tile = true;
                }
            } else {
                stale_sources.authority_revision_mismatch |= current->revision != source.revision;
                stale_sources.authority_dense_hash_mismatch |=
                    current->dense_content_hash != source.dense_content_hash;
            }
        }
        bool const matches = world.matchesAuthoritativeTileVersions(identity, column.authority_sources);
        if (!matches && !stale_sources.authority_unavailable_or_incomplete
            && !stale_sources.authority_revision_mismatch && !stale_sources.authority_dense_hash_mismatch) {
            stale_sources.authority_other = true;
        }
        stale_sources.authority = stale_sources.authority_other || stale_sources.authority_unavailable_or_incomplete
            || stale_sources.authority_revision_mismatch || stale_sources.authority_dense_hash_mismatch;
    }
    return stale_sources;
}

class ScopedDiagnosticsTimer final {
public:
    explicit ScopedDiagnosticsTimer(std::chrono::nanoseconds* const duration)
        : m_duration(duration)
    {
        if (m_duration != nullptr) {
            m_started = std::chrono::steady_clock::now();
        }
    }

    ~ScopedDiagnosticsTimer() { finish(); }

    void finish()
    {
        if (m_duration != nullptr) {
            *m_duration += std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - m_started
            );
            m_duration = nullptr;
        }
    }

private:
    std::chrono::nanoseconds* m_duration;
    std::chrono::steady_clock::time_point m_started{};
};

bool capturedTerrainLodColumnCurrent(
    std::shared_ptr<TerrainLodCapturedColumn const> const& column,
    shared::SparseWorld const& world,
    TerrainLodAuthorityBatch const& batch
)
{
    return staleCapturedTerrainLodColumns(std::span{&column, 1U}, world, batch).empty();
}

} // namespace

void selectPresentedTerrainLodColumns(
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> pending,
    std::span<TerrainLodColumnKey const> rendered,
    std::vector<std::shared_ptr<TerrainLodColumnInput const>>& presented,
    std::vector<std::shared_ptr<TerrainLodColumnInput const>>& retired)
{
    std::vector<uint64_t> membership;
    membership.reserve(rendered.size());
    for (auto const key : rendered) {
        membership.push_back((static_cast<uint64_t>(key.x) << 32U) | key.y);
    }
    std::ranges::sort(membership);
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> selected;
    selected.reserve(std::min(pending.size(), rendered.size()));
    for (auto const& column : pending) {
        uint64_t const key = (static_cast<uint64_t>(column->x) << 32U) | column->y;
        if (std::ranges::binary_search(membership, key)) {
            selected.push_back(column);
        }
    }
    retired.swap(presented);
    presented = std::move(selected);
}


bool mergeTerrainLodLocalColumns(
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> base,
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> live,
    std::span<TerrainLodColumnCoordinate const> targets,
    std::vector<std::shared_ptr<TerrainLodColumnInput const>>& merged)
{
    merged.assign(base.begin(),base.end());
    for (auto q : targets) {
        auto input = std::ranges::find_if(live,[=](auto const& v) { return v->x == q.x && v->y == q.y; });
        if (input == live.end()) {
            return false;
        }
        auto old = std::ranges::find_if(merged,[=](auto const& v) { return v->x == q.x && v->y == q.y; });
        if (old == merged.end()) {
            merged.push_back(*input);
        } else {
            *old = *input;
        }
    }
    return true;
}
bool selectTerrainLodLocalColumns(
    TerrainLodRuntimePlan const& plan,
    std::span<std::shared_ptr<TerrainLodCapturedColumn const> const> captured,
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> presented,
    std::vector<std::shared_ptr<TerrainLodColumnInput const>>& columns,
    std::vector<TerrainLodColumnCoordinate>& targets,
    std::span<TerrainLodColumnCoordinate const> edited,
    std::optional<TerrainLodColumnCoordinate> const visual_center)
{
    columns.assign(presented.begin(),presented.end());
    targets.clear();
    auto const relevant = [&](TerrainLodColumnKey const key) {
        return plan.visuallyRelevant(key)
            && (!visual_center || terrainLodVisualColumn(key.x, key.y, visual_center->x, visual_center->y));
    };
    auto add = [&](TerrainLodColumnKey key) {
        if (!relevant(key)) {
            return;
        }
        auto ready = std::ranges::find_if(captured,[=](auto const& v) { return v->input().x == key.x && v->input().y == key.y; });
        if (ready == captured.end()) {
            return;
        }
        auto old = std::ranges::find_if(columns,[=](auto const& v) { return v->x == key.x && v->y == key.y; });
        if (old != columns.end()) {
            return;
        }
        columns.emplace_back(*ready,&(*ready)->input());
        targets.push_back({key.x,key.y});
    };
    auto const center = plan.currentColumn();
    // Edge and corner neighbors cover body travel and reachable targets across a column boundary.
    for (auto offset : {std::pair<int32_t, int32_t>{0,0}, std::pair<int32_t, int32_t>{-1,0},
            std::pair<int32_t, int32_t>{1,0}, std::pair<int32_t, int32_t>{0,-1},
            std::pair<int32_t, int32_t>{0,1}, std::pair<int32_t, int32_t>{-1,-1},
            std::pair<int32_t, int32_t>{1,-1}, std::pair<int32_t, int32_t>{-1,1},
            std::pair<int32_t, int32_t>{1,1}}) {
        add({uint32_t((static_cast<int32_t>(center.x) + offset.first + 4'096) % 4'096),
            uint32_t((static_cast<int32_t>(center.y) + offset.second + 4'096) % 4'096)});
    }
    for (auto q : edited) {
        if (std::ranges::any_of(columns,[=](auto const& v) { return v->x == q.x && v->y == q.y; })
            && std::ranges::find(targets,q) == targets.end()) {
            targets.push_back(q);
        }
    }
    if (!targets.empty()) {
        return true;
    }
    std::vector<uint64_t> presented_keys;
    presented_keys.reserve(columns.size());
    for (auto const& column : columns) {
        presented_keys.push_back((static_cast<uint64_t>(column->x) << 32U) | column->y);
    }
    std::ranges::sort(presented_keys);

    std::optional<TerrainLodColumnKey> nearest;
    uint32_t distance = std::numeric_limits<uint32_t>::max();
    for (auto const& ready : captured) {
        TerrainLodColumnKey key{ready->input().x,ready->input().y};
        if (!relevant(key)
            || std::ranges::binary_search(presented_keys, (static_cast<uint64_t>(key.x) << 32U) | key.y)) {
            continue;
        }
        auto const d = terrainLodVisualDistanceSquared(key.x,key.y,center.x,center.y);
        if (!nearest || d < distance) {
            nearest = key;
            distance = d;
        }
    }
    if (nearest) {
        add(*nearest);
    }
    return !targets.empty();
}

bool selectTerrainLodSceneColumns(
    TerrainLodRuntimePlan const& plan,
    std::span<std::shared_ptr<TerrainLodCapturedColumn const> const> const captured,
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> const presented,
    bool const active,
    std::vector<std::shared_ptr<TerrainLodColumnInput const>>& columns)
{
    columns.clear();
    std::unordered_set<uint64_t> included;
    for (auto const& column : presented) {
        included.insert((static_cast<uint64_t>(column->x) << 32U) | column->y);
    }
    for (auto const& column : captured) {
        auto const& input = column->input();
        if (!active || included.contains((static_cast<uint64_t>(input.x) << 32U) | input.y)) {
            columns.emplace_back(column, &column->input());
        }
    }
    return active && selectTerrainLodFrontierColumns(plan, captured, columns);
}

bool selectTerrainLodFrontierColumns(
    TerrainLodRuntimePlan const& plan,
    std::span<std::shared_ptr<TerrainLodCapturedColumn const> const> const captured,
    std::vector<std::shared_ptr<TerrainLodColumnInput const>>& columns)
{
    size_t const previous_size = columns.size();
    std::erase_if(columns, [&](auto const& column) { return !plan.visuallyRelevant({column->x, column->y}); });
    std::unordered_map<uint64_t, size_t> included;
    for (size_t i = 0U; i < columns.size(); ++i) {
        included.emplace((static_cast<uint64_t>(columns[i]->x) << 32U) | columns[i]->y, i);
    }
    bool changed = columns.size() != previous_size;
    std::shared_ptr<TerrainLodCapturedColumn const> next;
    uint32_t nearest = std::numeric_limits<uint32_t>::max();
    auto const current = plan.currentColumn();
    for (auto const& column : captured) {
        auto const& input = column->input();
        if (!plan.visuallyRelevant({input.x, input.y})) {
            continue;
        }
        uint64_t const key = (static_cast<uint64_t>(input.x) << 32U) | input.y;
        auto const found = included.find(key);
        if (found == included.end()) {
            uint32_t const priority = terrainLodVisualDistanceSquared(input.x, input.y, current.x, current.y);
            if (!next || priority < nearest
                || (priority == nearest && key < ((static_cast<uint64_t>(next->input().x) << 32U) | next->input().y))) {
                next = column;
                nearest = priority;
            }
        } else {
            std::shared_ptr<TerrainLodColumnInput const> ready{column, &column->input()};
            if (columns[found->second] != ready) {
                columns[found->second] = std::move(ready);
                changed = true;
            }
        }
    }
    // One exact footprint and its transition rim form the publication unit.
    // A slow unrelated capture cannot turn ready destinations into a batch.
    if (next) {
        columns.emplace_back(next, &next->input());
        changed = true;
    }
    return changed;
}

namespace {
bool terrainLodColumnEdited(TerrainLodColumnInput const& column, shared::SparseWorld const& actual)
{
    return std::ranges::any_of(column.edit_sources, [&](auto const& stamp) {
        return actual.editChunkRevision(stamp.coordinate) != stamp.revision;
    }) || std::ranges::any_of(column.sources, [&](auto const& stamp) {
        return actual.editChunkRevision(stamp.coordinate) != stamp.edit_revision;
    });
}

bool captureTerrainLodColumnInputs(
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> const columns,
    shared::SparseWorld const& actual,
    TerrainLodAuthorityBatch& batch,
    std::vector<std::shared_ptr<TerrainLodColumnInput const>>& ready,
    TerrainLodUpdateDiagnostics* const diagnostics)
{
    ready.clear();
    bool incomplete = false;
    for (auto const& column : columns) {
        ScopedDiagnosticsTimer capture_timer{
            diagnostics != nullptr ? &diagnostics->edit_capture_duration : nullptr};
        if (diagnostics != nullptr) {
            ++diagnostics->edit_capture_pass_count;
        }
        auto captured = captureTerrainLodColumnForBatch(actual, column->x, column->y, batch);
        if (!captured) {
            incomplete = true;
            continue;
        }
        ready.emplace_back(captured, &captured->input());
    }
    return !incomplete;
}
} // namespace

bool captureEditedTerrainLodColumns(
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> const presented,
    shared::SparseWorld const& actual,
    TerrainLodAuthorityBatch& batch,
    std::vector<std::shared_ptr<TerrainLodColumnInput const>>& ready,
    TerrainLodUpdateDiagnostics* const diagnostics)
{
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> edited;
    {
        ScopedDiagnosticsTimer detection_timer{
            diagnostics != nullptr ? &diagnostics->edit_detection_duration : nullptr};
        if (diagnostics != nullptr) {
            ++diagnostics->edit_detection_pass_count;
        }
        for (auto const& column : presented) {
            if (diagnostics != nullptr) {
                ++diagnostics->edit_detection_column_count;
            }
            if (terrainLodColumnEdited(*column, actual)) {
                edited.push_back(column);
            }
        }
    }
    return captureTerrainLodColumnInputs(edited, actual, batch, ready, diagnostics);
}

uint64_t TerrainLodPresentedEditIndex::key(uint32_t const x, uint32_t const y) noexcept
{
    return (static_cast<uint64_t>(x) << 32U) | y;
}

void TerrainLodPresentedEditIndex::reset()
{
    m_columns.clear();
    m_dependents.clear();
    m_dirty.clear();
    m_changes.coordinates.clear();
    m_cursor = 0U;
    m_sparse_revision = 0U;
    m_initialized = false;
}

void TerrainLodPresentedEditIndex::remove(uint64_t const column_key)
{
    auto const found = m_columns.find(column_key);
    if (found == m_columns.end()) {
        return;
    }
    for (uint64_t const dependency : found->second.dependencies) {
        auto dependents = m_dependents.find(dependency);
        dependents->second.erase(column_key);
        if (dependents->second.empty()) {
            m_dependents.erase(dependents);
        }
    }
    m_columns.erase(found);
    m_dirty.erase(column_key);
}

void TerrainLodPresentedEditIndex::upsert(std::shared_ptr<TerrainLodColumnInput const> column)
{
    uint64_t const column_key = key(column->x, column->y);
    auto const previous = m_columns.find(column_key);
    if (previous != m_columns.end() && previous->second.column == column) {
        return;
    }
    remove(column_key);
    Entry entry{.column = std::move(column), .dependencies = {}};
    auto const add_dependency = [&](auto const& source) {
        uint64_t const dependency = key(static_cast<uint32_t>(source.coordinate.x),
            static_cast<uint32_t>(source.coordinate.y));
        // Vertical edit stamps arrive grouped by XY; keep one dependency for
        // each run before deduplicating overlaps between the source classes.
        if (entry.dependencies.empty() || entry.dependencies.back() != dependency) {
            entry.dependencies.push_back(dependency);
        }
    };
    for (auto const& source : entry.column->authority_sources) {
        add_dependency(source);
    }
    for (auto const& source : entry.column->sources) {
        add_dependency(source);
    }
    for (auto const& source : entry.column->edit_sources) {
        add_dependency(source);
    }
    std::ranges::sort(entry.dependencies);
    entry.dependencies.erase(std::unique(entry.dependencies.begin(), entry.dependencies.end()), entry.dependencies.end());
    for (uint64_t const dependency : entry.dependencies) {
        m_dependents[dependency].insert(column_key);
    }
    m_columns.emplace(column_key, std::move(entry));
}

void TerrainLodPresentedEditIndex::replace(
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> const presented,
    shared::SparseWorld const& world)
{
    if (!m_initialized) {
        // Publication already checked these captures. Start after its events,
        // rather than rescanning every published input at the first edit.
        world.readAuthoritativeColumnChanges(m_cursor, m_changes);
        m_cursor = m_changes.cursor;
        m_sparse_revision = world.sparseEditRevision();
        m_initialized = true;
    }
    std::unordered_set<uint64_t> retained;
    retained.reserve(presented.size());
    for (auto const& column : presented) {
        retained.insert(key(column->x, column->y));
        upsert(column);
    }
    std::vector<uint64_t> removed;
    for (auto const& [column_key, entry] : m_columns) {
        static_cast<void>(entry);
        if (!retained.contains(column_key)) {
            removed.push_back(column_key);
        }
    }
    for (uint64_t const column_key : removed) {
        remove(column_key);
    }
}

void TerrainLodPresentedEditIndex::select(
    shared::SparseWorld const& world,
    std::vector<std::shared_ptr<TerrainLodColumnInput const>>& edited,
    TerrainLodUpdateDiagnostics* const diagnostics)
{
    edited.clear();
    ScopedDiagnosticsTimer detection_timer{
        diagnostics != nullptr ? &diagnostics->edit_detection_duration : nullptr};
    uint64_t const sparse_revision = world.sparseEditRevision();
    // Advance through ordinary streaming too, without checking sparse stamps.
    // Otherwise a long flight would overflow this cursor before the next edit.
    world.readAuthoritativeColumnChanges(m_cursor, m_changes);
    m_cursor = m_changes.cursor;
    if (!m_initialized || sparse_revision != m_sparse_revision) {
        if (!m_initialized || m_changes.full_resync) {
            for (auto const& [column_key, entry] : m_columns) {
                static_cast<void>(entry);
                m_dirty.insert(column_key);
            }
        } else {
            for (auto const coordinate : m_changes.coordinates) {
                auto const dependents = m_dependents.find(key(static_cast<uint32_t>(coordinate.x),
                    static_cast<uint32_t>(coordinate.y)));
                if (dependents != m_dependents.end()) {
                    m_dirty.insert(dependents->second.begin(), dependents->second.end());
                }
            }
        }
        m_initialized = true;
        m_sparse_revision = sparse_revision;
    }
    if (m_dirty.empty()) {
        return;
    }
    if (diagnostics != nullptr) {
        ++diagnostics->edit_detection_pass_count;
    }
    for (auto pending = m_dirty.begin(); pending != m_dirty.end();) {
        auto const& column = m_columns.at(*pending).column;
        if (diagnostics != nullptr) {
            ++diagnostics->edit_detection_column_count;
        }
        if (terrainLodColumnEdited(*column, world)) {
            edited.push_back(column);
            ++pending;
        } else {
            pending = m_dirty.erase(pending);
        }
    }
    std::ranges::sort(edited, [](auto const& left, auto const& right) {
        return key(left->x, left->y) < key(right->x, right->y);
    });
}

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
    if (input.authority_sources.empty()) {
        // Each fallback edit source column records every z; z=0 supplies its journal key.
        for (auto const& source : input.edit_sources) {
            if (source.coordinate.z == 0) {
                entry.dependencies.push_back(key(static_cast<uint32_t>(source.coordinate.x),
                    static_cast<uint32_t>(source.coordinate.y)));
            }
        }
    }
    std::ranges::sort(entry.dependencies);
    entry.dependencies.erase(std::unique(entry.dependencies.begin(), entry.dependencies.end()), entry.dependencies.end());
    uint64_t const column_key = key(input.x, input.y);
    for (uint64_t const dependency : entry.dependencies) {
        m_dependents[dependency].insert(column_key);
    }
    m_columns.emplace(column_key, std::move(entry));
}

std::shared_ptr<TerrainLodCapturedColumn const> TerrainLodSourceIndex::find(
    TerrainLodColumnKey const coordinate) const
{
    auto const found = m_columns.find(key(coordinate.x, coordinate.y));
    return found == m_columns.end() ? nullptr : found->second.column;
}

void TerrainLodSourceIndex::select(
    shared::SparseWorld const& world,
    bool const scan_residents,
    Selection& output,
    TerrainLodSourceIndexDiagnostics* const diagnostics)
{
    output.columns.clear();
    output.discovery.clear();
    ScopedDiagnosticsTimer change_read_timer{
        diagnostics != nullptr ? &diagnostics->change_read_duration : nullptr};
    world.readAuthoritativeColumnChanges(m_cursor, m_changes);
    change_read_timer.finish();
    m_cursor = m_changes.cursor;
    output.full_resync = !m_initialized || m_changes.full_resync;
    m_initialized = true;
    if (diagnostics != nullptr) {
        ++diagnostics->selection_count;
        diagnostics->authoritative_change_count += m_changes.coordinates.size();
        diagnostics->full_resync_count += output.full_resync ? 1U : 0U;
    }
    if (scan_residents || !m_changes.coordinates.empty() || output.full_resync) {
        ScopedDiagnosticsTimer resident_snapshot_timer{
            diagnostics != nullptr ? &diagnostics->resident_snapshot_duration : nullptr};
        world.residentChunkContents(m_residents);
        resident_snapshot_timer.finish();
        if (diagnostics != nullptr) {
            ++diagnostics->resident_snapshot_count;
            diagnostics->resident_chunk_count += m_residents.size();
        }
        auto const less = [](auto const& a, auto const& b) {
            auto const& x = a.coordinate;
            auto const& y = b.coordinate;
            return x.x != y.x ? x.x < y.x : x.y != y.y ? x.y < y.y : x.z < y.z;
        };
        size_t before = 0U;
        size_t after = 0U;
        size_t const change_count_before_diff = m_changes.coordinates.size();
        ScopedDiagnosticsTimer resident_diff_timer{
            diagnostics != nullptr ? &diagnostics->resident_diff_duration : nullptr};
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
        resident_diff_timer.finish();
        if (diagnostics != nullptr) {
            diagnostics->resident_diff_change_count +=
                m_changes.coordinates.size() - change_count_before_diff;
        }
    }
    ScopedDiagnosticsTimer dependency_selection_timer{
        diagnostics != nullptr ? &diagnostics->dependency_selection_duration : nullptr};
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
    dependency_selection_timer.finish();
    if (diagnostics != nullptr) {
        diagnostics->affected_column_count += affected.size();
        diagnostics->selected_column_count += output.columns.size();
        diagnostics->discovery_candidate_count += discovery.size();
    }
}


void TerrainLodRuntime::startLocalJob(
    std::shared_ptr<shared::TerrainWorld const> source, std::shared_ptr<TerrainLodScene const> base,
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> base_columns,
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> columns,
    std::vector<TerrainLodColumnCoordinate> targets,
    std::vector<std::shared_ptr<TerrainLodLocalUnit const>> constraints,
    TerrainLodWorkTicket ticket, bool far, Recenter recenter)
{
    ticket.columns.clear();
    for (auto const& input : columns) {
        ticket.columns.push_back({input->x,input->y});
    }
    uint64_t const trace_id = ++m_local_job_trace_sequence;
    m_local_job_trace_id = trace_id;
    uint64_t const travel_generation = recenter == Recenter::Ahead ? m_travel->generation : 0U;
    auto const current_key = m_plan.currentColumn();
    bool const current_target = std::ranges::find(
        targets, TerrainLodColumnCoordinate{current_key.x,current_key.y}) != targets.end();
    auto const visual_center = recenter == Recenter::Settle
        ? std::optional{TerrainLodColumnCoordinate{current_key.x, current_key.y}} : std::nullopt;
    m_local_job_far = far;
    m_local_job_recenter = recenter;
    m_local_job_started_with_far_ready = m_far_ready.has_value();
    m_local_job_targets_current = current_target;
    bool const current_in_base = base && std::ranges::any_of(base->leaves, [current_key](auto const leaf) {
        return leaf.level == shared::TerrainLodLevel::L3 && leaf.x == current_key.x && leaf.y == current_key.y;
    });
    if (streamingTraceEnabled()) {
        CORE_INFO(
            "stream_trace event=local_start ns={} job={} far={} far_ready_queued={} key_x={} key_y={} "
            "targets={} current_target={} base_scene={} base_has_current={} inputs={} visual_recenter={} "
            "current_first={} ahead_mixed={}",
            streamingTraceTime(), trace_id, far, m_local_job_started_with_far_ready,
            current_key.x, current_key.y, targets.size(), current_target,
            reinterpret_cast<uintptr_t>(base.get()), current_in_base, columns.size(),
            recenter != Recenter::None, recenter == Recenter::CurrentFirst, recenter == Recenter::Ahead);
        for (size_t i = 0U; i < targets.size(); ++i) {
            CORE_INFO("stream_trace event=local_target ns={} job={} index={} key_x={} key_y={} current={}",
                streamingTraceTime(), trace_id, i, targets[i].x, targets[i].y,
                targets[i].x == current_key.x && targets[i].y == current_key.y);
        }
    }
    m_local_cancellation = std::stop_source{};
    auto cancellation = m_local_cancellation.get_token();
    auto retired_job_columns = std::move(m_retired_job_columns);
    auto retired_presented_columns = std::move(m_retired_presented_columns);
    m_local_job = std::async(std::launch::async,
        [source = std::move(source),base = std::move(base),base_columns = std::move(base_columns),
         columns = std::move(columns),targets = std::move(targets),constraints = std::move(constraints),
         ticket = std::move(ticket),far,cancellation,trace_id,recenter,travel_generation,visual_center,
         retired_job_columns = std::move(retired_job_columns),
         retired_presented_columns = std::move(retired_presented_columns)]() mutable {
            TerrainLodLocalPreparation prepared;
            try {
                if (isTransientRecenter(recenter)) {
                    prepared = prepareTerrainLodLocalScene(
                        *source, *base, base_columns, columns, targets, constraints, cancellation, true);
                    if (streamingTraceEnabled()) {
                        CORE_INFO("stream_trace event={} job={} prepared={}",
                            recenter == Recenter::Ahead ? "ahead_mixed_local" : "current_first_local",
                            trace_id, static_cast<bool>(prepared.scene));
                    }
                    // The mixed footprint needs exact-base and whole-scene source validation.
                    prepared.unit.reset();
                }
                if (!prepared.scene && (recenter == Recenter::CurrentFirst || recenter == Recenter::Settle)) {
                    // Only the same immutable exact capture can reuse previous column geometry.
                    std::unordered_set<TerrainLodColumnInput const*> selected;
                    selected.reserve(columns.size());
                    for (auto const& input : columns) {
                        selected.insert(input.get());
                    }
                    std::erase_if(base_columns, [&](auto const& input) {
                        return !selected.contains(input.get());
                    });
                    auto scene = std::const_pointer_cast<TerrainLodScene>(buildTerrainLodSceneFromColumns(
                        *source, base->center_x, base->center_y, columns, cancellation, base.get(),
                        base_columns, true, visual_center));
                    scene->session_join_epoch = base->session_join_epoch;
                    scene->coverage_epoch = base->coverage_epoch;
                    prepared.scene = std::move(scene);
                } else if (recenter == Recenter::None) {
                    prepared = prepareTerrainLodLocalScene(
                        *source, *base, base_columns, columns, targets, constraints, cancellation);
                }
            } catch (...) {
                retired_job_columns.clear();
                retired_presented_columns.clear();
                throw;
            }
            retired_job_columns.clear();
            retired_presented_columns.clear();
            return LocalJobResult{
                std::move(prepared),std::move(columns),std::move(base),std::move(ticket),far,
                recenter,trace_id,travel_generation,std::move(targets)};
        });
}
std::shared_ptr<TerrainLodColumnInput const> TerrainLodRuntime::validatedSettleForwardInput(
    shared::SparseWorld const& actual, std::string_view const action)
{
    if (!m_recenter_needs_settle
        || !m_presented_scene || !m_presented_scene->local_layout || m_presented_scene->visual_center
        || !m_travel || !m_travel->forward || m_travel->ahead_dispatched
        || !m_edited_columns.empty() || !m_local_edit_targets.empty()) {
        return {};
    }

    // The tracked forward key is cardinally adjacent, so it is in the ordinary local selector's first pass.
    auto const forward = *m_travel->forward;
    if (!m_plan.visuallyRelevant(forward)) {
        return {};
    }
    if (std::ranges::any_of(m_presented_columns, [forward](auto const& column) {
            return column->x == forward.x && column->y == forward.y;
        })) {
        return {};
    }

    auto const found = m_source_index.find(forward);
    bool const captured = found && !found->input().authority_sources.empty();
    auto& probe = m_travel->probe;
    bool const same_capture = probe && !probe->capture.owner_before(found) && !found.owner_before(probe->capture);
    if (!captured && same_capture) {
        return {};
    }

    // Dense content can change without incrementing the sparse authority or edit revisions.
    bool const fresh = captured && capturedTerrainLodColumnCurrent(found, actual, m_columns_authority_batch);
    std::string_view const verdict = !captured ? "absent" : fresh ? "ready" : "stale";
    if (streamingTraceEnabled() && (!probe || !same_capture || probe->verdict != verdict)) {
        auto const current = m_plan.currentColumn();
        CORE_INFO(
            "stream_trace event=settle_forward_probe ns={} action={} fresh={} verdict={} job={} scene={} "
            "key_x={} key_y={} forward_x={} forward_y={} join={} coverage={}",
            streamingTraceTime(), action, fresh, verdict, m_local_job_trace_id,
            reinterpret_cast<uintptr_t>(m_presented_scene.get()), current.x, current.y, forward.x, forward.y,
            m_presented_join_epoch, m_presented_coverage_epoch);
    }
    probe = ColumnTravel::Probe{found, verdict};
    return fresh ? std::shared_ptr<TerrainLodColumnInput const>{found, &found->input()} : nullptr;
}

void TerrainLodRuntime::prepareFarHandoff()
{
    if (!m_far_ready || m_local_job.valid() || m_pending || m_patch_job.valid() || m_patch) {
        return;
    }
    std::vector<TerrainLodColumnCoordinate> targets;
    auto add = [&](TerrainLodColumnCoordinate q) {
        if (std::ranges::find(targets,q) == targets.end()) {
            targets.push_back(q);
        }
    };
    for (auto const& unit : m_local_units) {
        for (auto q : terrainLodLocalTargets(*unit)) {
            add(q);
        }
    }
    for (auto q : m_local_edit_targets) {
        add(q);
    }
    // Missing center/presented guards are repaired by geometry preparation, never suppressed.
    std::vector<uint64_t> rendered;
    rendered.reserve(m_far_ready->scene->actual_column_count);
    for (auto const leaf : m_far_ready->scene->leaves) {
        if (leaf.level == shared::TerrainLodLevel::L3) {
            rendered.push_back((static_cast<uint64_t>(leaf.x) << 32U) | leaf.y);
        }
    }
    std::ranges::sort(rendered);
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> live = m_presented_columns;
    std::vector<TerrainLodColumnCoordinate> frontier;
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> ready;
    selectTerrainLodLocalColumns(m_plan,m_columns,live,ready,frontier,m_local_edit_targets);
    for (auto const& input : ready) {
        uint64_t const key = (static_cast<uint64_t>(input->x) << 32U) | input->y;
        if (!std::ranges::binary_search(rendered, key) && m_plan.visuallyRelevant({input->x,input->y})) {
            add({input->x,input->y});
        }
    }
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> merged;
    if (targets.empty()) {
        m_pending = std::move(m_far_ready->scene);
        m_pending_columns = std::move(m_far_ready->columns);
        m_pending_ticket = std::move(m_far_ticket);
        m_pending_unit.reset();
        m_pending_base.reset();
        m_pending_far = true;
        m_far_ready.reset();
    } else if (mergeTerrainLodLocalColumns(m_far_ready->columns,ready,targets,merged)) {
        auto base = std::move(*m_far_ready);
        m_far_ready.reset();
        startLocalJob(m_world,std::move(base.scene),std::move(base.columns),std::move(merged),
            std::move(targets),m_local_units,m_far_ticket,true);
    } else {
        m_far_ready.reset();
        m_dirty = true;
    }
}

TerrainLodRuntime::~TerrainLodRuntime()
{
    m_cancellation.request_stop();
    m_local_cancellation.request_stop();
    if (m_local_job.valid()) {
        m_local_job.wait();
    }
    if (m_job.valid()) {
        m_job.wait();
    }
}

void TerrainLodRuntime::updateEditedColumns(
    shared::SparseWorld const& actual,
    VulkanRenderer& renderer,
    std::chrono::steady_clock::time_point const deadline,
    uint64_t const join_epoch,
    uint64_t const coverage_epoch,
    TerrainLodUpdateDiagnostics* const diagnostics)
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
            m_edit_checkpoint.discard();
        }
    }
    if (m_patch) {
        bool const current = m_active && m_world && m_patch->identity == m_world->identity()
            && m_patch->session_join_epoch == join_epoch && m_patch->coverage_epoch == coverage_epoch
            && terrainLodSourcesCurrent(*m_patch, actual);
        std::shared_ptr<TerrainLodScene const> presented;
        if (current) {
            TerrainLodRendererDiagnostics renderer_diagnostics{};
            ScopedDiagnosticsTimer replace_timer{
                diagnostics != nullptr ? &diagnostics->edit_renderer_replace_duration : nullptr};
            if (diagnostics != nullptr) {
                ++diagnostics->edit_renderer_replace_pass_count;
            }
            presented = diagnostics != nullptr
                ? renderer.replaceTerrainLodGroups(m_patch, deadline, &renderer_diagnostics)
                : renderer.replaceTerrainLodGroups(m_patch, deadline);
            replace_timer.finish();
            if (diagnostics != nullptr) {
                diagnostics->renderer_plan_duration += renderer_diagnostics.plan_duration;
                diagnostics->renderer_buffer_update_duration += renderer_diagnostics.buffer_update_duration;
                diagnostics->renderer_publish_duration += renderer_diagnostics.publish_duration;
                diagnostics->renderer_uploaded_face_count += renderer_diagnostics.uploaded_face_count;
                diagnostics->prepared_draw_index_publish_count +=
                    renderer_diagnostics.prepared_draw_index_publish_count;
                diagnostics->render_thread_draw_index_build_count +=
                    renderer_diagnostics.render_thread_draw_index_build_count;
            }
            if (!presented) {
                return;
            }
        }
        if (current) {
            for (auto const& column : m_patch_columns) {
                auto const previous = std::ranges::find_if(m_presented_columns, [&](auto const& entry) {
                    return entry->x == column->x && entry->y == column->y;
                });
                if (previous != m_presented_columns.end()) {
                    *previous = column;
                    m_presented_edit_index.upsert(column);
                    TerrainLodColumnCoordinate const q{column->x,column->y};
                    if (std::ranges::find(m_local_edit_targets,q) == m_local_edit_targets.end()) {
                        m_local_edit_targets.push_back(q);
                    }
                }
            }
            CORE_INFO("Terrain edit groups published: columns={} surface_revision={}",
                m_patch_columns.size(), actual.authoritativeSurfaceRevision());
            // The merged immutable scene supersedes old frontier bases while
            // retaining current geometry for the next local promotion.
            m_presented_scene = std::move(presented);
            m_dirty = true;
            m_edit_checkpoint.published();
        } else {
            m_edit_checkpoint.discard();
        }
        m_patch.reset();
        m_patch_columns.clear();
    }
    if (!m_active || !m_world || m_presented_join_epoch != join_epoch
        || m_presented_coverage_epoch != coverage_epoch) {
        return;
    }
    uint64_t const sparse_edit_revision = actual.sparseEditRevision();
    m_presented_edit_index.select(actual, m_edited_columns, diagnostics);
    if (!m_edit_checkpoint.needsCapture(sparse_edit_revision)) {
        return;
    }
    // Only edits enter this lane. Ordinary streamed surface replacement keeps
    // using the background scene path, regardless of its discovery backlog.
    auto const column_key = [](TerrainLodColumnInput const& column) {
        return (static_cast<uint64_t>(column.x) << 32U) | column.y;
    };
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> ready;
    ready.reserve(m_patch_columns.size() + m_edited_columns.size());
    std::unordered_set<uint64_t> ready_columns;
    ready_columns.reserve(m_patch_columns.size() + m_edited_columns.size());
    for (auto const& previous : m_patch_columns) {
        uint64_t const key = column_key(*previous);
        if (ready_columns.contains(key)) {
            continue;
        }
        bool const still_selected = std::ranges::any_of(m_edited_columns, [&](auto const& column) {
            return column_key(*column) == key;
        });
        if (still_selected && terrainLodColumnSourcesCurrent(*previous, actual)) {
            ready.push_back(previous);
            ready_columns.insert(key);
        }
    }
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> missing;
    if (m_edited_columns.size() > ready.size()) {
        missing.reserve(m_edited_columns.size() - ready.size());
    }
    for (auto const& column : m_edited_columns) {
        if (!ready_columns.contains(column_key(*column))) {
            missing.push_back(column);
        }
    }
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> newly_captured;
    bool const missing_complete = captureTerrainLodColumnInputs(
        missing, actual, m_columns_authority_batch, newly_captured, diagnostics);
    ready.insert(ready.end(), newly_captured.begin(), newly_captured.end());
    bool const complete = missing_complete && ready.size() == m_edited_columns.size()
        && std::ranges::all_of(ready, [&](auto const& column) {
            return terrainLodColumnSourcesCurrent(*column, actual);
        });
    m_patch_columns = std::move(ready);
    m_edit_checkpoint.captured(sparse_edit_revision, complete);
    if (!m_edit_checkpoint.canPublish()) {
        return;
    }
    if (m_patch_columns.empty()) {
        // No geometry needs publication; a complete scan is already satisfied.
        m_edit_checkpoint.published();
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
    auto* const diagnostics = m_last_update_diagnostics.enabled ? &m_last_update_diagnostics : nullptr;
    ScopedDiagnosticsTimer refresh_timer{
        diagnostics != nullptr ? &diagnostics->source_refresh_duration : nullptr};
    if (diagnostics != nullptr) {
        ++diagnostics->source_refresh_count;
    }
    m_source_index.select(actual, scan_residents, m_source_selection,
        diagnostics != nullptr ? &diagnostics->source_index : nullptr);
    std::vector<TerrainLodColumnCoordinate> stale;
    if (!m_source_selection.columns.empty()) {
        ScopedDiagnosticsTimer validation_timer{
            diagnostics != nullptr ? &diagnostics->source_validation_duration : nullptr};
        if (diagnostics != nullptr) {
            ++diagnostics->source_validation_count;
            diagnostics->validated_column_count += m_source_selection.columns.size();
        }
        stale = staleCapturedTerrainLodColumns(m_source_selection.columns, actual, m_columns_authority_batch);
        validation_timer.finish();
        if (diagnostics != nullptr) {
            diagnostics->stale_column_count += stale.size();
        }
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

TerrainLodRuntimeProgress TerrainLodRuntime::progress() const noexcept
{
    bool const outstanding = m_job.valid();
    auto const elapsed = outstanding
        ? std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_job_started)
        : std::chrono::duration_cast<std::chrono::milliseconds>(m_last_job_duration);
    return {
        .dispatched = m_dispatched,
        .completed = m_completed,
        .failed = m_failed,
        .captured = m_captured,
        .published = m_published,
        .source_rejected = m_source_rejected,
        .plan_rejected = m_plan_rejected,
        .source_changes = m_source_changes,
        .actual_world_changes = m_actual_world_changes,
        .job_wall_ms = static_cast<uint64_t>(elapsed.count()),
        .job_outstanding = outstanding,
        .job_ready = outstanding && m_job.wait_for(std::chrono::seconds{0}) == std::future_status::ready,
        .candidate_pending = m_pending != nullptr,
        .last_error = m_last_error,
        .last_error_text = m_last_error_text,
        .last_plan_failure = m_last_publication_issue.failure,
    };
}

void TerrainLodRuntime::reset(VulkanRenderer* const renderer)
{
    m_cancellation.request_stop();
    m_actual_world = nullptr;
    m_world.reset();
    m_travel.reset();
    m_local_cancellation.request_stop();
    m_far_ready.reset();
    m_local_units.clear();
    m_local_edit_targets.clear();
    m_pending_unit.reset();
    m_pending_base.reset();
    m_pending_far = false;
    m_pending_recenter = Recenter::None;
    m_pending_transient_targets.clear();
    m_recenter_needs_settle = false;
    m_recenter_base_columns.clear();
    m_pending.reset();
    m_presented_scene.reset();
    m_columns.clear();
    m_source_index.clear();
    m_source_selection.columns.clear();
    m_source_selection.discovery.clear();
    m_columns_authority_batch.reset();
    m_presented_columns.clear();
    m_edit_checkpoint.reset();
    m_presented_edit_index.reset();
    m_edited_columns.clear();
    m_patch.reset();
    m_patch_columns.clear();
    m_plan.reset();
    m_discovery_initialized = false;
    m_center_x = 0U;
    m_center_y = 0U;
    m_dirty = true;
    m_actual_reported = false;
    m_current_capture_trace_initialized = false;
    if (m_active && renderer != nullptr) {
        renderer->clearTerrainLodScene();
    }
    m_active = false;
}

void TerrainLodRuntime::traceCurrentCapture(
    TerrainLodColumnKey const key,
    bool const discovery,
    bool const accepted,
    int64_t const elapsed_us,
    uint64_t const join_epoch,
    uint64_t const coverage_epoch)
{
    if (!streamingTraceEnabled()) {
        return;
    }
    bool const changed = !m_current_capture_trace_initialized
        || key.x != m_current_capture_trace_key.x || key.y != m_current_capture_trace_key.y
        || discovery != m_current_capture_trace_discovery || accepted != m_current_capture_trace_accepted
        || join_epoch != m_current_capture_trace_join_epoch
        || coverage_epoch != m_current_capture_trace_coverage_epoch;
    if (changed) {
        CORE_INFO(
            "stream_trace event=current_capture ns={} trigger={} key_x={} key_y={} "
            "accepted={} elapsed_us={} join={} coverage={}",
            streamingTraceTime(), discovery ? "discovery" : "active_current", key.x, key.y, accepted,
            elapsed_us, join_epoch, coverage_epoch);
        m_current_capture_trace_initialized = true;
        m_current_capture_trace_discovery = discovery;
        m_current_capture_trace_accepted = accepted;
        m_current_capture_trace_join_epoch = join_epoch;
        m_current_capture_trace_coverage_epoch = coverage_epoch;
        m_current_capture_trace_key = key;
    }
}

void TerrainLodRuntime::update(
    std::shared_ptr<shared::TerrainWorld const> source,
    shared::SparseWorld const* const actual,
    std::optional<shared::Player> const& player,
    VulkanRenderer& renderer,
    std::chrono::steady_clock::time_point const deadline,
    std::optional<shared::TerrainSessionIdentity> session,
    std::optional<uint64_t> const coverage_epoch,
    bool const collect_diagnostics,
    std::shared_ptr<shared::TerrainLodArtifact const> prepared_root
)
{
    if (collect_diagnostics) {
        m_last_update_diagnostics = {};
        m_last_update_diagnostics.enabled = true;
    } else {
        m_last_update_diagnostics.enabled = false;
    }
    auto* const diagnostics = collect_diagnostics ? &m_last_update_diagnostics : nullptr;
    {
        ScopedDiagnosticsTimer report_timer{
            diagnostics != nullptr ? &diagnostics->report_duration : nullptr};
        reportDiagnostics();
    }
    ScopedDiagnosticsTimer world_reset_timer{
        diagnostics != nullptr ? &diagnostics->world_reset_duration : nullptr};
    if (actual != m_actual_world) {
        ++m_actual_world_changes;
        reset(&renderer);
        m_actual_world = actual;
    }
    if (source != m_world) {
        ++m_source_changes;
        m_travel.reset();
        m_cancellation.request_stop();
        m_local_cancellation.request_stop();
        m_far_ready.reset();
        m_local_units.clear();
        m_local_edit_targets.clear();
        m_pending_unit.reset();
        m_pending_base.reset();
        m_pending_far = false;
        m_pending_recenter = Recenter::None;
        m_pending_transient_targets.clear();
        m_recenter_needs_settle = false;
        m_recenter_base_columns.clear();
        m_pending.reset();
        m_presented_scene.reset();
        m_presented_columns.clear();
        m_patch.reset();
        m_patch_columns.clear();
        m_edit_checkpoint.reset();
        m_presented_edit_index.reset();
        m_edited_columns.clear();
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
    world_reset_timer.finish();
    ScopedDiagnosticsTimer interest_prune_timer{
        diagnostics != nullptr ? &diagnostics->interest_prune_duration : nullptr};
    if (player) {
        uint32_t const x = shared::WorldBounds::wrapHorizontal(static_cast<int64_t>(shared::playerPositionX(*player)));
        uint32_t const y = shared::WorldBounds::wrapHorizontal(static_cast<int64_t>(shared::playerPositionY(*player)));
        if (m_plan.updateInterest(x, y)) {
            std::erase_if(m_columns, [this](auto const& column) {
                TerrainLodColumnInput const& input = column->input();
                bool const remove = !m_plan.relevant({input.x, input.y});
                if (remove) {
                    m_source_index.remove({input.x, input.y});
                }
                return remove;
            });
        }
        if (x / 64U != m_center_x / 64U || y / 64U != m_center_y / 64U) {
            m_center_x = x / 64U * 64U;
            m_center_y = y / 64U * 64U;
            m_discovery_initialized = false;
            m_dirty = true;
        }
    }
    interest_prune_timer.finish();
    ScopedDiagnosticsTimer worker_handoff_timer{
        diagnostics != nullptr ? &diagnostics->worker_handoff_duration : nullptr};
    if (m_job.valid() && m_job.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
        m_last_job_duration = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - m_job_started
        );
        try {
            ScopedDiagnosticsTimer future_get_timer{
                diagnostics != nullptr ? &diagnostics->worker_future_get_duration : nullptr};
            auto result = m_job.get();
            auto candidate = std::move(result.scene);
            future_get_timer.finish();
            ++m_completed;
            ++m_diagnostic_events;
            if (diagnostics != nullptr) {
                ++diagnostics->completed_job_count;
            }
            ScopedDiagnosticsTimer candidate_adopt_timer{
                diagnostics != nullptr ? &diagnostics->worker_candidate_adopt_duration : nullptr};
            bool const candidate_current = source && m_job_world == source && candidate->identity == source->identity();
            if (candidate_current) {
                if (streamingTraceEnabled()) {
                    CORE_INFO("stream_trace event=candidate ns={} scene={} center_x={} center_y={} columns={} join={} coverage={}",
                        streamingTraceTime(), reinterpret_cast<uintptr_t>(candidate.get()), candidate->center_x,
                        candidate->center_y, candidate->actual_column_count, candidate->session_join_epoch,
                        candidate->coverage_epoch);
                    auto const key = m_plan.currentColumn();
                    bool const included = std::ranges::any_of(candidate->leaves, [key](auto const& leaf) {
                        return leaf.level == shared::TerrainLodLevel::L3 && leaf.x == key.x && leaf.y == key.y;
                    });
                    CORE_INFO("stream_trace event=candidate_center ns={} scene={} key_x={} key_y={} included={}",
                        streamingTraceTime(), reinterpret_cast<uintptr_t>(candidate.get()), key.x, key.y, included);
                }
                if (m_active) {
                    m_far_ready = JobResult{std::move(candidate),std::move(result.columns)};
                    m_far_ticket = m_job_ticket;
                } else {
                    m_pending = std::move(candidate);
                    m_pending_ticket = m_job_ticket;
                    m_pending_columns = std::move(result.columns);
                    m_pending_unit.reset();
                    m_pending_base.reset();
                    m_pending_far = true;
                }
            } else {
                m_retired_job_columns = std::move(result.columns);
            }
            candidate_adopt_timer.finish();
        } catch (std::bad_alloc const&) {
            ++m_failed;
            m_last_error = TerrainLodRuntimeFailure::Allocation;
            m_last_error_text = {};
            if (!m_cancellation.stop_requested()) {
                m_dirty = true;
            }
        } catch (std::exception const& error) {
            ++m_failed;
            m_last_error = TerrainLodRuntimeFailure::Preparation;
            m_last_error_text = {};
            std::string_view const message{error.what()};
            std::copy_n(message.begin(), std::min(message.size(), m_last_error_text.size() - 1U),
                m_last_error_text.begin());
            if (!m_cancellation.stop_requested()) {
                m_dirty = true;
                CORE_ERROR("Terrain LOD scene preparation failed: {}", error.what());
            }
        }
    }
    worker_handoff_timer.finish();
    uint64_t const join_epoch = session && source && session->terrain == source->identity() ? session->join_epoch : 0U;
    if (!player || !actual || !m_presented_scene || m_presented_join_epoch != join_epoch
        || m_presented_coverage_epoch != coverage_epoch.value_or(0U)) {
        m_travel.reset();
    } else {
        auto const current = m_plan.currentColumn();
        if (!m_travel || m_travel->player != player->id || m_travel->current != current) {
            std::optional<TerrainLodColumnKey> forward;
            if (m_travel && m_travel->player == player->id && terrainLodVisualDistanceSquared(
                    current.x, current.y, m_travel->current.x, m_travel->current.y) == 1U) {
                constexpr uint32_t WORLD_COLUMNS = shared::WorldExtent::WIDTH / 16U;
                forward = TerrainLodColumnKey{
                    (2U * current.x + WORLD_COLUMNS - m_travel->current.x) % WORLD_COLUMNS,
                    (2U * current.y + WORLD_COLUMNS - m_travel->current.y) % WORLD_COLUMNS,
                };
            }
            m_travel = ColumnTravel{player->id, current, forward, ++m_travel_generation};
        }
    }
    if (streamingTraceEnabled() && actual && player) {
        auto const key = m_plan.currentColumn();
        auto const column = actual->authoritativeColumn(key.x * 16U, key.y * 16U);
        struct TraceObservation final {
            shared::SparseWorld const* world = nullptr;
            uint64_t join = 0U;
            uint32_t x = 0U;
            uint32_t y = 0U;
            bool available = false;
            uint64_t revision = 0U;
        };
        static thread_local TraceObservation previous;
        bool const available = column.has_value();
        uint64_t const revision = column ? column->revision : 0U;
        if (previous.world != actual || previous.join != join_epoch || previous.x != key.x
            || previous.y != key.y || previous.available != available || previous.revision != revision) {
            CORE_INFO("stream_trace event=center_authority ns={} key_x={} key_y={} available={} revision={} join={} coverage={}",
                streamingTraceTime(), key.x, key.y, available, revision, join_epoch, coverage_epoch.value_or(0U));
            previous = {actual, join_epoch, key.x, key.y, available, revision};
        }
    }
    if (actual && player && m_active) {
        TerrainLodColumnKey const current = m_plan.currentColumn();
        auto const existing = std::ranges::find_if(m_columns, [&](auto const& column) {
            return column->input().x == current.x && column->input().y == current.y;
        });
        if (existing == m_columns.end()
            || !capturedTerrainLodColumnCurrent(*existing, *actual, m_columns_authority_batch)) {
            ScopedDiagnosticsTimer capture_timer{
                diagnostics != nullptr ? &diagnostics->discovery_capture_duration : nullptr};
            if (diagnostics != nullptr) {
                ++diagnostics->discovery_capture_attempt_count;
            }
            auto const capture_started = std::chrono::steady_clock::now();
            auto captured = captureTerrainLodColumnForBatch(*actual, current.x, current.y, m_columns_authority_batch);
            auto const elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - capture_started).count();
            traceCurrentCapture(current, false, static_cast<bool>(captured), elapsed_us, join_epoch,
                coverage_epoch.value_or(0U));
            if (captured) {
                ++m_captured;
                if (diagnostics != nullptr) {
                    ++diagnostics->discovery_capture_success_count;
                }
                m_plan.remember(current);
                m_source_index.upsert(captured);
                if (existing == m_columns.end()) {
                    m_columns.push_back(std::move(captured));
                } else {
                    *existing = std::move(captured);
                }
                m_dirty = true;
            }
        }
    }
    if (m_recenter_needs_settle && m_presented_scene && actual && player
        && m_local_job.valid() && m_local_job_recenter == Recenter::Settle) {
        auto const current = m_plan.currentColumn();
        bool const presented = std::ranges::any_of(m_presented_scene->leaves, [current](auto const leaf) {
            return leaf.level == shared::TerrainLodLevel::L3 && leaf.x == current.x && leaf.y == current.y;
        });
        bool const ready = !presented && std::ranges::any_of(m_columns, [&](auto const& column) {
            return column->input().x == current.x && column->input().y == current.y
                && capturedTerrainLodColumnCurrent(column, *actual, m_columns_authority_batch);
        });
        if (ready) {
            // Retain the future until cooperative completion; never block the render thread on replacement.
            m_local_cancellation.request_stop();
        }
    }
    ScopedDiagnosticsTimer edit_lane_timer{
        diagnostics != nullptr ? &diagnostics->edit_lane_duration : nullptr};
    if (actual && player) {
        if (diagnostics != nullptr) {
            ++diagnostics->edit_lane_call_count;
        }
        updateEditedColumns(*actual, renderer, deadline, join_epoch, coverage_epoch.value_or(0U), diagnostics);
    }
    edit_lane_timer.finish();
    if (source && actual && m_active && m_presented_scene && m_recenter_needs_settle
        && m_presented_join_epoch == join_epoch && m_presented_coverage_epoch == coverage_epoch.value_or(0U)
        && m_travel && m_travel->forward && !m_travel->ahead_dispatched
        && !m_pending && !m_patch_job.valid() && !m_patch
        && m_local_job.valid() && m_local_job_recenter == Recenter::Settle && !m_local_cancellation.stop_requested()
        && m_local_job.wait_for(std::chrono::seconds{0}) == std::future_status::timeout) {
        auto const current = m_plan.currentColumn();
        bool const current_presented = std::ranges::any_of(m_presented_scene->leaves, [current](auto const leaf) {
            return leaf.level == shared::TerrainLodLevel::L3 && leaf.x == current.x && leaf.y == current.y;
        });
        if (current_presented && validatedSettleForwardInput(*actual, "cancel_settle")) {
            // Drain the existing future before dispatching; the next admission revalidates the live capture.
            m_local_cancellation.request_stop();
        }
    }
    if (!m_pending && !m_patch_job.valid() && !m_patch && m_local_job.valid()
        && m_local_job.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
        try {
            m_local_job_recenter = Recenter::None;
            auto result = m_local_job.get();
            auto& prepared = result.preparation;
            auto const current_key = m_plan.currentColumn();
            bool const current_target = std::ranges::find(
                result.targets, TerrainLodColumnCoordinate{current_key.x,current_key.y}) != result.targets.end();
            bool const prepared_has_current = prepared.scene && std::ranges::any_of(
                prepared.scene->leaves, [current_key](auto const leaf) {
                    return leaf.level == shared::TerrainLodLevel::L3
                        && leaf.x == current_key.x && leaf.y == current_key.y;
                });
            if (streamingTraceEnabled()) {
                CORE_INFO(
                    "stream_trace event=local_complete ns={} job={} far={} scene={} key_x={} key_y={} "
                    "targets={} current_target={} prepared={} prepared_has_current={}",
                    streamingTraceTime(), result.trace_id, result.far,
                    reinterpret_cast<uintptr_t>(prepared.scene.get()), current_key.x, current_key.y,
                    result.targets.size(), current_target,
                    static_cast<bool>(prepared.scene), prepared_has_current);
            }
            if (!m_local_cancellation.stop_requested()
                && prepared.scene && source && prepared.scene->identity == source->identity()
                && prepared.scene->shading_atlas == source->atlas()
                && prepared.scene->session_join_epoch == join_epoch
                && prepared.scene->coverage_epoch == coverage_epoch.value_or(0U)
                && (result.recenter != Recenter::CurrentFirst || (current_target && prepared_has_current))
                && (result.recenter != Recenter::Ahead || (prepared_has_current && m_travel
                    && result.travel_generation == m_travel->generation))) {
                if (prepared.unit && !result.far && result.base != m_presented_scene && m_presented_scene) {
                    std::vector<std::shared_ptr<TerrainLodColumnInput const>> merged;
                    auto const unit_targets = terrainLodLocalTargets(*prepared.unit);
                    std::vector<TerrainLodColumnCoordinate> targets(unit_targets.begin(),unit_targets.end());
                    for (auto q : m_local_edit_targets) {
                        if (std::ranges::find(targets,q) == targets.end()) {
                            targets.push_back(q);
                        }
                    }
                    // Newer accepted edits win over the old job's capture at an existing key.
                    auto live = result.columns;
                    for (auto const& input : m_presented_columns) {
                        auto old = std::ranges::find_if(live,[&](auto const& v) { return v->x == input->x && v->y == input->y; });
                        if (old != live.end()) {
                            *old = input;
                        }
                    }
                    if (mergeTerrainLodLocalColumns(m_presented_columns,live,targets,merged)) {
                        startLocalJob(source,m_presented_scene,m_presented_columns,std::move(merged),
                            {targets.begin(),targets.end()},{prepared.unit},result.ticket,false);
                    } else {
                        m_dirty = true;
                    }
                } else {
                    m_pending = std::move(prepared.scene);
                    m_pending_unit = result.far ? nullptr : std::move(prepared.unit);
                    m_pending_base = result.base;
                    m_pending_far = result.far;
                    m_pending_recenter = result.recenter;
                    m_pending_travel_generation = result.travel_generation;
                    if (isTransientRecenter(result.recenter)) {
                        m_pending_transient_targets = std::move(result.targets);
                    }
                    m_pending_columns = std::move(result.columns);
                    m_pending_ticket = std::move(result.ticket);
                }
            } else {
                m_dirty = true;
            }
        } catch (std::bad_alloc const&) {
            if (streamingTraceEnabled()) {
                CORE_INFO("stream_trace event=local_complete ns={} job={} outcome=allocation_failure",
                    streamingTraceTime(), m_local_job_trace_id);
            }
            ++m_failed;
            m_last_error = TerrainLodRuntimeFailure::Allocation;
            m_dirty = true;
        } catch (std::exception const& error) {
            bool const cancelled = m_local_cancellation.stop_requested();
            if (streamingTraceEnabled()) {
                CORE_INFO("stream_trace event=local_complete ns={} job={} outcome={} error={}",
                    streamingTraceTime(), m_local_job_trace_id,
                    cancelled ? "cancelled" : "preparation_failure", error.what());
            }
            if (!cancelled) {
                CORE_ERROR("Terrain LOD local preparation failed: {}",error.what());
                ++m_failed;
                m_last_error = TerrainLodRuntimeFailure::Preparation;
            }
            m_dirty = true;
        }
    }
    // Edits may publish while a near upload is pending. Rebase rather than overwrite that newer base.
    if (m_pending && m_pending_unit && m_pending_base.lock() != m_presented_scene && m_presented_scene
        && !m_local_job.valid() && !m_patch_job.valid() && !m_patch) {
        std::vector<std::shared_ptr<TerrainLodColumnInput const>> merged;
        auto const unit_targets = terrainLodLocalTargets(*m_pending_unit);
        std::vector<TerrainLodColumnCoordinate> targets(unit_targets.begin(),unit_targets.end());
        for (auto q : m_local_edit_targets) {
            if (std::ranges::find(targets,q) == targets.end()) {
                targets.push_back(q);
            }
        }
        auto live = m_pending_columns;
        for (auto const& input : m_presented_columns) {
            auto old = std::ranges::find_if(live,[&](auto const& v) { return v->x == input->x && v->y == input->y; });
            if (old != live.end()) {
                *old = input;
            }
        }
        if (mergeTerrainLodLocalColumns(m_presented_columns,live,targets,merged)) {
            startLocalJob(source,m_presented_scene,m_presented_columns,std::move(merged),
                {targets.begin(),targets.end()},{m_pending_unit},m_pending_ticket,false);
        } else {
            m_dirty = true;
        }
        m_pending.reset();
        m_pending_unit.reset();
        m_pending_columns.clear();
        m_pending_base.reset();
    }
    std::vector<TerrainLodColumnKey> rendered;
    if (m_pending && player) {
        for (auto const leaf : m_pending->leaves) {
            if (leaf.level == shared::TerrainLodLevel::L3) {
                rendered.push_back({leaf.x, leaf.y});
            }
        }
        if (m_pending_recenter != Recenter::None) {
            auto const current = m_plan.currentColumn();
            TerrainLodColumnKey const admitted_center{
                m_pending_ticket.center_x / 16U, m_pending_ticket.center_y / 16U};
            bool current_matches = current == admitted_center;
            if (isTransientRecenter(m_pending_recenter)) {
                bool const admitted_target = std::ranges::find(
                    m_pending_transient_targets, TerrainLodColumnCoordinate{current.x, current.y})
                    != m_pending_transient_targets.end();
                // Ahead can publish while the player is still on its retained center column.
                current_matches = std::ranges::find(rendered, current) != rendered.end()
                    && (admitted_target || (m_pending_recenter == Recenter::Ahead && current_matches));
            }
            bool const ahead_invalidated = m_pending_recenter == Recenter::Ahead
                && (!m_travel || m_pending_travel_generation != m_travel->generation
                    || !m_local_edit_targets.empty() || !m_edited_columns.empty());
            if (m_pending_base.lock() != m_presented_scene || !current_matches || ahead_invalidated) {
                m_pending.reset();
                m_pending_base.reset();
                m_pending_recenter = Recenter::None;
                m_pending_transient_targets.clear();
                m_retired_job_columns = std::move(m_pending_columns);
                m_plan.requestSelectionDemand();
                m_dirty = true;
            }
        }
    }
    if (m_pending && player) {
        ScopedDiagnosticsTimer pending_validation_timer{
            diagnostics != nullptr ? &diagnostics->pending_validation_duration : nullptr};
        bool const sources_current = actual && (m_pending_unit
                ? terrainLodLocalSourcesCurrent(*m_pending_unit, *actual) : terrainLodSourcesCurrent(*m_pending, *actual))
            && m_pending->session_join_epoch == join_epoch
            && m_pending->coverage_epoch == coverage_epoch.value_or(0U);
        auto const issue = m_plan.publicationIssue(m_pending_ticket, rendered);
        if (diagnostics != nullptr) {
            ++diagnostics->pending_validation_count;
            diagnostics->pending_actual_column_count += m_pending->actual_column_count;
            diagnostics->pending_chunk_source_count += m_pending->chunk_sources.size();
            diagnostics->pending_authority_source_count += m_pending->authority_sources.size();
            diagnostics->source_rejection_count += sources_current ? 0U : 1U;
            diagnostics->plan_rejection_count += issue.failure == TerrainLodPublicationFailure::None ? 0U : 1U;
        }
        pending_validation_timer.finish();
        if (!sources_current || issue.failure != TerrainLodPublicationFailure::None) {
            m_source_rejected += sources_current ? 0U : 1U;
            m_plan_rejected += issue.failure == TerrainLodPublicationFailure::None ? 0U : 1U;
            if (issue.failure != TerrainLodPublicationFailure::None) {
                ++m_plan_failure_counts[static_cast<uint32_t>(issue.failure)];
            }
            m_last_publication_issue = issue;
            ++m_diagnostic_events;
            bool rebasing = false;
            bool const missing = issue.failure == TerrainLodPublicationFailure::MissingCenter
                || issue.failure == TerrainLodPublicationFailure::MissingPresented;
            if (sources_current && missing && m_pending_recenter == Recenter::None && !m_local_job.valid()) {
                if (!m_pending_unit && !m_far_ready) {
                    // Keep completed far work when travel advances during upload. The same guards
                    // must pass after a closed local repair; do not restart the global producer.
                    m_far_ready = JobResult{std::move(m_pending),std::move(m_pending_columns)};
                    m_far_ticket = m_pending_ticket;
                    rebasing = true;
                } else if (m_pending_unit && m_presented_scene) {
                    std::vector<std::shared_ptr<TerrainLodColumnInput const>> ready, merged;
                    std::vector<TerrainLodColumnCoordinate> targets, additional;
                    auto const old_targets = terrainLodLocalTargets(*m_pending_unit);
                    targets.assign(old_targets.begin(),old_targets.end());
                    selectTerrainLodLocalColumns(m_plan,m_columns,m_pending_columns,ready,additional,m_local_edit_targets);
                    for (auto q : additional) {
                        if (std::ranges::find(targets,q) == targets.end()) {
                            targets.push_back(q);
                        }
                    }
                    if (mergeTerrainLodLocalColumns(m_presented_columns,ready,targets,merged)) {
                        startLocalJob(source,m_presented_scene,m_presented_columns,std::move(merged),
                            std::move(targets),{m_pending_unit},m_pending_ticket,false);
                        rebasing = true;
                    }
                }
            }
            m_pending.reset();
            m_pending_unit.reset(); m_pending_base.reset();
            if (m_pending_recenter != Recenter::None) {
                m_plan.requestSelectionDemand();
                m_pending_recenter = Recenter::None;
                m_pending_transient_targets.clear();
            }
            m_retired_job_columns = std::move(m_pending_columns);
            if (!rebasing) {
                m_discovery_initialized = false;
                m_dirty = true;
            }
        } else if (!m_patch_job.valid() && !m_patch) {
            ScopedDiagnosticsTimer renderer_upsert_timer{
                diagnostics != nullptr ? &diagnostics->renderer_upsert_duration : nullptr};
            if (diagnostics != nullptr) {
                ++diagnostics->scene_handoff_attempt_count;
            }
            TerrainLodRendererDiagnostics renderer_diagnostics{};
            bool const upserted = renderer.upsertTerrainLodScene(
                m_pending,
                deadline,
                diagnostics != nullptr ? &renderer_diagnostics : nullptr
            );
            if (diagnostics != nullptr) {
                diagnostics->renderer_plan_duration += renderer_diagnostics.plan_duration;
                diagnostics->renderer_buffer_update_duration += renderer_diagnostics.buffer_update_duration;
                diagnostics->renderer_publish_duration += renderer_diagnostics.publish_duration;
                diagnostics->renderer_uploaded_face_count += renderer_diagnostics.uploaded_face_count;
                diagnostics->prepared_draw_index_publish_count +=
                    renderer_diagnostics.prepared_draw_index_publish_count;
                diagnostics->render_thread_draw_index_build_count +=
                    renderer_diagnostics.render_thread_draw_index_build_count;
            }
            renderer_upsert_timer.finish();
            if (upserted) {
                ScopedDiagnosticsTimer scene_handoff_timer{
                    diagnostics != nullptr ? &diagnostics->scene_handoff_duration : nullptr};
                ScopedDiagnosticsTimer activation_log_timer{
                    diagnostics != nullptr ? &diagnostics->scene_activation_log_duration : nullptr};
                if (!m_active || (!m_actual_reported && m_pending->actual_column_count != 0U)) {
                    CORE_INFO(
                        "Terrain LOD scene active: model={} leaves={} faces={} "
                        "actual_columns={}",
                        shared::terrainLodModel(
                            m_pending->identity.terrain_version
                            == generation::terrainVersion(generation::TerrainModelTag::V9)
                            ? generation::TerrainModelTag::V9
                            : (m_pending->identity.terrain_version
                            == generation::terrainVersion(generation::TerrainModelTag::V21)
                            ? generation::TerrainModelTag::V21
                            : (m_pending->identity.terrain_version
                            == generation::terrainVersion(generation::TerrainModelTag::V10)
                            ? generation::TerrainModelTag::V10
                            : generation::TerrainModelTag::R6))
                        ),
                        m_pending->leaves.size(),
                        terrainLodSceneFaceCount(*m_pending),
                        m_pending->actual_column_count
                    );
                }
                activation_log_timer.finish();
                m_actual_reported = m_actual_reported || m_pending->actual_column_count != 0U;
                ++m_published;
                ++m_diagnostic_events;
                ScopedDiagnosticsTimer plan_presented_timer{
                    diagnostics != nullptr ? &diagnostics->scene_plan_presented_duration : nullptr};
                m_plan.presented(rendered);
                plan_presented_timer.finish();
                ScopedDiagnosticsTimer column_selection_timer{
                    diagnostics != nullptr ? &diagnostics->scene_column_selection_duration : nullptr};
                if (isTransientRecenter(m_pending_recenter) && !m_recenter_needs_settle) {
                    for (auto const& column : m_presented_columns) {
                        m_recenter_base_columns.insert((static_cast<uint64_t>(column->x) << 32U) | column->y);
                    }
                }
                selectPresentedTerrainLodColumns(
                    m_pending_columns, rendered, m_presented_columns, m_retired_presented_columns
                );
                m_retired_job_columns = std::move(m_pending_columns);
                if (m_presented_join_epoch != m_pending->session_join_epoch
                    || m_presented_coverage_epoch != m_pending->coverage_epoch) {
                    m_presented_edit_index.reset();
                    m_recenter_needs_settle = false;
                    m_recenter_base_columns.clear();
                }
                m_presented_edit_index.replace(m_presented_columns, *actual);
                column_selection_timer.finish();
                m_presented_join_epoch = m_pending->session_join_epoch;
                m_presented_coverage_epoch = m_pending->coverage_epoch;
                ScopedDiagnosticsTimer pending_release_timer{
                    diagnostics != nullptr ? &diagnostics->scene_pending_release_duration : nullptr};
                m_presented_scene = m_pending;
                if (m_pending_far || m_pending_recenter == Recenter::Settle) {
                    // The full disk rebuilt or retired the exact inputs and their old local units.
                    m_local_units.clear();
                    m_local_edit_targets.clear();
                } else if (m_pending_unit) {
                    auto const repaired = terrainLodLocalTargets(*m_pending_unit);
                    std::erase_if(m_local_edit_targets,[&](auto q) { return std::ranges::find(repaired,q) != repaired.end(); });
                }
                if (!m_pending_far && m_pending_unit && (m_job.valid() || m_far_ready)) {
                    m_local_units.push_back(m_pending_unit);
                }
                if (isTransientRecenter(m_pending_recenter)) {
                    // Only a published mixed mask needs a conventional disk to retire it.
                    m_recenter_needs_settle = true;
                } else if (m_pending_recenter == Recenter::Settle) {
                    m_recenter_needs_settle = false;
                    m_recenter_base_columns.clear();
                }
                m_pending_unit.reset(); m_pending_base.reset(); m_pending_far = false;
                m_pending_recenter = Recenter::None;
                m_pending_transient_targets.clear();
                m_pending.reset();
                pending_release_timer.finish();
                m_active = true;
                if (diagnostics != nullptr) {
                    ++diagnostics->scene_handoff_complete_count;
                }
                scene_handoff_timer.finish();
            }
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
    ScopedDiagnosticsTimer discovery_capture_timer{
        diagnostics != nullptr ? &diagnostics->discovery_capture_duration : nullptr};
    while (actual && captures < 8U && std::chrono::steady_clock::now() < deadline) {
        auto const next = m_plan.nextDiscovery();
        if (!next) {
            break;
        }
        TerrainLodColumnKey const key = *next;
        ++captures;
        if (diagnostics != nullptr) {
            ++diagnostics->discovery_capture_attempt_count;
        }
        uint32_t const x = key.x;
        uint32_t const y = key.y;
        auto const current_key = m_plan.currentColumn();
        bool const is_current = x == current_key.x && y == current_key.y;
        auto const capture_started = std::chrono::steady_clock::now();
        auto captured = captureTerrainLodColumnForBatch(*actual, x, y, m_columns_authority_batch);
        if (is_current) {
            auto const elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - capture_started).count();
            traceCurrentCapture(key, true, static_cast<bool>(captured), elapsed_us, join_epoch,
                coverage_epoch.value_or(0U));
        }
        auto const existing =
            std::ranges::find_if(m_columns, [x, y](std::shared_ptr<TerrainLodCapturedColumn const> const& column) {
                return column->input().x == x && column->input().y == y;
            });
        if (captured) {
            ++m_captured;
            if (diagnostics != nullptr) {
                ++diagnostics->discovery_capture_success_count;
            }
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
    discovery_capture_timer.finish();
    if (!source) {
        return;
    }
    ScopedDiagnosticsTimer dispatch_preparation_timer{
        diagnostics != nullptr ? &diagnostics->dispatch_preparation_duration : nullptr};
    bool const selection_demand = m_plan.selectionDemand();
    uint8_t blocked_reason = 0U;
    std::string_view blocked_name = "none";
    if (selection_demand) {
        if (!m_active) {
            blocked_reason = 1U;
            blocked_name = "inactive";
        } else if (!actual) {
            blocked_reason = 2U;
            blocked_name = "no_actual_world";
        } else if (!m_presented_scene) {
            blocked_reason = 3U;
            blocked_name = "no_presented_scene";
        } else if (m_pending) {
            blocked_reason = 4U;
            blocked_name = "pending_scene";
        } else if (m_local_job.valid()) {
            blocked_reason = 6U;
            blocked_name = "local_job";
        } else if (m_patch_job.valid()) {
            blocked_reason = 7U;
            blocked_name = "patch_job";
        } else if (m_patch) {
            blocked_reason = 8U;
            blocked_name = "patch";
        }
    }
    bool selection_admitted = false;
    auto log_selection_demand = [&](bool const demand, bool const admitted, uint8_t const reason,
        std::string_view const name) {
        if (!streamingTraceEnabled()) {
            return;
        }
        auto const key = m_plan.currentColumn();
        bool const changed = !m_selection_trace_initialized || demand != m_selection_trace_demand
            || admitted != m_selection_trace_admitted || reason != m_selection_trace_blocked_reason
            || key.x != m_selection_trace_key.x || key.y != m_selection_trace_key.y;
        if (changed && (demand || m_selection_trace_demand)) {
            CORE_INFO("stream_trace event=selection_demand ns={} key_x={} key_y={} demand={} admitted={} blocked={}",
                streamingTraceTime(), key.x, key.y, demand, admitted, name);
        }
        m_selection_trace_initialized = true;
        m_selection_trace_demand = demand;
        m_selection_trace_admitted = admitted;
        m_selection_trace_blocked_reason = reason;
        m_selection_trace_key = key;
    };
    auto acknowledge_unavailable_selection = [&] {
        if (selection_demand) {
            m_plan.acknowledgeSelectionDemand();
            log_selection_demand(false,false,0U,"no_work_available");
        }
    };
    if (m_active && actual && m_presented_scene
        && (selection_demand || !m_local_edit_targets.empty() || m_recenter_needs_settle)
        && m_presented_join_epoch == join_epoch && m_presented_coverage_epoch == coverage_epoch.value_or(0U)
        && !m_pending && !m_local_job.valid() && !m_patch_job.valid() && !m_patch) {
        std::vector<std::shared_ptr<TerrainLodColumnInput const>> local_columns;
        std::vector<TerrainLodColumnCoordinate> targets;
        auto const current = m_plan.currentColumn();
        TerrainLodColumnCoordinate const current_target{current.x, current.y};
        bool local_ready = selectTerrainLodLocalColumns(
            m_plan, m_columns, m_presented_columns, local_columns, targets, m_local_edit_targets);
        bool const outside_disk = m_presented_scene->visual_center
            && !terrainLodVisualColumn(current.x, current.y,
                m_presented_scene->visual_center->x, m_presented_scene->visual_center->y);
        bool const recenter = m_recenter_needs_settle
            || (outside_disk && std::ranges::find(targets, current_target) != targets.end());
        auto mode = recenter ? Recenter::Settle : Recenter::None;
        bool const current_presented = std::ranges::any_of(m_presented_scene->leaves, [current](auto const leaf) {
            return leaf.level == shared::TerrainLodLevel::L3 && leaf.x == current.x && leaf.y == current.y;
        });
        bool const escaped_settle = m_recenter_needs_settle && !current_presented;
        if (recenter && (!m_recenter_needs_settle || escaped_settle) && m_edited_columns.empty()
            && (m_local_edit_targets.empty() || escaped_settle)) {
            std::vector<std::shared_ptr<TerrainLodColumnInput const>> footprint;
            std::vector<TerrainLodColumnCoordinate> footprint_targets;
            selectTerrainLodLocalColumns(m_plan, m_columns, {}, footprint, footprint_targets);
            std::erase_if(footprint, [&](auto const& input) {
                return !terrainLodColumnSourcesCurrent(*input, *actual);
            });
            if (std::ranges::any_of(footprint, [&](auto const& input) {
                    return input->x == current.x && input->y == current.y;
                })) {
                // The current footprint wins at overlapping coordinates. No other discovery
                // capture can enter the mixed mask.
                bool const trace_retention = streamingTraceEnabled();
                CurrentFirstRetentionCounts retention_counts;
                std::unordered_set<uint64_t> prior_keys;
                std::unordered_set<uint64_t> urgent_keys;
                if (trace_retention) {
                    retention_counts.prior_presented = m_presented_columns.size();
                    prior_keys.reserve(m_presented_columns.size());
                    urgent_keys.reserve(footprint.size());
                    for (auto const& column : m_presented_columns) {
                        prior_keys.insert((static_cast<uint64_t>(column->x) << 32U) | column->y);
                    }
                }
                local_columns.clear();
                local_columns.reserve(m_presented_columns.size() + footprint.size());
                std::unordered_set<uint64_t> included;
                included.reserve(local_columns.capacity());
                auto add = [&](auto const& column) {
                    if (included.insert((static_cast<uint64_t>(column->x) << 32U) | column->y).second) {
                        local_columns.push_back(column);
                    }
                };
                for (auto const& column : footprint) {
                    if (trace_retention) {
                        uint64_t const key = (static_cast<uint64_t>(column->x) << 32U) | column->y;
                        if (prior_keys.contains(key)) {
                            ++retention_counts.urgent_overlap;
                        } else {
                            ++retention_counts.urgent_new;
                        }
                        urgent_keys.insert(key);
                    }
                    add(column);
                }
                for (auto const& column : m_presented_columns) {
                    if (trace_retention) {
                        uint64_t const key = (static_cast<uint64_t>(column->x) << 32U) | column->y;
                        if (urgent_keys.contains(key)) {
                            continue;
                        }
                    }
                    bool const edited = std::ranges::find(m_local_edit_targets,
                        TerrainLodColumnCoordinate{column->x, column->y}) != m_local_edit_targets.end();
                    bool const visually_relevant = m_plan.visuallyRelevant({column->x, column->y});
                    bool const retained = !m_recenter_needs_settle || edited || visually_relevant
                        || m_recenter_base_columns.contains(
                            (static_cast<uint64_t>(column->x) << 32U) | column->y);
                    // Obsolete offscreen captures must not invalidate the current footprint.
                    // Visible and edited inputs still participate in full source validation.
                    if (trace_retention) {
                        bool const sources_current = terrainLodColumnSourcesCurrent(*column, *actual);
                        if (sources_current && retained) {
                            ++retention_counts.old_retained_fresh;
                        } else if (!sources_current) {
                            auto const stale = terrainLodColumnStaleSources(*column, *actual);
                            recordStaleSources(retention_counts, stale, retained && (edited || visually_relevant));
                        }
                        if (!retained) {
                            ++retention_counts.removed_nonretained_recenterbase;
                        }
                        if (retained && (edited || visually_relevant || sources_current)) {
                            add(column);
                        }
                    } else if (retained
                        && (edited || visually_relevant || terrainLodColumnSourcesCurrent(*column, *actual))) {
                        add(column);
                    }
                }
                if (trace_retention) {
                    CORE_INFO(
                        "stream_trace event=current_first_retention ns={} prior_presented={} urgent_overlap={} "
                        "urgent_new={} old_retained_fresh={} retained_edited_visible_stale={} "
                        "removed_source_stale={} removed_nonretained_recenterbase={} stale_chunk_columns={} "
                        "stale_authority_columns={} stale_fallback_edit_columns={} "
                        "authority_unavailable_or_incomplete_columns={} authority_missing_tile_columns={} "
                        "authority_incomplete_edit_requirements_columns={} authority_revision_mismatch_columns={} "
                        "authority_dense_hash_mismatch_columns={} authority_other_columns={} selected_inputs={}",
                        streamingTraceTime(), retention_counts.prior_presented, retention_counts.urgent_overlap,
                        retention_counts.urgent_new, retention_counts.old_retained_fresh,
                        retention_counts.retained_edited_visible_stale, retention_counts.removed_source_stale,
                        retention_counts.removed_nonretained_recenterbase, retention_counts.stale_chunk_columns,
                        retention_counts.stale_authority_columns, retention_counts.stale_fallback_edit_columns,
                        retention_counts.authority_unavailable_or_incomplete_columns,
                        retention_counts.authority_missing_tile_columns,
                        retention_counts.authority_incomplete_edit_requirements_columns,
                        retention_counts.authority_revision_mismatch_columns,
                        retention_counts.authority_dense_hash_mismatch_columns, retention_counts.authority_other_columns,
                        local_columns.size());
                }
                targets.clear();
                for (auto const& column : footprint) {
                    targets.push_back({column->x, column->y});
                }
                mode = Recenter::CurrentFirst;
            }
        }
        if (mode == Recenter::Settle && current_presented) {
            if (auto forward = validatedSettleForwardInput(*actual, "dispatch_ahead")) {
                // Extend a mixed footprint by one ordinary forward input before paying its settle debt.
                local_columns.resize(m_presented_columns.size());
                targets.assign(1U, TerrainLodColumnCoordinate{forward->x, forward->y});
                local_columns.push_back(std::move(forward));
                local_ready = true;
                mode = Recenter::Ahead;
            }
        }
        if (mode == Recenter::None && local_ready && current_presented && !outside_disk
            && m_presented_scene->visual_center && m_travel && m_travel->forward && !m_travel->ahead_dispatched
            && m_edited_columns.empty() && m_local_edit_targets.empty()) {
            auto const visual = *m_presented_scene->visual_center;
            auto const forward = *m_travel->forward;
            auto const additions = std::span{local_columns}.subspan(m_presented_columns.size());
            auto const next = std::ranges::find_if(additions, [&](auto const& column) {
                return column->x == forward.x && column->y == forward.y;
            });
            if (next != additions.end() && !terrainLodVisualColumn(forward.x, forward.y, visual.x, visual.y)
                && !(*next)->authority_sources.empty() && terrainLodColumnSourcesCurrent(**next, *actual)) {
                // Keep only the ordinary selector's forward capture on the rendered base.
                auto captured = *next;
                local_columns.resize(m_presented_columns.size());
                local_columns.push_back(std::move(captured));
                targets.assign(1U, TerrainLodColumnCoordinate{forward.x, forward.y});
                mode = Recenter::Ahead;
            }
        }
        if (mode == Recenter::None && m_presented_scene->visual_center) {
            auto const visual = *m_presented_scene->visual_center;
            if (std::ranges::any_of(targets, [visual](auto const target) {
                    return !terrainLodVisualColumn(target.x, target.y, visual.x, visual.y);
                })) {
                // Ordinary local preparation keeps the existing disk. Reselect inside it
                // so an unusable frontier capture cannot occupy the local job slot.
                local_ready = selectTerrainLodLocalColumns(
                    m_plan, m_columns, m_presented_columns, local_columns, targets, m_local_edit_targets, visual);
            }
        }
        std::vector<std::shared_ptr<TerrainLodColumnInput const>> base_columns;
        if (local_ready || recenter) {
            base_columns = m_presented_columns;
        }
        if (mode == Recenter::Settle) {
            if (m_recenter_needs_settle) {
                // Rediscovery can replace or retire stale mixed-mask captures before the full disk.
                std::vector<std::shared_ptr<TerrainLodColumnInput const>> fresh;
                selectTerrainLodSceneColumns(m_plan, m_columns, m_presented_columns, true, fresh);
                for (auto const& input : m_presented_columns) {
                    TerrainLodColumnCoordinate const key{input->x, input->y};
                    if (std::ranges::find(m_local_edit_targets, key) == m_local_edit_targets.end()) {
                        continue;
                    }
                    auto const old = std::ranges::find_if(fresh, [&](auto const& column) {
                        return column->x == key.x && column->y == key.y;
                    });
                    if (old == fresh.end()) {
                        fresh.push_back(input);
                    } else {
                        *old = input;
                    }
                }
                selectTerrainLodLocalColumns(m_plan, m_columns, fresh, local_columns, targets, m_local_edit_targets);
            }
            std::erase_if(base_columns, [&](auto const& input) {
                return std::ranges::find(m_local_edit_targets, TerrainLodColumnCoordinate{input->x, input->y})
                    != m_local_edit_targets.end();
            });
            local_ready = true;
        }
        if (local_ready) {
            startLocalJob(source, m_presented_scene, std::move(base_columns), std::move(local_columns),
                std::move(targets), {}, m_plan.ticket(), false, mode);
            if (mode == Recenter::Ahead) {
                m_travel->ahead_dispatched = true;
            }
            if (selection_demand) {
                m_plan.acknowledgeSelectionDemand();
                selection_admitted = true;
            }
        } else if (selection_demand) {
            blocked_reason = 9U;
            blocked_name = "no_ready_local_targets";
        }
    }
    if (selection_demand && !selection_admitted && blocked_reason == 0U && m_job.valid()) {
        blocked_reason = 10U;
        blocked_name = "far_job";
    }
    log_selection_demand(selection_demand, selection_admitted, blocked_reason, blocked_name);
    prepareFarHandoff();
    if (m_job.valid() || m_pending || m_far_ready || (m_local_job.valid() && !m_dirty)) {
        return;
    }
    if (!m_dirty && !selection_demand) {
        return;
    }
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> columns;
    columns.reserve(m_columns.size());
    bool promote_columns = false;
    if (actual) {
        promote_columns = selectTerrainLodSceneColumns(m_plan, m_columns, m_presented_columns, m_active, columns);
        if (!m_dirty && !promote_columns) {
            acknowledge_unavailable_selection();
            return;
        }
        // Revalidate the selected capture set at the dispatch boundary.
        refreshColumnSources(*actual, true, false);
        promote_columns = selectTerrainLodSceneColumns(m_plan, m_columns, m_presented_columns, m_active, columns);
        auto const& configuration = source->atlas()->configuration;
        if (!m_active && join_epoch != 0U && coverage_epoch.value_or(0U) != 0U
            && configuration.model_tag == generation::TerrainModelTag::V10 && configuration.use_v10_fine_terrain) {
            // A first scene without the authoritative center can be rejected when its capture arrives.
            auto const center = m_plan.currentColumn();
            if (std::ranges::none_of(columns, [center](auto const& input) {
                return input->x == center.x && input->y == center.y && !input->authority_sources.empty();
            })) {
                return;
            }
        }
    }
    if (!m_dirty && !promote_columns) {
        acknowledge_unavailable_selection();
        return;
    }
    if (diagnostics != nullptr) {
        diagnostics->dispatch_column_count += columns.size();
    }
    auto const base = m_active && m_presented_join_epoch == join_epoch
            && m_presented_coverage_epoch == coverage_epoch.value_or(0U)
        ? m_presented_scene : nullptr;
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> unchanged;
    if (base) {
        std::unordered_set<TerrainLodColumnInput const*> selected;
        selected.reserve(columns.size());
        for (auto const& column : columns) {
            selected.insert(column.get());
        }
        for (auto const& column : m_presented_columns) {
            if (selected.contains(column.get()) && std::ranges::find(m_local_edit_targets,
                    TerrainLodColumnCoordinate{column->x,column->y}) == m_local_edit_targets.end()) {
                unchanged.push_back(column);
            }
        }
    }
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> retired_job_columns =
        std::move(m_retired_job_columns);
    m_cancellation = std::stop_source{};
    auto const cancellation = m_cancellation.get_token();
    m_local_units.clear();
    m_job_world = source;
    m_job_ticket = m_plan.ticket();
    m_job_ticket.columns.clear();
    for (auto const& column : columns) {
        m_job_ticket.columns.push_back({column->x, column->y});
    }
    uint32_t const center_x = m_job_ticket.center_x;
    uint32_t const center_y = m_job_ticket.center_y;
    m_dirty = false;
    ++m_dispatched;
    ++m_diagnostic_events;
    if (diagnostics != nullptr) {
        ++diagnostics->dispatch_count;
    }
    if (streamingTraceEnabled()) {
        CORE_INFO("stream_trace event=dispatch ns={} center_x={} center_y={} columns={} join={} coverage={}",
            streamingTraceTime(), center_x, center_y, columns.size(), join_epoch, coverage_epoch.value_or(0U));
    }
    m_job_started = std::chrono::steady_clock::now();
    // NOLINTNEXTLINE(bugprone-exception-escape)
    m_job = std::async(std::launch::async,
        [source = std::move(source), base, center_x, center_y, promote_columns,
            prepared_root = base ? nullptr : std::move(prepared_root),
            session = std::move(session), coverage_epoch,
            columns = std::move(columns), unchanged = std::move(unchanged),
            retired_job_columns = std::move(retired_job_columns),
            retired_presented_columns = std::move(m_retired_presented_columns), cancellation]() mutable {
            std::shared_ptr<TerrainLodScene> scene;
            try {
                scene = std::const_pointer_cast<TerrainLodScene>(
                    buildTerrainLodSceneFromColumns(*source, center_x, center_y, columns, cancellation,
                        base.get(), unchanged, promote_columns,
                        TerrainLodColumnCoordinate{center_x / 16U, center_y / 16U}, prepared_root)
                );
            } catch (...) {
                retired_job_columns.clear();
                retired_presented_columns.clear();
                throw;
            }
            scene->session_join_epoch = session && session->terrain == scene->identity ? session->join_epoch : 0U;
            scene->coverage_epoch = coverage_epoch.value_or(0U);
            scene->center_x = center_x;
            scene->center_y = center_y;
            retired_job_columns.clear();
            retired_presented_columns.clear();
            return JobResult{std::shared_ptr<TerrainLodScene const>{std::move(scene)}, std::move(columns)};
        });
    if (selection_demand && !selection_admitted) {
        m_plan.acknowledgeSelectionDemand();
        log_selection_demand(true,true,0U,"full_job_admitted");
    }
}

} // namespace client
