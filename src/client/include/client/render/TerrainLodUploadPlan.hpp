#pragma once

#include <client/TerrainLodPresentation.hpp>
#include <client/render/TerrainLodDrawIndex.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace client::detail {

inline void validateTerrainLodGroups(TerrainLodScene const& scene)
{
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, bool> keys;
    uint64_t face_count = 0U;
    size_t const logical_face_count = terrainLodSceneFaceCount(scene);
    std::vector<std::pair<uint32_t, uint32_t>> intervals;
    intervals.reserve(scene.groups.size());
    for (auto const& group : scene.groups) {
        if (group.range.first_instance > logical_face_count
            || group.range.instance_count > logical_face_count - group.range.first_instance) {
            throw std::invalid_argument("terrain LOD group face range is invalid");
        }
        static_cast<void>(terrainLodGroupFaces(group));
        if (!keys.emplace(std::tuple{ group.origin_x, group.origin_y, group.extent }, true).second) {
            throw std::invalid_argument("terrain LOD group key is duplicated");
        }
        face_count += group.range.instance_count;
        if (group.range.instance_count != 0U) {
            intervals.emplace_back(group.range.first_instance, group.range.instance_count);
        }
    }
    std::ranges::sort(intervals);
    uint64_t next_face = 0U;
    for (auto const& [first, count] : intervals) {
        if (first != next_face) {
            throw std::invalid_argument("terrain LOD group face ranges overlap or leave gaps");
        }
        next_face += count;
    }
    if (face_count != logical_face_count || logical_face_count > std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument("terrain LOD groups do not cover the scene face count");
    }
}

[[nodiscard]] inline std::span<StoneFaceInstance const> terrainLodGroupFaces(
    TerrainLodScene const& scene,
    TerrainLodDrawGroup const& group
)
{
    size_t const logical_face_count = terrainLodSceneFaceCount(scene);
    if (group.range.first_instance > logical_face_count
        || group.range.instance_count > logical_face_count - group.range.first_instance) {
        throw std::invalid_argument("terrain LOD group logical range is invalid");
    }
    return client::terrainLodGroupFaces(group);
}

[[nodiscard]] inline std::vector<std::optional<uint32_t>> terrainLodReusableGroups(
    TerrainLodScene const* active,
    TerrainLodScene const& candidate
)
{
    validateTerrainLodGroups(candidate);
    using Key = std::tuple<uint32_t, uint32_t, uint32_t>;
    std::vector<std::optional<uint32_t>> reusable(candidate.groups.size());
    if (active == nullptr) {
        return reusable;
    }
    std::map<Key, uint32_t> active_groups;
    validateTerrainLodGroups(*active);
    for (uint32_t index = 0U; index < active->groups.size(); ++index) {
        auto const& group = active->groups[index];
        active_groups.emplace(Key{ group.origin_x, group.origin_y, group.extent }, index);
    }
    for (uint32_t index = 0U; index < candidate.groups.size(); ++index) {
        auto const& group = candidate.groups[index];
        auto const previous = active_groups.find(Key{ group.origin_x, group.origin_y, group.extent });
        if (previous == active_groups.end()) {
            continue;
        }
        auto const old_faces = terrainLodGroupFaces(*active, active->groups[previous->second]);
        auto const new_faces = terrainLodGroupFaces(candidate, group);
        auto const& old = active->groups[previous->second];
        if ((old.face_storage == group.face_storage
                && old.face_storage_first_instance == group.face_storage_first_instance
                && old_faces.size() == new_faces.size())
            || std::ranges::equal(old_faces, new_faces)) {
            reusable[index] = previous->second;
        }
    }
    return reusable;
}

[[nodiscard]] inline bool terrainLodUploadsComplete(
    TerrainLodScene const& scene,
    std::span<uint32_t const> uploaded
)
{
    if (uploaded.size() != scene.groups.size()) {
        return false;
    }
    for (uint32_t index = 0U; index < uploaded.size(); ++index) {
        if (uploaded[index] != scene.groups[index].range.instance_count) {
            return false;
        }
    }
    return true;
}

