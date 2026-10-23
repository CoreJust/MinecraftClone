#include "Numerics.hpp"

#include <generation/Generation.hpp>
#include <generation/GenerationStages.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <queue>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <utility>
#include <vector>

namespace generation::detail
{
namespace
{

constexpr double WORLD_SIZE = 65'536.0;
constexpr std::array<std::pair<int32_t, int32_t>, 8U> NEIGHBORS{{
    {-1, -1}, {-1, 0}, {-1, 1}, {0, -1}, {0, 1}, {1, -1}, {1, 0}, {1, 1}
}};

struct Components
{
    Int32Field labels;
    std::vector<int32_t> sizes;
};

struct Route
{
    DoubleField filled;
    Int32Field downstream;
    Int32Field rank;
};

struct Drainage
{
    ByteField ocean;
    DoubleField filled;
    Int32Field downstream;
    Int32Field rank;
    DoubleField flow;
    ByteField lake;
    ByteField river;
    DoubleField water_level;
    uint32_t sea_components = 0U;
    double threshold = 0.0;
};

struct NeighborCoordinate final
{
    uint32_t coordinate;
    int32_t offset;
    uint32_t resolution;
};

struct OceanSelection final
{
    float sea_level;
    uint32_t resolution;
};

struct RouteLinks final
{
    std::span<int32_t const> downstream;
    std::span<int32_t const> rank;
};

[[nodiscard]]
uint32_t wrapNeighbor(NeighborCoordinate const& neighbor)
{
    int32_t wrapped = static_cast<int32_t>(neighbor.coordinate) + neighbor.offset;
    int32_t const extent = static_cast<int32_t>(neighbor.resolution);
    if (wrapped < 0) wrapped += extent;
    else if (wrapped >= extent) wrapped -= extent;
    return static_cast<uint32_t>(wrapped);
}

class DisjointSets
{
public:
    explicit DisjointSets(size_t size) : parent_(size)
    {
        for (size_t index = 0U; index < size; ++index) {
            parent_[index] = static_cast<int32_t>(index);
        }
    }

    int32_t root(int32_t value)
    {
        int32_t current = value;
        while (parent_[static_cast<size_t>(current)] != current) {
            parent_[static_cast<size_t>(current)] = parent_[static_cast<size_t>(
                parent_[static_cast<size_t>(current)])];
            current = parent_[static_cast<size_t>(current)];
        }
        return current;
    }

