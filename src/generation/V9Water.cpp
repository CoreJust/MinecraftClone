#include "V9Water.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <queue>
#include <utility>

namespace generation::detail
{
namespace
{

using Triangle = std::array<int32_t, 3>;
using Rates = std::array<double, 3>;
using Polynomial = std::array<std::array<double, 4>, 4>;

struct Hypsometry final
{
    double origin = 0;
    std::vector<double> events;
    std::vector<Polynomial> prefix;

    Hypsometry(std::vector<int32_t> const& members, std::vector<Triangle> const& triangles, std::span<double const> bed,
               std::vector<Rates> const& weights, double area)
    {
        origin = std::numeric_limits<double>::infinity();
        for (int32_t const id : members) {
            for (int32_t const vertex : triangles[id]) {
                origin = std::min(origin, bed[vertex]);
            }
        }
        std::vector<std::pair<double, Polynomial>> entries;
        entries.reserve(3 * members.size());
        for (int32_t const id : members) {
            std::array<double, 3> h;
            for (uint32_t k = 0; k < 3; ++k) {
                h[k] = bed[triangles[id][k]] - origin;
            }
            std::sort(h.begin(), h.end());
            double const lo = h[0], mid = h[1], hi = h[2], span = hi - lo;
            std::array<double, 3> q{}, linear{}, step{};
            if (mid > lo && hi > mid) {
                q = {1 / ((mid - lo) * span), -1 / ((mid - lo) * (hi - mid)), 1 / (span * (hi - mid))};
            } else if (mid == lo && hi > lo) {
                q = {-1 / (span * span), 0, 1 / (span * span)};
                linear[0] = 2 / span;
            } else if (hi == mid && hi > lo) {
                q = {1 / (span * span), 0, -1 / (span * span)};
                linear[2] = -2 / span;
            } else {
                step[0] = 1;
            }
            std::array<double, 4> const rates = {1, weights[id][0], weights[id][1], weights[id][2]};
            for (uint32_t k = 0; k < 3; ++k) {
                std::array<double, 4> const coefficient = {
                    q[k],
                    linear[k] - 2 * q[k] * h[k],
                    step[k] - linear[k] * h[k] + q[k] * h[k] * h[k],
                    -q[k] * h[k] * h[k] * h[k] / 3 + linear[k] * h[k] * h[k] / 2 - step[k] * h[k],
                };
                Polynomial event{};
                for (uint32_t r = 0; r < 4; ++r) {
                    for (uint32_t c = 0; c < 4; ++c) {
                        event[r][c] = coefficient[c] * area * rates[r];
                    }
                }
                entries.emplace_back(h[k], event);
            }
        }
        std::stable_sort(entries.begin(), entries.end(),
                         [](auto const& a, auto const& b) { return a.first < b.first; });
        Polynomial sum{};
        for (auto const& [height, coefficient] : entries) {
            events.push_back(height);
            for (uint32_t r = 0; r < 4; ++r) {
                for (uint32_t c = 0; c < 4; ++c) {
                    sum[r][c] += coefficient[r][c];
                }
            }
            prefix.push_back(sum);
        }
    }

    std::array<double, 5> at(double stage) const
    {
        double const z = stage - origin;
        auto const end = std::lower_bound(events.begin(), events.end(), z);
        if (end == events.begin()) {
            return {};
        }
        Polynomial const& c = prefix[static_cast<size_t>(end - events.begin() - 1)];
        std::array<double, 5> result{};
        result[0] = std::max(c[0][0] * z * z + c[0][1] * z + c[0][2], 0.0);
        result[1] = std::max(c[0][0] * z * z * z / 3 + c[0][1] * z * z / 2 + c[0][2] * z + c[0][3], 0.0);
        for (uint32_t r = 1; r < 4; ++r) {
            result[r + 1] = c[r][0] * z * z + c[r][1] * z + c[r][2];
        }
        return result;
    }

    std::array<double, 3> demandCoefficients(double z) const
    {
        auto const end = std::upper_bound(events.begin(), events.end(), z);
        if (end == events.begin()) {
            return {};
        }
        Polynomial const& c = prefix[static_cast<size_t>(end - events.begin() - 1)];
        return {c[1][0] - c[2][0] + c[3][0], c[1][1] - c[2][1] + c[3][1], c[1][2] - c[2][2] + c[3][2]};
    }

