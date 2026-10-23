#pragma once

#include <client/TerrainLodPresentation.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace client::detail {

inline void validateTerrainLodGroups(TerrainLodScene const& scene)
{
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, bool> keys;
    uint64_t face_count = 0U;
    std::vector<std::pair<uint32_t, uint32_t>> intervals;
    intervals.reserve(scene.groups.size());
    for (auto const& group : scene.groups) {
        if (group.range.first_instance > scene.faces.size()
            || group.range.instance_count > scene.faces.size() - group.range.first_instance) {
            throw std::invalid_argument("terrain LOD group face range is invalid");
        }
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
    if (face_count != scene.faces.size()) {
        throw std::invalid_argument("terrain LOD groups do not cover the scene face count");
    }
}

[[nodiscard]] inline std::span<StoneFaceInstance const> terrainLodGroupFaces(
    TerrainLodScene const& scene,
    TerrainLodDrawGroup const& group
)
{
    return std::span<StoneFaceInstance const>{ scene.faces }.subspan(
        group.range.first_instance, group.range.instance_count
    );
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
        if (std::ranges::equal(old_faces, new_faces)) {
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

struct TerrainLodUploadPlan final {
    uint64_t generation = 0U;
    std::shared_ptr<TerrainLodScene const> scene;
    std::vector<StoneIndirectDraws::Range> ranges;
    std::vector<std::optional<uint32_t>> reusable;
    std::vector<uint32_t> uploaded;
    std::exception_ptr failure;
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