    void join(int32_t first, int32_t second)
    {
        first = root(first);
        second = root(second);
        if (first != second) {
            parent_[static_cast<size_t>(std::max(first, second))] = std::min(first, second);
        }
    }

private:
    Int32Field parent_;
};

[[nodiscard]]
Components periodicComponents(
    std::span<uint8_t const> mask,
    uint32_t resolution,
    std::stop_token const& cancellation
)
{
    size_t const count = mask.size();
    DisjointSets sets(count);
    for (uint32_t y = 0U; y < resolution; ++y) {
        if ((y & 31U) == 0U) checkCancellation(cancellation);
        for (uint32_t x = 0U; x < resolution; ++x) {
            size_t const index = static_cast<size_t>(y) * resolution + x;
            if (mask[index] == 0U) continue;
            for (auto const [dy, dx] : NEIGHBORS) {
                uint32_t const ny = wrapNeighbor(
                    NeighborCoordinate{.coordinate = y, .offset = dy, .resolution = resolution}
                );
                uint32_t const nx = wrapNeighbor(
                    NeighborCoordinate{.coordinate = x, .offset = dx, .resolution = resolution}
                );
                size_t const neighbor = static_cast<size_t>(ny) * resolution + nx;
                if (mask[neighbor] != 0U) {
                    sets.join(static_cast<int32_t>(index), static_cast<int32_t>(neighbor));
                }
            }
        }
    }

    Int32Field first_index(count, -1);
    for (size_t index = 0U; index < count; ++index) {
        if (mask[index] != 0U) {
            int32_t const component_root = sets.root(static_cast<int32_t>(index));
            int32_t& first = first_index[static_cast<size_t>(component_root)];
            if (first < 0) first = static_cast<int32_t>(index);
        }
    }
    std::vector<std::pair<int32_t, int32_t>> order;
    for (size_t index = 0U; index < first_index.size(); ++index) {
        if (first_index[index] >= 0) order.emplace_back(first_index[index], static_cast<int32_t>(index));
    }
    std::sort(order.begin(), order.end());
    Int32Field root_id(count, 0);
    for (size_t component = 0U; component < order.size(); ++component) {
        root_id[static_cast<size_t>(order[component].second)] = static_cast<int32_t>(component + 1U);
    }
    Components result{Int32Field(count, 0), std::vector<int32_t>(order.size() + 1U, 0)};
    for (size_t index = 0U; index < count; ++index) {
        if (mask[index] != 0U) {
            int32_t const component = root_id[static_cast<size_t>(sets.root(static_cast<int32_t>(index)))];
            result.labels[index] = component;
            ++result.sizes[static_cast<size_t>(component)];
        }
    }
    return result;
}

[[nodiscard]]
ByteField chooseOcean(
    std::span<double const> bed,
    OceanSelection const& selection,
    uint32_t& sea_components,
    std::stop_token const& cancellation
)
{
    float const sea_level = selection.sea_level;
    uint32_t const resolution = selection.resolution;
    ByteField below_sea(bed.size(), 0U);
    for (size_t index = 0U; index < bed.size(); ++index) {
        below_sea[index] = static_cast<uint8_t>(bed[index] <= sea_level);
    }
    Components const components = periodicComponents(below_sea, resolution, cancellation);
    sea_components = static_cast<uint32_t>(components.sizes.size() - 1U);
    if (sea_components == 0U) {
        return ByteField(bed.size(), 0U);
    }
    int32_t chosen = 1;
    for (int32_t component = 2; component <= static_cast<int32_t>(sea_components); ++component) {
        if (components.sizes[static_cast<size_t>(component)]
            > components.sizes[static_cast<size_t>(chosen)]) {
            chosen = component;
        }
    }
    ByteField ocean(bed.size(), 0U);
    for (size_t index = 0U; index < bed.size(); ++index) {
        ocean[index] = static_cast<uint8_t>(components.labels[index] == chosen);
    }
    return ocean;
}

struct HeapEntry
{
    double height;
    int32_t index;
};

struct LaterEntry
{
    bool operator()(HeapEntry const& lhs, HeapEntry const& rhs) const
    {
        if (lhs.height != rhs.height) return lhs.height > rhs.height;
        return lhs.index > rhs.index;
    }
};

[[nodiscard]]
Route priorityFlood(
    std::span<double const> bed,
    std::span<uint8_t const> ocean,
    uint32_t resolution,
    std::stop_token const& cancellation
)
{
    size_t const count = bed.size();
    Route route{
        .filled = DoubleField(count),
        .downstream = Int32Field(count, -1),
        .rank = Int32Field(count, -1),
    };
    ByteField visited(count, 0U);
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, LaterEntry> heap;
    bool has_anchor = false;
    for (size_t index = 0U; index < count; ++index) {
        if (ocean[index] != 0U) {
            visited[index] = 1U;
            route.filled[index] = bed[index];
            heap.push({bed[index], static_cast<int32_t>(index)});
            has_anchor = true;
        }
    }
    if (!has_anchor) {
        size_t sink = 0U;
        for (size_t index = 1U; index < count; ++index) {
            if (bed[index] < bed[sink]) sink = index;
        }
        visited[sink] = 1U;
        route.filled[sink] = bed[sink];
        heap.push({bed[sink], static_cast<int32_t>(sink)});
    }

    int32_t order = 0;
    while (!heap.empty()) {
        if ((static_cast<uint32_t>(order) & 4'095U) == 0U) checkCancellation(cancellation);
        HeapEntry const entry = heap.top();
        heap.pop();
        int32_t const index = entry.index;
        route.rank[static_cast<size_t>(index)] = order++;
        uint32_t const y = static_cast<uint32_t>(index) / resolution;
        uint32_t const x = static_cast<uint32_t>(index) % resolution;
        for (auto const [dy, dx] : NEIGHBORS) {
            uint32_t const ny = wrapNeighbor(
                NeighborCoordinate{.coordinate = y, .offset = dy, .resolution = resolution}
            );
            uint32_t const nx = wrapNeighbor(
                NeighborCoordinate{.coordinate = x, .offset = dx, .resolution = resolution}
            );
            size_t const target = static_cast<size_t>(ny) * resolution + nx;
            if (visited[target] != 0U) continue;
            visited[target] = 1U;
            route.downstream[target] = index;
            double const target_height = std::max(entry.height, bed[target]);
            route.filled[target] = target_height;
            heap.push({target_height, static_cast<int32_t>(target)});
        }
    }

    DoubleField best_slope(count, 0.0);
    constexpr double tie_tolerance = 1.0e-12;
    for (auto const [dy, dx] : NEIGHBORS) {
        for (uint32_t y = 0U; y < resolution; ++y) {
            if ((y & 31U) == 0U) checkCancellation(cancellation);
            uint32_t const ny = wrapNeighbor(
                NeighborCoordinate{.coordinate = y, .offset = dy, .resolution = resolution}
            );
            for (uint32_t x = 0U; x < resolution; ++x) {
                size_t const index = static_cast<size_t>(y) * resolution + x;
                if (ocean[index] != 0U) continue;
                uint32_t const nx = wrapNeighbor(
                    NeighborCoordinate{.coordinate = x, .offset = dx, .resolution = resolution}
                );
                size_t const neighbor = static_cast<size_t>(ny) * resolution + nx;
                double const slope = (route.filled[index] - route.filled[neighbor])
                    / std::hypot(static_cast<double>(dx), static_cast<double>(dy));
                bool const better = slope > best_slope[index] + tie_tolerance
                    || (slope > tie_tolerance
                        && std::abs(slope - best_slope[index]) <= tie_tolerance
                        && static_cast<int32_t>(neighbor) < route.downstream[index]);
                if (better) {
                    route.downstream[index] = static_cast<int32_t>(neighbor);
                    best_slope[index] = slope;
                }
            }
        }
    }
    return route;
}

[[nodiscard]]
DoubleField accumulate(
    RouteLinks const& route,
    std::span<double const> runoff,
    std::stop_token const& cancellation
)
{
    DoubleField flow(runoff.begin(), runoff.end());
    std::vector<int32_t> order(route.rank.size());
    for (size_t index = 0U; index < route.rank.size(); ++index) {
        order[static_cast<size_t>(route.rank[index])] = static_cast<int32_t>(index);
    }
    for (size_t remaining = order.size(); remaining > 0U; --remaining) {
        if ((remaining & 4'095U) == 0U) checkCancellation(cancellation);
        int32_t const index = order[remaining - 1U];
        int32_t const target = route.downstream[static_cast<size_t>(index)];
        if (target >= 0) flow[static_cast<size_t>(target)] += flow[static_cast<size_t>(index)];
    }
    return flow;
}

[[nodiscard]]
Int32Field outletIds(RouteLinks const& route)
{
    Int32Field order(route.rank.size());
    for (size_t index = 0U; index < route.rank.size(); ++index) {
        order[static_cast<size_t>(route.rank[index])] = static_cast<int32_t>(index);
    }
    Int32Field outlet(route.rank.size(), -1);
    for (size_t step = 0U; step < order.size(); ++step) {
        int32_t const index = order[step];
        int32_t const target = route.downstream[static_cast<size_t>(index)];
        outlet[static_cast<size_t>(index)] = target < 0
            ? index : outlet[static_cast<size_t>(target)];
    }
    return outlet;
}

[[nodiscard]]
Drainage fields(
    std::span<double const> bed,
    float sea_level,
    std::span<double const> runoff,
    uint32_t resolution,
    std::stop_token const& cancellation
)
{
    Drainage result;
    result.ocean = chooseOcean(
        bed,
        OceanSelection{.sea_level = sea_level, .resolution = resolution},
        result.sea_components,
        cancellation
    );
    Route route = priorityFlood(bed, result.ocean, resolution, cancellation);
    result.filled = std::move(route.filled);
    result.downstream = std::move(route.downstream);
    result.rank = std::move(route.rank);
    size_t const count = bed.size();
    result.lake.resize(count, 0U);
    result.river.resize(count, 0U);
    result.water_level.resize(count);
    DoubleField effective_runoff(runoff.begin(), runoff.end());
    for (size_t index = 0U; index < count; ++index) {
        result.lake[index] = static_cast<uint8_t>(result.ocean[index] == 0U
            && result.filled[index] > bed[index] + 1.0e-6);
        if (result.ocean[index] != 0U) effective_runoff[index] = 0.0;
    }
    result.flow = accumulate(
        RouteLinks{.downstream = result.downstream, .rank = result.rank},
        effective_runoff,
        cancellation
    );
    result.threshold = std::max(8.0, static_cast<double>(count) * 0.0005);
    for (size_t index = 0U; index < count; ++index) {
        result.river[index] = static_cast<uint8_t>(result.flow[index] >= result.threshold
            && result.ocean[index] == 0U && result.lake[index] == 0U && bed[index] > sea_level);
        result.water_level[index] = result.ocean[index] != 0U ? sea_level
            : (result.lake[index] != 0U ? result.filled[index] : bed[index]);
    }
    return result;
}

[[nodiscard]]
uint32_t roundedRadius(double world_units, double cell_world_units)
{
    return std::max(1U, static_cast<uint32_t>(std::nearbyint(world_units / cell_world_units)));
}

} // namespace

HydrologyFields buildHydrology(
    GenerationConfiguration const& configuration,
    std::span<float const> elevation,
    std::span<float const> precipitation,
    std::span<float const> temperature,
    HydrologyParameters const& parameters,
    std::stop_token const& cancellation,
    ProgressCallback const& progress
)
{
    checkCancellation(cancellation);
    uint32_t const erosion_steps = parameters.erosion_steps;
    float const sea_level = parameters.sea_level;
    GenerationStage const stage = parameters.stage;
    uint32_t const resolution = configuration.resolution;
    size_t const count = static_cast<size_t>(resolution) * resolution;
    if (resolution < 3U || count > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        throw std::invalid_argument("hydrology resolution must be at least 3 and fit a 32-bit field size");
    }
    if (elevation.size() != count || (!precipitation.empty() && precipitation.size() != count)
        || (!temperature.empty() && temperature.size() != count)) {
        throw std::invalid_argument("hydrology fields must match the configured square resolution");
    }
    if (!std::isfinite(sea_level)
        || !std::all_of(elevation.begin(), elevation.end(), [](float value) { return std::isfinite(value); })
        || !std::all_of(precipitation.begin(), precipitation.end(), [](float value) { return std::isfinite(value); })
        || !std::all_of(temperature.begin(), temperature.end(), [](float value) { return std::isfinite(value); })) {
        throw std::invalid_argument("hydrology fields and sea level must be finite");
    }

    DoubleField bed(elevation.begin(), elevation.end());
    DoubleField const original_bed = bed;
    uint32_t original_component_count = 0U;
    ByteField const original_ocean = chooseOcean(
        bed,
        OceanSelection{.sea_level = sea_level, .resolution = resolution},
        original_component_count,
        cancellation
    );
    DoubleField runoff(count, 1.0);
    if (!precipitation.empty()) {
        for (size_t index = 0U; index < count; ++index) {
            runoff[index] = 0.05 + std::max(static_cast<double>(precipitation[index]), 0.0);
        }
    }
    for (double& value : runoff) value = std::clamp(value, 0.0, 8.0);

    ByteField submerged_glacial_cells(count, 0U);
    ByteField glacial_route_cells(count, 0U);
    double const cell_world_units = WORLD_SIZE / resolution;
    uint32_t const total_work = std::max(erosion_steps, 1U) + 1U;
    reportProgress(progress, stage, 0U, total_work);
    for (uint32_t step = 0U; step < erosion_steps; ++step) {
        checkCancellation(cancellation);
        Drainage current = fields(bed, sea_level, runoff, resolution, cancellation);
        DoubleField slope(count, 0.0);
        for (size_t source = 0U; source < count; ++source) {
            int32_t const receiver = current.downstream[source];
            if (receiver < 0) continue;
            size_t const target = static_cast<size_t>(receiver);
            uint32_t const sy = static_cast<uint32_t>(source / resolution);
            uint32_t const sx = static_cast<uint32_t>(source % resolution);
            uint32_t const ty = static_cast<uint32_t>(target / resolution);
            uint32_t const tx = static_cast<uint32_t>(target % resolution);
            uint32_t const raw_dx = sx > tx ? sx - tx : tx - sx;
            uint32_t const raw_dy = sy > ty ? sy - ty : ty - sy;
            uint32_t const dx = std::min(raw_dx, resolution - raw_dx);
            uint32_t const dy = std::min(raw_dy, resolution - raw_dy);
            double const distance = std::max(std::hypot(static_cast<double>(dx), static_cast<double>(dy))
                * cell_world_units, 1.0);
            double const drop = std::max(bed[source] - bed[target], 0.0);
            slope[source] = drop * 256.0 / distance;
        }
        DoubleField incision(count, 0.0);
        for (size_t index = 0U; index < count; ++index) {
            double const channel = std::sqrt(std::clamp(current.flow[index] / current.threshold, 0.0, 64.0));
            incision[index] = std::min(0.035, 0.005 * channel * slope[index]);
            if (current.ocean[index] != 0U || current.lake[index] != 0U || bed[index] <= sea_level) {
                incision[index] = 0.0;
            }
        }
        DoubleField channel_cut(count);
        for (size_t index = 0U; index < count; ++index) {
            channel_cut[index] = incision[index] * current.river[index];
        }
        DoubleField valley = gaussianFilterWrap(
            channel_cut,
            GaussianFilterParameters{.resolution = resolution, .sigma = 384.0 / cell_world_units},
            cancellation
        );
        DoubleField cut(count);
        for (size_t index = 0U; index < count; ++index) {
            cut[index] = std::min(0.045, channel_cut[index] + 2.0 * valley[index]);
        }

        DoubleField glacial(count, 0.0);
        if (!temperature.empty()) {
            DoubleField cold_source(count, 0.0);
            for (size_t index = 0U; index < count; ++index) {
                if (temperature[index] < -6.0F
                    && bed[index] > static_cast<double>(sea_level) + 0.35) {
                    cold_source[index] = runoff[index];
                }
            }
            DoubleField const cold_flow = accumulate(
                RouteLinks{.downstream = current.downstream, .rank = current.rank},
                cold_source,
                cancellation
            );
            uint32_t const coast_radius = roundedRadius(1536.0, cell_world_units);
            ByteField const coastal = maximumFilterWrap(current.ocean, resolution, 2U * coast_radius + 1U, cancellation);
            ByteField glacier_route(count, 0U);
            for (size_t index = 0U; index < count; ++index) {
                bool const route = cold_flow[index] > current.threshold * 1.5
                    && coastal[index] != 0U && current.ocean[index] == 0U
                    && current.lake[index] == 0U
                    && bed[index] <= static_cast<double>(sea_level) + 0.24;
                glacier_route[index] = static_cast<uint8_t>(route);
                glacial_route_cells[index] = static_cast<uint8_t>(glacial_route_cells[index] != 0U || route);
            }
            DoubleField glacier_values(count);
            for (size_t index = 0U; index < count; ++index) glacier_values[index] = glacier_route[index];
            DoubleField lateral = gaussianFilterWrap(
                glacier_values,
                GaussianFilterParameters{.resolution = resolution, .sigma = 384.0 / cell_world_units},
                cancellation
            );
            for (size_t index = 0U; index < count; ++index) {
                glacial[index] = std::min(0.035, 0.012 * glacier_route[index] + 0.03 * lateral[index]);
                double const crossing = std::min(0.08, std::max(0.0, bed[index] - (sea_level - 0.025)));
                if (glacier_route[index] != 0U) glacial[index] = std::max(glacial[index], crossing);
                if (bed[index] <= static_cast<double>(sea_level) - 0.12) glacial[index] = 0.0;
                cut[index] += glacial[index];
            }
        }

        for (size_t index = 0U; index < count; ++index) {
            cut[index] = std::min(cut[index], 0.08);
            if (current.ocean[index] != 0U || current.lake[index] != 0U) cut[index] = 0.0;
            if (!temperature.empty() && glacial[index] > 0.001 && bed[index] > sea_level
                && bed[index] - cut[index] <= sea_level) {
                submerged_glacial_cells[index] = 1U;
            }
            bed[index] -= cut[index];
        }
        reportProgress(progress, stage, step + 1U, total_work);
    }

    checkCancellation(cancellation);
    Drainage final_fields = fields(bed, sea_level, runoff, resolution, cancellation);
    Components const lake_components = periodicComponents(final_fields.lake, resolution, cancellation);
    Int32Field const outlet_id = outletIds(RouteLinks{
        .downstream = final_fields.downstream,
        .rank = final_fields.rank,
    });

    ByteField submerged_core(count, 0U);
    for (size_t index = 0U; index < count; ++index) {
        submerged_core[index] = static_cast<uint8_t>(final_fields.ocean[index] != 0U
            && submerged_glacial_cells[index] != 0U && glacial_route_cells[index] != 0U
            && original_bed[index] > sea_level);
    }
    Components const fjord_components = periodicComponents(submerged_core, resolution, cancellation);
    ByteField touches_old_sea = maximumFilterWrap(original_ocean, resolution, 3U, cancellation);
    ByteField valid_fjord_component(fjord_components.sizes.size(), 0U);
    for (size_t index = 0U; index < count; ++index) {
        if (submerged_core[index] != 0U && touches_old_sea[index] != 0U) {
            int32_t const component = fjord_components.labels[index];
            if (component > 0 && fjord_components.sizes[static_cast<size_t>(component)] >= 2) {
                valid_fjord_component[static_cast<size_t>(component)] = 1U;
            }
        }
    }
    ByteField fjord_mask(count, 0U);
    for (size_t index = 0U; index < count; ++index) {
        int32_t const component = fjord_components.labels[index];
        fjord_mask[index] = static_cast<uint8_t>(submerged_core[index] != 0U && component > 0
            && valid_fjord_component[static_cast<size_t>(component)] != 0U);
    }

    DoubleField land(count, 0.0);
    for (size_t index = 0U; index < count; ++index) {
        land[index] = static_cast<double>(final_fields.ocean[index] == 0U && bed[index] > sea_level);
    }
    uint32_t const inner_radius = roundedRadius(512.0, cell_world_units);
    uint32_t const outer_radius = std::max(inner_radius + 1U, roundedRadius(2048.0, cell_world_units));
    DoubleField const inner_land = uniformFilterWrap(land, resolution, 2U * inner_radius + 1U, cancellation);
    DoubleField const outer_land = uniformFilterWrap(land, resolution, 2U * outer_radius + 1U, cancellation);
    ByteField land_mask(count);
    for (size_t index = 0U; index < count; ++index) land_mask[index] = static_cast<uint8_t>(land[index] != 0.0);
    ByteField adjacent_land = maximumFilterWrap(land_mask, resolution, 3U, cancellation);
    uint32_t const shelter_radius = roundedRadius(768.0, cell_world_units);
    ByteField west(count, 0U), east(count, 0U), north(count, 0U), south(count, 0U);
    for (uint32_t distance = 1U; distance <= shelter_radius; ++distance) {
        checkCancellation(cancellation);
        for (uint32_t y = 0U; y < resolution; ++y) {
            uint32_t const north_y = (y + distance) % resolution;
            uint32_t const south_y = (y + resolution - (distance % resolution)) % resolution;
            for (uint32_t x = 0U; x < resolution; ++x) {
                uint32_t const west_x = (x + distance) % resolution;
                uint32_t const east_x = (x + resolution - (distance % resolution)) % resolution;
                size_t const index = static_cast<size_t>(y) * resolution + x;
                west[index] = static_cast<uint8_t>(
                    west[index] != 0U
                        || land_mask[static_cast<size_t>(y) * resolution + west_x] != 0U
                );
                east[index] = static_cast<uint8_t>(
                    east[index] != 0U
                        || land_mask[static_cast<size_t>(y) * resolution + east_x] != 0U
                );
                north[index] = static_cast<uint8_t>(
                    north[index] != 0U
                        || land_mask[static_cast<size_t>(north_y) * resolution + x] != 0U
                );
                south[index] = static_cast<uint8_t>(
                    south[index] != 0U
                        || land_mask[static_cast<size_t>(south_y) * resolution + x] != 0U
                );
            }
        }
    }
    ByteField harbour_mask(count, 0U);
    for (size_t index = 0U; index < count; ++index) {
        bool const enclosed = (west[index] != 0U && east[index] != 0U)
            || (north[index] != 0U && south[index] != 0U);
        bool const harbour = final_fields.ocean[index] != 0U && adjacent_land[index] != 0U && enclosed
            && sea_level - bed[index] >= 0.05 && inner_land[index] >= 0.30 && inner_land[index] <= 0.75
            && outer_land[index] <= 0.55 && outer_land[index] <= inner_land[index] - 0.08;
        harbour_mask[index] = static_cast<uint8_t>(harbour);
    }

    HydrologyFields result{
        .elevation = FloatField(count),
        .filled = FloatField(count),
        .water_level = FloatField(count),
        .flow_accumulation = FloatField(count),
        .downstream = std::move(final_fields.downstream),
        .lake_id = lake_components.labels,
        .outlet_id = outlet_id,
        .lake_mask = std::move(final_fields.lake),
        .river_mask = std::move(final_fields.river),
        .ocean_mask = std::move(final_fields.ocean),
        .fjord_candidate_mask = std::move(fjord_mask),
        .harbour_candidate_mask = std::move(harbour_mask),
        .sea_components = final_fields.sea_components,
        .river_threshold_flux = static_cast<float>(final_fields.threshold),
    };
    for (size_t index = 0U; index < count; ++index) {
        result.elevation[index] = static_cast<float>(bed[index]);
        result.filled[index] = static_cast<float>(final_fields.filled[index]);
        result.water_level[index] = static_cast<float>(final_fields.water_level[index]);
        result.flow_accumulation[index] = static_cast<float>(final_fields.flow[index]);
    }
    reportProgress(progress, stage, total_work, total_work);
    return result;
}

} // namespace generation::detail