    std::pair<double, bool> stableStage(double supply, double bottom, double spill) const
    {
        std::vector<double> breaks = {bottom, spill};
        for (double const event : events) {
            double const height = event + origin;
            if (height >= bottom && height <= spill) {
                breaks.push_back(height);
            }
        }
        std::sort(breaks.begin(), breaks.end());
        breaks.erase(std::unique(breaks.begin(), breaks.end()), breaks.end());
        double previous = bottom;
        for (size_t i = 1; i < breaks.size(); ++i) {
            double const upper = breaks[i];
            auto const net = demandCoefficients((previous + upper) * .5 - origin);
            std::vector<double> points;
            if (net[0] != 0) {
                double const vertex = origin - net[1] / (2 * net[0]);
                if (previous < vertex && vertex < upper) {
                    points.push_back(vertex);
                }
            }
            points.push_back(upper);
            for (double const high : points) {
                auto const values = at(std::nextafter(high, std::numeric_limits<double>::infinity()));
                if (values[2] - values[3] + values[4] >= supply) {
                    double const zlo = previous - origin, zhi = high - origin;
                    auto const coefficients = demandCoefficients((zlo + zhi) * .5);
                    double const aa = coefficients[0], bb = coefficients[1], cc = coefficients[2] - supply;
                    std::vector<double> roots;
                    if (std::abs(aa) < 1e-30) {
                        if (bb > 0) {
                            roots.push_back(-cc / bb);
                        }
                    } else {
                        double const discriminant = bb * bb - 4 * aa * cc;
                        if (discriminant >= 0) {
                            roots = {(-bb - std::sqrt(discriminant)) / (2 * aa),
                            (-bb + std::sqrt(discriminant)) / (2 * aa)};
                        }
                    }
                    std::erase_if(roots,
                            [&](double r) { return r < zlo - 1e-7 || r > zhi + 1e-7 || 2 * aa * r + bb < 0; });
                    if (!roots.empty()) {
                        return {origin + *std::min_element(roots.begin(), roots.end()), false};
                    }
                    return {previous, true};
                }
                previous = high;
            }
        }
        return {spill, false};
    }
};

std::array<int32_t, 6> neighbours(int32_t i, int32_t n)
{
    int32_t const x = i % n, y = i / n;
    auto id = [n](int32_t yy, int32_t xx) { return ((yy + n) % n) * n + (xx + n) % n; };
    return {id(y - 1, x), id(y + 1, x), id(y, x - 1), id(y, x + 1), id(y - 1, x - 1), id(y + 1, x + 1)};
}

std::vector<uint8_t> marineMask(std::span<double const> bed, int32_t n)
{
    std::vector<int32_t> labels(bed.size(), -1);
    std::vector<int32_t> sizes;
    for (int32_t i = 0; i < static_cast<int32_t>(bed.size()); ++i) {
        if (bed[i] >= 0 || labels[i] >= 0) {
            continue;
        }
        int32_t const label = static_cast<int32_t>(sizes.size());
        std::vector<int32_t> queue = {i};
        labels[i] = label;
        for (size_t k = 0; k < queue.size(); ++k) {
            auto const near = neighbours(queue[k], n);
            for (uint32_t j = 0; j < 4; ++j) {
                int32_t const next = near[j];
                if (bed[next] < 0 && labels[next] < 0) {
                    labels[next] = label;
                    queue.push_back(next);
                }
            }
        }
        sizes.push_back(static_cast<int32_t>(queue.size()));
    }
    std::vector<uint8_t> ocean(bed.size());
    if (sizes.empty()) {
        return ocean;
    }
    int32_t const largest = static_cast<int32_t>(std::max_element(sizes.begin(), sizes.end()) - sizes.begin());
    std::vector<int32_t> queue;
    for (int32_t i = 0; i < static_cast<int32_t>(bed.size()); ++i) {
        if (labels[i] == largest) {
            ocean[i] = 1;
            queue.push_back(i);
        }
    }
    for (size_t k = 0; k < queue.size(); ++k) {
        for (int32_t const j : neighbours(queue[k], n)) {
            if (!ocean[j] && bed[j] < 0) {
                ocean[j] = 1;
                queue.push_back(j);
            }
        }
    }
    return ocean;
}

} // namespace

std::expected<V9WaterEquilibrium, V9WaterFailure>
solveV9WaterEquilibrium(std::span<double const> bed, uint32_t resolution, V9WaterRates const& rates, double domain_m)
{
    auto failure = [](V9WaterFailure::Kind kind, std::string message) {
        return std::unexpected(V9WaterFailure{kind, std::move(message)});
    };
    if (resolution < 2 || resolution > 4'096 || bed.size() != static_cast<size_t>(resolution) * resolution ||
        !std::isfinite(domain_m) || domain_m <= 0) {
        return failure(V9WaterFailure::Kind::InvalidInput, "finite square bed and positive domain required");
    }
    double const cell = domain_m / resolution, area = cell * cell / 2;
    if (!std::isfinite(cell) || cell <= 0 || !std::isfinite(area) || area <= 0) {
        return failure(V9WaterFailure::Kind::InvalidInput, "finite positive water cell area required");
    }
    for (auto const* field : {&rates.evaporation_m_s, &rates.precipitation_m_s, &rates.runoff_m_s}) {
        if (field->size() != bed.size() ||
            std::any_of(field->begin(), field->end(), [](double v) { return !std::isfinite(v) || v < 0; })) {
            return failure(V9WaterFailure::Kind::InvalidInput, "matching finite nonnegative registered rates required");
        }
    }
    if (std::any_of(bed.begin(), bed.end(), [](double h) { return !std::isfinite(h); })) {
        return failure(V9WaterFailure::Kind::InvalidInput, "finite bed required");
    }
    int32_t const n = static_cast<int32_t>(resolution), total = static_cast<int32_t>(bed.size());
    V9WaterEquilibrium result;
    result.resolution = resolution;
    result.domain_m = domain_m;
    result.hydraulic_bed_m.assign(bed.begin(), bed.end());
    result.ocean = marineMask(bed, n);
    auto const marine = std::find(result.ocean.begin(), result.ocean.end(), 1);
    if (marine == result.ocean.end()) {
        return failure(V9WaterFailure::Kind::MissingMarineOutlet, "equilibrium requires explicit marine outlet");
    }
    std::vector<int32_t> uf(total, -1), component(total), vertex_node(total), fixed_receiver(total, -1);
    int32_t const marine_root = static_cast<int32_t>(marine - result.ocean.begin());
    for (int32_t i = 0; i < total; ++i) {
        if (result.ocean[i]) {
            uf[i] = marine_root;
            fixed_receiver[i] = i;
        }
    }
    auto find = [&](int32_t i) {
        while (uf[i] != i) {
            uf[i] = uf[uf[i]];
            i = uf[i];
        }
        return i;
    };
    result.topology.emplace_back();
    result.topology[0].spill_height_m = std::numeric_limits<double>::infinity();
    auto node = [&](int32_t a, int32_t b, double h, int32_t ea, int32_t eb, int32_t pit) {
        int32_t const id = static_cast<int32_t>(result.topology.size());
        result.topology.push_back({a, b, -1, ea, eb, pit, -1, h, std::numeric_limits<double>::infinity()});
        if (a >= 0) {
            result.topology[a].parent = result.topology[b].parent = id;
            result.topology[a].spill_height_m = result.topology[b].spill_height_m = h;
        }
        return id;
    };
    std::vector<int32_t> order(total);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int32_t a, int32_t b) { return bed[a] < bed[b]; });
    for (int32_t const i : order) {
        if (result.ocean[i]) {
            continue;
        }
        std::vector<std::pair<int32_t, int32_t>> roots;
        for (int32_t const j : neighbours(i, n)) {
            if (uf[j] < 0) {
                continue;
            }
            int32_t const root = find(j);
            if (std::none_of(roots.begin(), roots.end(), [&](auto const& r) { return r.first == root; })) {
                roots.emplace_back(root, j);
            }
        }
        uf[i] = i;
        int32_t current;
        if (roots.empty()) {
            current = node(-1, -1, bed[i], -1, -1, i);
        } else {
            auto sorted = roots;
            std::stable_sort(sorted.begin(), sorted.end(),
                            [&](auto const& a, auto const& b) { return component[a.first] < component[b.first]; });
            current = component[sorted[0].first];
            int32_t const witness = sorted[0].second;
            for (size_t k = 1; k < sorted.size(); ++k) {
                int32_t const other = component[sorted[k].first];
                if (current == 0 || other == 0) {
                    int32_t const child = current == 0 ? other : current;
                    if (child > 0) {
                        result.topology[child].parent = 0;
                        result.topology[child].spill_height_m = std::max(bed[i], 0.0);
                        result.topology[child].spill_receiver_vertex = current == 0 ? witness : sorted[k].second;
                    }
                    current = 0;
                } else {
                    current = node(current, other, bed[i], witness, sorted[k].second, -1);
                }
            }
            for (auto const& [r, j] : sorted) {
                (void)j;
                uf[r] = i;
            }
        }
        component[i] = vertex_node[i] = current;
        if (current == 0) {
            for (auto const& [r, j] : roots) {
                if (component[r] == 0 && fixed_receiver[j] >= 0) {
                    fixed_receiver[i] = fixed_receiver[j];
                    break;
                }
            }
        }
    }
    result.vertex_receiver.resize(total);
    result.vertex_leaf.resize(total);
    result.vertex_outlet.assign(total, -1);
    for (int32_t const i : order) {
        int32_t receiver = i;
        for (int32_t const j : neighbours(i, n)) {
            if (bed[j] < bed[receiver] || (bed[j] == bed[receiver] && j < receiver)) {
                receiver = j;
            }
        }
        result.vertex_receiver[i] = receiver;
        result.vertex_leaf[i] = result.ocean[i] ? 0 : (receiver == i ? vertex_node[i] : result.vertex_leaf[receiver]);
        result.vertex_outlet[i] =
            result.ocean[i] ? i : (receiver == i ? fixed_receiver[i] : result.vertex_outlet[receiver]);
    }
    size_t const count = result.topology.size();
    std::vector<Rates> weights;
    std::vector<int32_t> triangle_leaf;
    std::vector<double> source(count);
    std::vector<std::vector<int32_t>> owned(count);
    for (int32_t y = 0; y < n; ++y) {
        for (int32_t x = 0; x < n; ++x) {
            int32_t const a = y * n + x, b = y * n + (x + 1) % n;
            int32_t const c = ((y + 1) % n) * n + (x + 1) % n, d = ((y + 1) % n) * n + x;
            for (Triangle const tri : {Triangle{a, b, c}, Triangle{a, c, d}}) {
                int32_t const tid = static_cast<int32_t>(weights.size());
                int32_t const lowest =
                    *std::min_element(tri.begin(), tri.end(), [&](int32_t u, int32_t v) { return bed[u] < bed[v]; });
                int32_t const leaf = result.vertex_leaf[lowest];
                Rates w{};
                for (int32_t const v : tri) {
                    w[0] += rates.evaporation_m_s[v];
                    w[1] += rates.precipitation_m_s[v];
                    w[2] += result.ocean[v] ? 0 : rates.runoff_m_s[v];
                }
                for (double& value : w) {
                    value /= 3;
                }
                result.triangle_vertex_ids.push_back(tri);
                result.triangle_source_vertex.push_back(lowest);
                result.triangle_runoff_source_m3_s.push_back(w[2] * area);
                triangle_leaf.push_back(leaf);
                weights.push_back(w);
                source[leaf] += w[2] * area;
                owned[leaf].push_back(tid);
            }
        }
    }
    for (size_t id = 1; id < count; ++id) {
        auto const& t = result.topology[id];
        if (t.left >= 0) {
            source[id] += source[t.left] + source[t.right];
            owned[id] = owned[t.left];
            owned[id].insert(owned[id].end(), owned[t.right].begin(), owned[t.right].end());
        }
    }
    std::vector<Hypsometry> curves;
    std::vector<int32_t> curve_id(count, -1);
    auto curve = [&](int32_t id) -> Hypsometry const& {
        if (curve_id[id] < 0) {
            curve_id[id] = static_cast<int32_t>(curves.size());
            curves.emplace_back(owned[id], result.triangle_vertex_ids, bed, weights, area);
        }
        return curves[curve_id[id]];
    };
    // A recursive update may allocate another curve; keep references stable.
    curves.reserve(count);
    std::vector<int32_t> low(count), high(count), roots;
    int32_t serial = 0;
    std::function<void(int32_t)> intervals = [&](int32_t id) {
        low[id] = serial;
        auto const& t = result.topology[id];
        if (t.left < 0) {
            ++serial;
        } else {
            intervals(t.left);
            intervals(t.right);
        }
        high[id] = serial;
    };
    for (int32_t id = 1; id < static_cast<int32_t>(count); ++id) {
        if (result.topology[id].parent == 0) {
            roots.push_back(id);
            intervals(id);
        }
    }
    using Injection = std::pair<int32_t, double>;
    std::vector<uint8_t> initialized(count), merged(count), full(count), films(count);
    std::vector<double> extra(count), outgoing(count), levels(count), sent_left(count), sent_right(count);
    std::function<void(int32_t, std::vector<Injection> const&)> update;
    update = [&](int32_t id, std::vector<Injection> const& additions) {
        bool const fresh = !initialized[id];
        initialized[id] = 1;
        double added = 0;
        for (auto const& [target, rate] : additions) {
            (void)target;
            added += rate;
        }
        extra[id] += added;
        if (!fresh && full[id]) {
            outgoing[id] += added;
            return;
        }
        auto const& t = result.topology[id];
        Hypsometry const& c = curve(id);
        double const supply = source[id] + extra[id];
        if (t.left >= 0 && !merged[id]) {
            int32_t const a = t.left, b = t.right;
            std::vector<Injection> ia, ib;
            for (auto const& injection : additions) {
                (low[a] <= low[injection.first] && low[injection.first] < high[a] ? ia : ib).push_back(injection);
            }
            if (fresh || !ia.empty()) {
                update(a, ia);
            }
            if (fresh || !ib.empty()) {
                update(b, ib);
            }
            if (full[a] && !full[b]) {
                double const delta = outgoing[a] - sent_left[id];
                if (delta > 0) {
                    sent_left[id] += delta;
                    update(b, {{result.vertex_leaf[t.entry_right], delta}});
                }
            } else if (full[b] && !full[a]) {
                double const delta = outgoing[b] - sent_right[id];
                if (delta > 0) {
                    sent_right[id] += delta;
                    update(a, {{result.vertex_leaf[t.entry_left], delta}});
                }
            }
            if (!(full[a] && full[b])) {
                return;
            }
            merged[id] = 1;
        }
        if (t.parent == 0 && t.spill_height_m <= 0) {
            levels[id] = 0;
            full[id] = 1;
            outgoing[id] = supply;
        } else if (supply <= 0) {
            levels[id] = t.merge_height_m;
            full[id] = c.at(t.spill_height_m)[1] <= 1e-9;
        } else {
            auto const [stage, film] = c.stableStage(supply, t.merge_height_m, t.spill_height_m);
            levels[id] = stage;
            films[id] = film;
            full[id] = stage >= t.spill_height_m - 1e-8 && !film;
            auto const at = c.at(stage);
            outgoing[id] = full[id] && !film ? std::max(supply - at[2] + at[3] - at[4], 0.0) : 0;
        }
    };
    std::vector<int32_t> root_of(count), root_target(count), target_leaf(count), incoming(count);
    for (int32_t id = 1; id < static_cast<int32_t>(count); ++id) {
        int32_t cursor = id;
        while (result.topology[cursor].parent > 0) {
            cursor = result.topology[cursor].parent;
        }
        root_of[id] = cursor;
    }
    std::priority_queue<int32_t, std::vector<int32_t>, std::greater<>> ready;
    for (int32_t const id : roots) {
        int32_t const receiver = result.topology[id].spill_receiver_vertex;
        target_leaf[id] = result.vertex_leaf[receiver];
        root_target[id] = root_of[target_leaf[id]];
        if (root_target[id] == id) {
            return failure(V9WaterFailure::Kind::CyclicReceivers, "root spill witness returns to its depression");
        }
        if (root_target[id]) {
            ++incoming[root_target[id]];
        }
    }
    for (int32_t const id : roots) {
        if (!incoming[id]) {
            ready.push(id);
        }
    }
    result.ocean_export_m3_s = source[0];
    size_t processed = 0;
    while (!ready.empty()) {
        int32_t const id = ready.top();
        ready.pop();
        ++processed;
        update(id, {});
        int32_t const target = root_target[id];
        if (outgoing[id] > 0) {
            result.spill_transfers.push_back({id, target, result.topology[id].spill_receiver_vertex, outgoing[id]});
            if (target) {
                update(target, {{target_leaf[id], outgoing[id]}});
            } else {
                result.ocean_export_m3_s += outgoing[id];
            }
        }
        if (target && --incoming[target] == 0) {
            ready.push(target);
        }
    }
    if (processed != roots.size()) {
        return failure(V9WaterFailure::Kind::CyclicReceivers, "cyclic native root spill receivers");
    }
    size_t const triangle_count = weights.size();
    result.triangle_owner.resize(triangle_count);
    result.triangle_basin_owner.resize(triangle_count);
    result.triangle_stage_m.resize(triangle_count);
    double evaporation = 0, precipitation = 0, replaced = 0;
    std::function<void(int32_t)> collect = [&](int32_t id) {
        auto const& t = result.topology[id];
        if (t.left >= 0 && !merged[id]) {
            collect(t.left);
            collect(t.right);
            if (sent_left[id] > 0) {
                result.spill_transfers.push_back({t.left, t.right, t.entry_right, sent_left[id]});
            }
            if (sent_right[id] > 0) {
                result.spill_transfers.push_back({t.right, t.left, t.entry_left, sent_right[id]});
            }
            return;
        }
        double const supply = source[id] + extra[id], z = levels[id];
        if (supply <= 0 || (t.parent == 0 && t.spill_height_m <= 0)) {
            return;
        }
        auto values = curve(id).at(z);
        if (films[id]) {
            auto const right = curve(id).at(std::nextafter(z, std::numeric_limits<double>::infinity()));
            double const demand_delta = right[2] - values[2] - right[3] + values[3] + right[4] - values[4];
            double const fraction =
                demand_delta > 0 ? std::clamp((supply - values[2] + values[3] - values[4]) / demand_delta, 0.0, 1.0)
                            : 0;
            for (size_t k = 0; k < values.size(); ++k) {
                values[k] += fraction * (right[k] - values[k]);
            }
        }
        for (int32_t const tri : owned[id]) {
            result.triangle_basin_owner[tri] = id;
            if (bed[result.triangle_source_vertex[tri]] < z) {
                result.triangle_owner[tri] = id;
                result.triangle_stage_m[tri] = z;
            }
        }
        double const overflow =
            z >= t.spill_height_m - 1e-8 && !films[id] ? std::max(supply - values[2] + values[3] - values[4], 0.0) : 0;
        result.bodies.push_back({id, z, t.spill_height_m, values[0], values[1], supply, values[2], values[3], values[4],
                            overflow, films[id] != 0});
        evaporation += values[2];
        precipitation += values[3];
        replaced += values[4];
    };
    for (int32_t const id : roots) {
        collect(id);
    }
    result.runoff_vertex_mean_m3_s =
        std::accumulate(rates.runoff_m_s.begin(), rates.runoff_m_s.end(), 0.0) / total * cell * cell;
    result.runoff_source_m3_s =
        std::accumulate(result.triangle_runoff_source_m3_s.begin(), result.triangle_runoff_source_m3_s.end(), 0.0);
    result.water_residual_m3_s =
        result.runoff_source_m3_s + precipitation - evaporation - replaced - result.ocean_export_m3_s;
    if (std::abs(result.water_residual_m3_s) > 1e-8 * std::max(result.runoff_source_m3_s, 1.0)) {
        return failure(V9WaterFailure::Kind::BudgetMismatch, "equilibrium global water budget does not close");
    }
    std::vector<double> vertex_stage(total, std::numeric_limits<double>::infinity());
    result.lake_id.resize(total);
    result.lake.resize(total);
    result.water_level_m.resize(total);
    result.water_depth_m.resize(total);
    result.vertex_basin_owner.resize(total);
    for (uint32_t k = 0; k < 3; ++k) {
        for (size_t tri = 0; tri < triangle_count; ++tri) {
            if (result.triangle_owner[tri] > 0) {
                int32_t const vertex = result.triangle_vertex_ids[tri][k];
                vertex_stage[vertex] = std::min(vertex_stage[vertex], result.triangle_stage_m[tri]);
            }
        }
    }
    for (uint32_t k = 0; k < 3; ++k) {
        for (size_t tri = 0; tri < triangle_count; ++tri) {
            int32_t const vertex = result.triangle_vertex_ids[tri][k];
            if (result.triangle_owner[tri] > 0 && result.triangle_stage_m[tri] == vertex_stage[vertex]) {
                result.lake_id[vertex] = result.triangle_owner[tri];
            }
        }
    }
    std::vector<uint8_t> active(count);
    for (auto const& body : result.bodies) {
        active[body.id] = 1;
        double const residual = body.source_m3_s + body.precipitation_m3_s - body.evaporation_m3_s -
                            body.replaced_runoff_m3_s - body.overflow_m3_s;
        if (std::abs(residual) > 1e-8 * std::max(body.source_m3_s, 1.0)) {
            return failure(V9WaterFailure::Kind::BudgetMismatch, "equilibrium body water budget does not close");
        }
    }
    for (int32_t i = 0; i < total; ++i) {
        bool const wet = vertex_stage[i] > bed[i] && std::isfinite(vertex_stage[i]) && !result.ocean[i];
        result.lake[i] = wet;
        result.water_level_m[i] = static_cast<float>(wet ? vertex_stage[i] : (result.ocean[i] ? 0 : bed[i]));
        result.water_depth_m[i] =
            static_cast<float>(wet ? vertex_stage[i] - bed[i] : (result.ocean[i] ? std::max(-bed[i], 0.0) : 0));
        int32_t cursor = result.vertex_leaf[i];
        while (cursor > 0 && !active[cursor]) {
            cursor = result.topology[cursor].parent;
        }
        result.vertex_basin_owner[i] = std::max(cursor, 0);
    }
    return result;
}