// The cursor belongs to one pending plan and advances only across complete groups.
inline void advanceTerrainLodUploadCursor(
    TerrainLodScene const& scene, std::span<uint32_t const> const uploaded, size_t& cursor
)
{
    if (uploaded.size() != scene.groups.size() || cursor > uploaded.size()) {
        throw std::invalid_argument("terrain LOD upload progress does not match its scene");
    }
    while (cursor < uploaded.size() && uploaded[cursor] == scene.groups[cursor].range.instance_count) {
        ++cursor;
    }
}

struct TerrainLodUploadPlan final {
    uint64_t generation = 0U;
    std::shared_ptr<TerrainLodScene const> scene;
    std::vector<StoneIndirectDraws::Range> ranges;
    std::vector<std::optional<uint32_t>> reusable;
    std::vector<uint32_t> uploaded;
    std::exception_ptr failure;
};

inline void validateTerrainLodEditSourceRevisions(std::span<TerrainLodEditSource const> const sources)
{
    std::map<std::tuple<int32_t, int32_t, int32_t>, uint64_t> revisions;
    for (TerrainLodEditSource const& source : sources) {
        auto const [entry, inserted] = revisions.emplace(
            std::tuple{source.coordinate.x, source.coordinate.y, source.coordinate.z}, source.revision);
        if (!inserted && entry->second != source.revision) {
            throw std::invalid_argument("terrain LOD scene has conflicting edit revisions");
        }
    }
}

// Existing group indices remain stable; only explicit patch keys lose GPU reuse.
[[nodiscard]] inline TerrainLodUploadPlan terrainLodReplacementPlan(
    TerrainLodScene const& active,
    std::span<StoneIndirectDraws::Range const> active_ranges,
    TerrainLodScene const& patch
)
{
    if (active.identity != patch.identity || active.session_join_epoch != patch.session_join_epoch
        || active.coverage_epoch != patch.coverage_epoch || active_ranges.size() != active.groups.size()
        || (patch.visual_center && patch.visual_center != active.visual_center)) {
        throw std::invalid_argument("terrain LOD replacement identity does not match active scene");
    }
    validateTerrainLodGroups(active);
    validateTerrainLodGroups(patch);
    validateTerrainLodEditSourceRevisions(active.edit_sources);
    validateTerrainLodEditSourceRevisions(patch.edit_sources);
    std::map<std::tuple<int32_t, int32_t, int32_t>, uint64_t> active_edit_revisions;
    for (TerrainLodEditSource const& source : active.edit_sources) {
        active_edit_revisions.emplace(
            std::tuple{source.coordinate.x, source.coordinate.y, source.coordinate.z}, source.revision);
    }
    for (TerrainLodEditSource const& source : patch.edit_sources) {
        auto const active_source = active_edit_revisions.find(
            std::tuple{source.coordinate.x, source.coordinate.y, source.coordinate.z});
        if (active_source != active_edit_revisions.end() && source.revision < active_source->second) {
            throw std::invalid_argument("terrain LOD replacement edit revision moves backwards");
        }
    }
    using Key = std::tuple<uint32_t, uint32_t, uint32_t>;
    std::map<Key, uint32_t> replacement_groups;
    for (uint32_t index = 0U; index < patch.groups.size(); ++index) {
        auto const& group = patch.groups[index];
        if (group.extent != 16U) {
            throw std::invalid_argument("terrain LOD replacement requires exact groups");
        }
        replacement_groups.emplace(Key{group.origin_x, group.origin_y, group.extent}, index);
    }
    auto merged = std::make_shared<TerrainLodScene>();
    merged->identity = active.identity;
    merged->session_join_epoch = active.session_join_epoch;
    merged->coverage_epoch = active.coverage_epoch;
    merged->center_x = active.center_x;
    merged->center_y = active.center_y;
    merged->visual_center = active.visual_center;
    merged->shading_atlas = active.shading_atlas;
    merged->root_artifact = active.root_artifact;
    merged->local_layout = active.local_layout;
    merged->normal_context = active.normal_context;
    merged->river_constraints = active.river_constraints;
    merged->root_extent = active.root_extent;
    merged->block_sea_level = active.block_sea_level;
    merged->groups = active.groups;
    merged->leaves = active.leaves;
    merged->actual_column_count = active.actual_column_count;
    merged->chunk_sources = active.chunk_sources;
    merged->edit_sources = active.edit_sources;
    merged->authority_sources = active.authority_sources;
    merged->draw_index = active.draw_index;
    // Geometry and its captured dependency identities publish together. Shared
    // boundary sources can repeat; the patch supersedes every old occurrence.
    auto merge_sources = []<typename Source>(
        std::vector<Source>& sources, std::vector<Source> const& replacements) {
        std::map<std::tuple<int32_t, int32_t, int32_t>, Source> by_coordinate;
        for (auto const& source : sources) {
            by_coordinate.insert_or_assign(
                std::tuple{source.coordinate.x, source.coordinate.y, source.coordinate.z}, source);
        }
        for (auto const& source : replacements) {
            by_coordinate.insert_or_assign(
                std::tuple{source.coordinate.x, source.coordinate.y, source.coordinate.z}, source);
        }
        sources.clear();
        sources.reserve(by_coordinate.size());
        for (auto& [coordinate, source] : by_coordinate) {
            static_cast<void>(coordinate);
            sources.push_back(std::move(source));
        }
    };
    merge_sources(merged->chunk_sources, patch.chunk_sources);
    merge_sources(merged->edit_sources, patch.edit_sources);
    merge_sources(merged->authority_sources, patch.authority_sources);
    TerrainLodUploadPlan result;
    result.ranges.resize(active.groups.size());
    result.reusable.resize(active.groups.size());
    result.uploaded.resize(active.groups.size());
    bool draw_index_bounds_changed = false;
    uint64_t logical_face_count = 0U;
    for (uint32_t index = 0U; index < active.groups.size(); ++index) {
        auto const& old_group = active.groups[index];
        auto const found = replacement_groups.find(Key{old_group.origin_x, old_group.origin_y, old_group.extent});
        auto group = old_group;
        if (found != replacement_groups.end()) {
            group = patch.groups[found->second];
            // The immutable index stores bounds in group order, independently of face ranges.
            if (group.minimum_z != old_group.minimum_z || group.maximum_z != old_group.maximum_z) {
                draw_index_bounds_changed = true;
            }
            replacement_groups.erase(found);
        } else {
            result.reusable[index] = index;
            result.ranges[index] = active_ranges[index];
            result.uploaded[index] = group.range.instance_count;
        }
        if (logical_face_count > std::numeric_limits<uint32_t>::max() - group.range.instance_count) {
            throw std::invalid_argument("terrain LOD replacement face count exceeds the indexed range");
        }
        group.range.first_instance = static_cast<uint32_t>(logical_face_count);
        logical_face_count += group.range.instance_count;
        merged->groups[index] = group;
    }
    merged->logical_face_count = static_cast<size_t>(logical_face_count);
    if (!replacement_groups.empty()) {
        throw std::invalid_argument("terrain LOD replacement group is not presented");
    }
    if (draw_index_bounds_changed && merged->draw_index) {
        auto refitted_index = std::make_shared<TerrainLodDrawIndex>(*merged->draw_index);
        if (refitted_index->refit(merged->groups)) {
            merged->draw_index = std::move(refitted_index);
        } else {
            merged->draw_index.reset();
        }
    }
    result.scene = std::move(merged);
    return result;
}

