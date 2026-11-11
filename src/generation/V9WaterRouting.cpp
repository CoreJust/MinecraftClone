#include "V9WaterRouting.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <numeric>
#include <queue>

namespace generation::detail
{
namespace
{

constexpr double MODEL_YEAR_S = 360.0 * 86'400;

double wetFraction(V9WaterEquilibrium const& water, size_t id)
{
    if (!water.triangle_owner[id]) {
        return 0;
    }
    std::array<double, 3> depth;
    for (uint32_t k = 0; k < 3; ++k) {
        depth[k] = water.triangle_stage_m[id] - water.hydraulic_bed_m[water.triangle_vertex_ids[id][k]];
    }
    std::sort(depth.begin(), depth.end());
    double const l = depth[0], m = depth[1], h = depth[2];
    if (l >= 0 && h > 0) {
        return 1;
    }
    if (h > 0 && m <= 0) {
        return h * h / ((h - l) * (h - m));
    }
    if (l < 0 && m > 0) {
        return h / (h - l) + (-l) / (h - l) * m / (m - l);
    }
    return 0;
}

} // namespace

std::expected<V9WaterRouting, V9WaterFailure> routeV9Water(V9WaterEquilibrium const& water)
{
    auto failure = [](V9WaterFailure::Kind kind, std::string message) {
        return std::unexpected(V9WaterFailure{kind, std::move(message)});
    };
    size_t const size = water.hydraulic_bed_m.size(), count = size + water.bodies.size();
    if (!size || water.vertex_receiver.size() != size || water.vertex_basin_owner.size() != size ||
        water.ocean.size() != size || water.lake.size() != size) {
        return failure(V9WaterFailure::Kind::InvalidInput, "complete solved water equilibrium required");
    }
    std::vector<int32_t> body_index(water.topology.size(), -1), destination(water.topology.size(), -1);
    for (size_t k = 0; k < water.bodies.size(); ++k) {
        body_index[water.bodies[k].id] = static_cast<int32_t>(size + k);
    }
    for (auto const& transfer : water.spill_transfers) {
        if (body_index[transfer.source_node] >= 0) {
            if (transfer.entry_vertex < 0) {
                return failure(V9WaterFailure::Kind::InvalidInput, "missing real spill entry witness");
            }
            destination[transfer.source_node] = transfer.entry_vertex;
        }
    }
    std::vector<int32_t> downstream(count, -1);
    std::copy(water.vertex_receiver.begin(), water.vertex_receiver.end(), downstream.begin());
    for (size_t i = 0; i < size; ++i) {
        if (water.ocean[i]) {
            downstream[i] = -1;
        }
        if (downstream[i] != static_cast<int32_t>(i)) {
            continue;
        }
        int32_t const owner = water.vertex_basin_owner[i];
        if (owner) {
            downstream[i] = body_index[owner];
            continue;
        }
        int32_t target = water.vertex_outlet[i];
        if (target < 0) {
            int32_t node = water.vertex_leaf[i];
            while (node > 0 && water.topology[node].parent > 0) {
                node = water.topology[node].parent;
            }
            if (node > 0 && water.topology[node].spill_height_m <= 0) {
                target = water.topology[node].spill_receiver_vertex;
            }
        }
        downstream[i] = target;
    }
    for (auto const& body : water.bodies) {
        if (body.overflow_m3_s > 0) {
            if (destination[body.id] < 0) {
                return failure(V9WaterFailure::Kind::InvalidInput, "positive overflow without receiver witness");
            }
            downstream[body_index[body.id]] = destination[body.id];
        }
    }
    std::vector<double> flow(count);
    for (size_t id = 0; id < water.triangle_source_vertex.size(); ++id) {
        flow[water.triangle_source_vertex[id]] += water.triangle_runoff_source_m3_s[id] * (1 - wetFraction(water, id));
    }
    std::vector<int32_t> indegree(count);
    for (int32_t const receiver : downstream) {
        if (receiver >= 0) {
            ++indegree[receiver];
        }
    }
    std::priority_queue<int32_t, std::vector<int32_t>, std::greater<>> ready;
    for (size_t i = 0; i < count; ++i) {
        if (!indegree[i]) {
            ready.push(static_cast<int32_t>(i));
        }
    }
    V9WaterRouting result;
    size_t visited = 0;
    while (!ready.empty()) {
        int32_t const i = ready.top();
        ready.pop();
        ++visited;
        if (i >= static_cast<int32_t>(size)) {
            auto const& body = water.bodies[i - size];
            double const expected = body.source_m3_s - body.replaced_runoff_m3_s, extra = flow[i] - expected;
            if (std::abs(extra) > 1e-7 && (extra < 0 || body.overflow_m3_s <= 0)) {
                return failure(V9WaterFailure::Kind::BudgetMismatch,
                            "new receiver supply requires nonspilling lake re-equilibration");
            }
            double const received = flow[i];
            flow[i] = body.overflow_m3_s > 0 ? std::max(body.overflow_m3_s + extra, 0.0) : 0;
            result.body_max_residual_m3_s =
                std::max(result.body_max_residual_m3_s,
                         std::abs(received + body.precipitation_m3_s - body.evaporation_m3_s - flow[i]));
        }
        int32_t const receiver = downstream[i];
        if (receiver >= 0) {
            flow[receiver] += flow[i];
            if (--indegree[receiver] == 0) {
                ready.push(receiver);
            }
        }
    }
    if (visited != count) {
        return failure(V9WaterFailure::Kind::CyclicReceivers, "cyclic equilibrium receiver carriers");
    }
    for (size_t i = 0; i < size; ++i) {
        if (water.ocean[i]) {
            result.ocean_export_m3_s += flow[i];
        } else if (downstream[i] < 0 && flow[i] > 1e-10) {
            return failure(V9WaterFailure::Kind::BudgetMismatch, "nonmarine unowned sink carries positive water");
        }
    }
    if (result.body_max_residual_m3_s > 1e-7 || std::abs(result.ocean_export_m3_s - water.ocean_export_m3_s) > 1e-7) {
        return failure(V9WaterFailure::Kind::BudgetMismatch, "receiver lake or marine export mismatch");
    }
    result.downstream.assign(downstream.begin(), downstream.begin() + size);
    result.discharge_m3_s.assign(flow.begin(), flow.begin() + size);
    result.flow_m3_model_year.resize(size);
    result.river_discharge_m3_s.resize(size);
    result.flow.resize(size);
    result.river_coverage_fraction.resize(size);
    result.river.resize(size);
    result.river_virtual_through_lake.resize(size);
    result.river_width_m.resize(size);
    result.river_depth_m.resize(size);
    result.river_stage_m.resize(size);
    std::vector<double> land_flow;
    for (size_t i = 0; i < size; ++i) {
        if (!water.ocean[i]) {
            land_flow.push_back(flow[i]);
        }
    }
    std::sort(land_flow.begin(), land_flow.end());
    double quantile = 1;
    if (!land_flow.empty()) {
        double const index = (land_flow.size() - 1) * .975;
        size_t const low = static_cast<size_t>(std::floor(index)), high = static_cast<size_t>(std::ceil(index));
        double const fraction = index - low;
        quantile = fraction < .5 ? land_flow[low] + (land_flow[high] - land_flow[low]) * fraction
                            : land_flow[high] - (land_flow[high] - land_flow[low]) * (1 - fraction);
    }
    double const threshold = std::max({quantile, water.runoff_vertex_mean_m3_s * 18, 1e-8});
    for (size_t i = 0; i < size; ++i) {
        int32_t const receiver = downstream[i];
        if (receiver >= static_cast<int32_t>(size)) {
            auto const& body = water.bodies[receiver - size];
            result.downstream[i] = destination[body.id];
        }
        result.flow_m3_model_year[i] = flow[i] * MODEL_YEAR_S;
        result.river_discharge_m3_s[i] = static_cast<float>(flow[i]);
        result.flow[i] = static_cast<float>(result.flow_m3_model_year[i]);
        bool const river = flow[i] >= threshold && !water.ocean[i] && !water.lake[i];
        result.river[i] = river;
        result.river_virtual_through_lake[i] = water.lake[i] && flow[i] >= threshold;
        int32_t const target = std::max(result.downstream[i], 0);
        float const slope = std::max((water.water_level_m[i] - water.water_level_m[target]) / 256.0F, 1e-4F);
        double const width = river ? std::pow(flow[i] * .04 * std::pow(8, 2.0 / 3) / std::sqrt(slope), 3.0 / 8) : 0;
        result.river_width_m[i] = static_cast<float>(width);
        result.river_coverage_fraction[i] = static_cast<float>(std::min(width / 256, 1.0));
        result.river_depth_m[i] = static_cast<float>(width / 8);
        result.river_stage_m[i] = static_cast<float>(double(water.water_level_m[i]) + (river ? width / 8 : 0));
    }
    result.native_downstream.assign(result.downstream.begin(), result.downstream.end());
    return result;
}

} // namespace generation::detail