std::expected<std::vector<V9WaterSurfaceTriangle>, V9WaterFailure>
queryV9LakeSurface(V9WaterEquilibrium const& water, std::span<double const> bed, uint32_t resolution, double domain_m)
{
    if (resolution != water.resolution || domain_m != water.domain_m || bed.size() != water.hydraulic_bed_m.size() ||
        !std::equal(bed.begin(), bed.end(), water.hydraulic_bed_m.begin())) {
        return std::unexpected(
            V9WaterFailure{V9WaterFailure::Kind::RegionalReceiverClosureRequired,
                           "refined or partial bed requires complete catchments and receiving-path reconciliation"});
    }
    std::vector<V9WaterSurfaceTriangle> output;
    double const cell = domain_m / resolution;
    using Point = std::array<double, 4>;
    for (size_t id = 0; id < water.triangle_owner.size(); ++id) {
        if (!water.triangle_owner[id]) {
            continue;
        }
        size_t const x = (id / 2) % resolution, y = (id / 2) / resolution;
        std::array<std::array<double, 2>, 3> const xy =
            id % 2 == 0 ? std::array<std::array<double, 2>, 3>{{{double(x), double(y)},
                            {double(x + 1), double(y)},
                            {double(x + 1), double(y + 1)}}}
                        : std::array<std::array<double, 2>, 3>{
                            {{double(x), double(y)}, {double(x + 1), double(y + 1)}, {double(x), double(y + 1)}}};
        std::array<Point, 3> polygon;
        double const level = water.triangle_stage_m[id];
        for (uint32_t k = 0; k < 3; ++k) {
            double const height = bed[water.triangle_vertex_ids[id][k]];
            polygon[k] = {xy[k][0] * cell, xy[k][1] * cell, height, level - height};
        }
        std::vector<Point> clipped;
        for (uint32_t k = 0; k < 3; ++k) {
            Point const& a = polygon[k];
            Point const& b = polygon[(k + 1) % 3];
            if (a[3] > 0) {
                clipped.push_back(a);
            }
            if ((a[3] > 0) != (b[3] > 0)) {
                Point intersection;
                for (uint32_t j = 0; j < 4; ++j) {
                    intersection[j] = a[j] + (b[j] - a[j]) * a[3] / (a[3] - b[3]);
                }
                clipped.push_back(intersection);
            }
        }
        for (size_t k = 1; k + 1 < clipped.size(); ++k) {
            V9WaterSurfaceTriangle triangle{};
            triangle.owner = water.triangle_owner[id];
            triangle.parent_triangle = static_cast<int32_t>(id);
            for (uint32_t j = 0; j < 3; ++j) {
                Point const& p = clipped[j == 0 ? 0 : (j == 1 ? k : k + 1)];
                triangle.xyz_m[j] = {p[0], p[1], level};
            }
            output.push_back(triangle);
        }
    }
    return output;
}

} // namespace generation::detail