struct TerrainLodReplacementJobResult final {
    uint64_t generation = 0U;
    std::shared_ptr<TerrainLodScene const> base;
    std::shared_ptr<TerrainLodScene const> patch;
    TerrainLodUploadPlan plan;
    std::exception_ptr failure;
};

// Immutable replacement planning stays off the render thread. Its packaged-task
// future never waits for worker completion when invalidated or destroyed.
class TerrainLodReplacementPlanner final {
public:
    [[nodiscard]]
    std::optional<TerrainLodUploadPlan> poll(
        std::shared_ptr<TerrainLodScene const> active,
        std::span<StoneIndirectDraws::Range const> active_ranges,
        std::shared_ptr<TerrainLodScene const> patch
    )
    {
        if (!active || !patch) {
            throw std::invalid_argument("terrain LOD replacement planning requires active and patch scenes");
        }
        if (active->identity != patch->identity || active->session_join_epoch != patch->session_join_epoch
            || active->coverage_epoch != patch->coverage_epoch || active_ranges.size() != active->groups.size()) {
            throw std::invalid_argument("terrain LOD replacement identity does not match active scene");
        }
        if (m_requested_base != active || m_requested_patch != patch) {
            m_requested_base = active;
            m_requested_patch = patch;
            m_requested_ranges.assign(active_ranges.begin(), active_ranges.end());
            ++m_generation;
        }
        if (m_future.valid()) {
            if (m_future.wait_for(std::chrono::seconds{ 0 }) != std::future_status::ready) {
                return std::nullopt;
            }
            auto result = m_future.get();
            if (result.generation == m_generation && result.base == active && result.patch == patch) {
                if (result.failure) {
                    auto const failure = result.failure;
                    invalidate();
                    std::rethrow_exception(failure);
                }
                invalidate();
                return std::move(result.plan);
            }
        }

        auto const generation = m_generation;
        auto ranges = std::move(m_requested_ranges);
        std::future<TerrainLodReplacementJobResult> future;
        std::thread worker;
        try {
            std::packaged_task<TerrainLodReplacementJobResult()> task([
                active = std::move(active), patch = std::move(patch), ranges = std::move(ranges), generation
            ] {
                TerrainLodReplacementJobResult result{
                    .generation = generation,
                    .base = active,
                    .patch = patch,
                };
                try {
                    result.plan = terrainLodReplacementPlan(*active, ranges, *patch);
                    result.plan.generation = generation;
                } catch (...) {
                    result.failure = std::current_exception();
                }
                return result;
            });
            future = task.get_future();
            worker = std::thread(std::move(task));
        } catch (...) {
            invalidate();
            throw;
        }
        worker.detach();
        m_future = std::move(future);
        return std::nullopt;
    }

    [[nodiscard]]
    bool pending() const noexcept
    {
        return m_requested_patch != nullptr && m_future.valid();
    }

    void invalidate() noexcept
    {
        m_requested_base.reset();
        m_requested_patch.reset();
        m_requested_ranges.clear();
        ++m_generation;
    }

private:
    uint64_t m_generation = 0U;
    std::shared_ptr<TerrainLodScene const> m_requested_base;
    std::shared_ptr<TerrainLodScene const> m_requested_patch;
    std::vector<StoneIndirectDraws::Range> m_requested_ranges;
    std::future<TerrainLodReplacementJobResult> m_future;
};

class TerrainLodUploadPlanner final {
public:
    [[nodiscard]]
    std::optional<TerrainLodUploadPlan> poll(
        std::shared_ptr<TerrainLodScene const> active,
        std::span<StoneIndirectDraws::Range const> active_ranges,
        std::shared_ptr<TerrainLodScene const> candidate
    )
    {
        if (!candidate) {
            throw std::invalid_argument("terrain LOD upload planning requires a candidate");
        }
        if (m_requested != candidate) {
            m_requested = candidate;
            ++m_generation;
        }
        if (m_future.valid()) {
            if (m_future.wait_for(std::chrono::seconds{ 0 }) != std::future_status::ready) {
                return std::nullopt;
            }
            auto result = m_future.get();
            if (result.generation == m_generation) {
                if (result.failure) {
                    std::rethrow_exception(result.failure);
                }
                return result;
            }
        }
        std::vector<StoneIndirectDraws::Range> ranges(active_ranges.begin(), active_ranges.end());
        m_future = std::async(std::launch::async, [
            active = std::move(active), candidate = std::move(candidate),
            ranges = std::move(ranges), generation = m_generation
        ] {
            TerrainLodUploadPlan result{
                .generation = generation,
                .scene = candidate,
                .ranges = {},
                .reusable = {},
                .uploaded = {},
                .failure = {},
            };
            try {
                result.reusable = terrainLodReusableGroups(active.get(), *candidate);
                result.ranges.resize(candidate->groups.size());
                result.uploaded.resize(candidate->groups.size());
                for (uint32_t index = 0U; index < result.reusable.size(); ++index) {
                    if (result.reusable[index]) {
                        result.ranges[index] = ranges.at(*result.reusable[index]);
                        result.uploaded[index] = result.ranges[index].instance_count;
                    }
                }
            } catch (...) {
                result.failure = std::current_exception();
            }
            return result;
        });
        return std::nullopt;
    }

    [[nodiscard]]
    bool pending() const noexcept
    {
        return m_requested != nullptr && m_future.valid();
    }

    void invalidate() noexcept
    {
        m_requested.reset();
        ++m_generation;
    }

private:
    uint64_t m_generation = 0U;
    std::shared_ptr<TerrainLodScene const> m_requested;
    std::future<TerrainLodUploadPlan> m_future;
};

} // namespace client::detail
