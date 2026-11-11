#include "V21FinePlan.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <utility>

#pragma STDC FP_CONTRACT OFF

namespace generation::detail
{
namespace
{
constexpr double WORLD = 65'536.0;
constexpr double MINIMUM_SLOPE = 1e-4;
constexpr double FREEBOARD = .05;
constexpr double MAXIMUM_SUPPORT = 256.0;
constexpr size_t MAX_FINE_CHORDS = 200'000U;
constexpr size_t MAX_QUERY_POINTS = 1'048'576U;
constexpr size_t MAX_CACHE_BYTES = 268'435'456U;
constexpr size_t CALLBACK_BATCH = 65'536U;
constexpr double INF = std::numeric_limits<double>::infinity();

struct FineException final
{
    V21FineFailure failure;
};

[[noreturn]] void fail(V21FineFailureKind kind, std::string reason,
    std::vector<V21FineWitness> witnesses = {})
{
    throw FineException{{kind, std::move(reason), std::move(witnesses)}};
}

void checkCancellation(std::stop_token token)
{
    if (token.stop_requested())
    {
        fail(V21FineFailureKind::Cancelled, "fine component plan cancelled");
    }
}

double canonical(double value)
{
    double const wrapped = std::fmod(value, WORLD);
    return wrapped < 0.0 ? wrapped + WORLD : wrapped;
}

double displacement(double value)
{
    return canonical(value + WORLD / 2.0) - WORLD / 2.0;
}

V21FinePoint add(V21FinePoint const& a, V21FinePoint const& b, double scale = 1.0)
{
    return {a[0] + scale * b[0], a[1] + scale * b[1]};
}

double length(V21FinePoint const& p)
{
    return std::sqrt(p[0] * p[0] + p[1] * p[1]);
}

using PortKey = std::array<int64_t, 2U>;
PortKey portKey(V21FinePoint const& point)
{
    return {static_cast<int64_t>(std::nearbyint(canonical(point[0]) * 1e7)),
        static_cast<int64_t>(std::nearbyint(canonical(point[1]) * 1e7))};
}

struct Context final
{
    V21FineGeometry geometry;
    V21FineCallbacks callbacks;
};

std::vector<double> heights(V21FineCallbacks::Heights const& callback,
    std::span<V21FinePoint const> points, std::stop_token token)
{
    std::vector<double> output;
    output.reserve(points.size());
    for (size_t offset = 0U; offset < points.size(); offset += CALLBACK_BATCH)
    {
        checkCancellation(token);
        auto const batch = points.subspan(offset, std::min(CALLBACK_BATCH, points.size() - offset));
        auto values = callback(batch);
        if (values.size() != batch.size() || !std::all_of(values.begin(), values.end(),
                [](double value) { return std::isfinite(value); }))
        {
            fail(V21FineFailureKind::InvalidInput, "finite substrate/base values matching coordinates required");
        }
        output.insert(output.end(), values.begin(), values.end());
    }
    checkCancellation(token);
    return output;
}

std::vector<V21FineStanding> standing(Context const& context,
    std::span<V21FinePoint const> points, std::stop_token token)
{
    std::vector<V21FineStanding> output;
    output.reserve(points.size());
    for (size_t offset = 0U; offset < points.size(); offset += CALLBACK_BATCH)
    {
        checkCancellation(token);
        auto const batch = points.subspan(offset, std::min(CALLBACK_BATCH, points.size() - offset));
        auto values = context.callbacks.standing(batch);
        if (values.size() != batch.size() || !std::all_of(values.begin(), values.end(),
                [](auto const& value) { return std::isfinite(value.stage); }))
        {
            fail(V21FineFailureKind::InvalidInput, "finite standing authority matching coordinates required");
        }
        output.insert(output.end(), values.begin(), values.end());
    }
    return output;
}

double manningDepth(double discharge, double width, double slope, double bank_fraction)
{
    auto const capacity = [&](double depth) {
        double const area = width * (1.0 - bank_fraction) * depth;
        double const side = width * bank_fraction;
        double const perimeter = width * (1.0 - 2.0 * bank_fraction)
            + 2.0 * std::sqrt(depth * depth + side * side);
        return area * std::pow(area / perimeter, 2.0 / 3.0) * std::sqrt(slope) / .04;
    };
    if (capacity(10.0) < discharge)
    {
        fail(V21FineFailureKind::InvalidInput, "discharge exceeds bounded10m reference-section depth");
    }
    double lo = 0.0, hi = 10.0;
    for (uint32_t iteration = 0U; iteration < 70U; ++iteration)
    {
        double const middle = (lo + hi) / 2.0;
        if (capacity(middle) < discharge)
        {
            lo = middle;
        }
        else
        {
            hi = middle;
        }
    }
    return (lo + hi) / 2.0;
}

struct Component final
{
    std::string id;
    std::vector<size_t> coarse;
    size_t fine_count = 0U;
};

struct Index final
{
    std::map<int32_t, std::vector<size_t>> bins;
    explicit Index(std::span<V21FineChord const> chords)
    {
        for (size_t edge = 0U; edge < chords.size(); ++edge)
        {
            auto const& row = chords[edge];
            double const support = std::max(row.support_m, row.support_coarse_m);
            int32_t const x0 = static_cast<int32_t>(std::floor(
                (std::min(row.a[0], row.a[0] + row.delta[0]) - support) / 256.0));
            int32_t const x1 = static_cast<int32_t>(std::floor(
                (std::max(row.a[0], row.a[0] + row.delta[0]) + support) / 256.0));
            int32_t const y0 = static_cast<int32_t>(std::floor(
                (std::min(row.a[1], row.a[1] + row.delta[1]) - support) / 256.0));
            int32_t const y1 = static_cast<int32_t>(std::floor(
                (std::max(row.a[1], row.a[1] + row.delta[1]) + support) / 256.0));
            for (int32_t y = y0; y <= y1; ++y)
            {
                for (int32_t x = x0; x <= x1; ++x)
                {
                    int32_t const bx = (x % 256 + 256) % 256;
                    int32_t const by = (y % 256 + 256) % 256;
                    bins[by * 256 + bx].push_back(edge);
                }
            }
        }
        for (auto& [key, rows] : bins)
        {
            static_cast<void>(key);
            rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
        }
    }
};

std::vector<Component> components(Context const& context)
{
    auto const& chords = context.geometry.chords;
    if (chords.size() > 100'000U)
    {
        fail(V21FineFailureKind::LimitExceeded, "global coarse edge count exceeds100000");
    }
    std::map<PortKey, size_t> ports;
    for (auto const& chord : chords)
    {
        ports.emplace(portKey(chord.a), 0U);
        ports.emplace(portKey(add(chord.a, chord.delta)), 0U);
    }
    size_t next = 0U;
    for (auto& [key, value] : ports)
    {
        static_cast<void>(key);
        value = next++;
    }
    std::vector<size_t> parent(ports.size());
    std::iota(parent.begin(), parent.end(), 0U);
    auto const root = [&](size_t vertex) {
        while (parent[vertex] != vertex)
        {
            parent[vertex] = parent[parent[vertex]];
            vertex = parent[vertex];
        }
        return vertex;
    };
    for (auto const& chord : chords)
    {
        size_t const a = root(ports.at(portKey(chord.a)));
        size_t const b = root(ports.at(portKey(add(chord.a, chord.delta))));
        parent[std::max(a, b)] = std::min(a, b);
    }
    std::map<size_t, std::vector<size_t>> groups;
    for (size_t edge = 0U; edge < chords.size(); ++edge)
    {
        groups[root(ports.at(portKey(chords[edge].a)))].push_back(edge);
    }
    std::vector<Component> output;
    for (auto& [key, rows] : groups)
    {
        static_cast<void>(key);
        Component component;
        char id[32];
        std::snprintf(id, sizeof(id), "c%08zu", rows.front());
        component.id = id;
        component.coarse = std::move(rows);
        for (size_t edge : component.coarse)
        {
            component.fine_count += static_cast<size_t>(std::max(1.0, std::ceil(length(chords[edge].delta) / 2.0)));
        }
        if (component.fine_count > MAX_FINE_CHORDS)
        {
            fail(V21FineFailureKind::LimitExceeded, "component fine chords exceed200000");
        }
        output.push_back(std::move(component));
    }
    std::sort(output.begin(), output.end(), [](auto const& a, auto const& b) {
        return a.coarse.front() < b.coarse.front();
    });
    return output;
}

struct WorkChord final
{
    V21FineChord row;
    V21FinePoint normal{};
    std::array<std::array<double, 3U>, 2U> terrain{};
    double end_stage = 0.0;
};

std::vector<V21FinePoint> bankEndpoints(std::span<WorkChord const> work)
{
    std::vector<V21FinePoint> points;
    points.reserve(work.size() * 6U);
    for (auto const& chord : work)
    {
        for (uint32_t endpoint = 0U; endpoint < 2U; ++endpoint)
        {
            auto const point = add(chord.row.a, chord.row.delta, endpoint);
            for (double side : {-1.0, 0.0, 1.0})
            {
                points.push_back(add(point, chord.normal, chord.row.halfwidth * side));
            }
        }
    }
    return points;
}

void assignTerrain(std::span<WorkChord> work, std::span<double const> values)
{
    for (size_t edge = 0U; edge < work.size(); ++edge)
    {
        for (size_t endpoint = 0U; endpoint < 2U; ++endpoint)
        {
            for (size_t side = 0U; side < 3U; ++side)
            {
                work[edge].terrain[endpoint][side] = values[6U * edge + 3U * endpoint + side];
            }
        }
    }
}

std::vector<WorkChord> prepareChords(Context const& context, Component const& component,
    std::span<uint64_t const> prefix, std::stop_token token)
{
    std::vector<WorkChord> work;
    work.reserve(component.fine_count);
    for (size_t edge : component.coarse)
    {
        auto const& coarse = context.geometry.chords[edge];
        size_t const count = static_cast<size_t>(std::max(1.0, std::ceil(length(coarse.delta) / 2.0)));
        for (size_t i = 0U; i < count; ++i)
        {
            WorkChord chord;
            chord.row = coarse;
            double const fraction = static_cast<double>(i) / count;
            chord.row.a = add(coarse.a, coarse.delta, fraction);
            chord.row.delta = {coarse.delta[0] / count, coarse.delta[1] / count};
            chord.row.global_order = prefix[edge] + i;
            chord.end_stage = coarse.water_stage0 + (fraction + 1.0 / count)
                * (coarse.water_stage1 - coarse.water_stage0);
            double const norm = length(chord.row.delta);
            if (!(norm > 0.0 && norm <= 2.01))
            {
                fail(V21FineFailureKind::InvalidInput, "fine planner requires actual at-most2m course chords");
            }
            chord.normal = {-chord.row.delta[1] / norm, chord.row.delta[0] / norm};
            work.push_back(chord);
        }
    }
    assignTerrain(work, heights(context.callbacks.substrate, bankEndpoints(work), token));
    return work;
}

struct Graph final
{
    std::vector<V21FinePoint> points;
    std::vector<size_t> u, v, order;
    std::vector<std::vector<size_t>> outgoing;
    std::vector<double> terrain, chord_length;
    std::map<size_t, double> fixed;
};

Graph graph(std::span<WorkChord const> work, std::map<PortKey, double> const& forced)
{
    Graph output;
    struct Port final { size_t index = 0U; V21FinePoint point{}; };
    std::map<PortKey, Port> ports;
    for (auto const& chord : work)
    {
        for (auto point : {chord.row.a, add(chord.row.a, chord.row.delta)})
        {
            point = {canonical(point[0]), canonical(point[1])};
            ports.try_emplace(portKey(point), Port{0U, point});
        }
    }
    for (auto& [key, port] : ports)
    {
        static_cast<void>(key);
        port.index = output.points.size();
        output.points.push_back(port.point);
    }
    output.outgoing.resize(ports.size());
    output.terrain.assign(ports.size(), INF);
    std::vector<size_t> indegree(ports.size(), 0U);
    for (size_t edge = 0U; edge < work.size(); ++edge)
    {
        auto const& chord = work[edge];
        size_t const u = ports.at(portKey(chord.row.a)).index;
        size_t const v = ports.at(portKey(add(chord.row.a, chord.row.delta))).index;
        output.u.push_back(u);
        output.v.push_back(v);
        output.chord_length.push_back(length(chord.row.delta));
        output.outgoing[u].push_back(edge);
        ++indegree[v];
        output.terrain[u] = std::min(output.terrain[u], *std::min_element(
            chord.terrain[0].begin(), chord.terrain[0].end()));
        output.terrain[v] = std::min(output.terrain[v], *std::min_element(
            chord.terrain[1].begin(), chord.terrain[1].end()));
    }
    std::deque<size_t> queue;
    for (size_t node = 0U; node < ports.size(); ++node)
    {
        if (indegree[node] == 0U)
        {
            queue.push_back(node);
        }
        auto const fixed = forced.find(portKey(output.points[node]));
        if (fixed != forced.end())
        {
            output.fixed[node] = fixed->second;
        }
    }
    while (!queue.empty())
    {
        size_t const node = queue.front();
        queue.pop_front();
        output.order.push_back(node);
        for (size_t edge : output.outgoing[node])
        {
            if (--indegree[output.v[edge]] == 0U)
            {
                queue.push_back(output.v[edge]);
            }
        }
    }
    if (output.order.size() != ports.size())
    {
        fail(V21FineFailureKind::CyclicGraph, "cyclic fine river graph");
    }
    for (size_t edge = 0U; edge < work.size(); ++edge)
    {
        size_t const node = output.v[edge];
        if (work[edge].row.shore_truncated && output.outgoing[node].empty())
        {
            double const stage = work[edge].end_stage;
            auto const existing = output.fixed.find(node);
            if (existing != output.fixed.end() && std::abs(existing->second - stage) > 1e-6)
            {
                fail(V21FineFailureKind::ConflictingStandingOutlet, "conflicting actual standing outlets",
                    {V21FineWitness{.node = static_cast<int64_t>(node)}});
            }
            output.fixed[node] = stage;
        }
    }
    return output;
}

double drop(Graph const& graph, std::span<double const> depth, size_t edge)
{
    size_t const u = graph.u[edge], v = graph.v[edge];
    return (graph.fixed.contains(v) ? 0.0 : MINIMUM_SLOPE * graph.chord_length[edge])
        + std::max(0.0, depth[u] - depth[v]);
}

void solveStages(Context const& context, std::span<WorkChord> work, Graph const& g,
    std::stop_token token)
{
    std::map<int32_t, size_t> reach_ids;
    for (auto const& chord : work)
    {
        reach_ids.emplace(chord.row.start, 0U);
    }
    size_t next = 0U;
    for (auto& [key, value] : reach_ids)
    {
        static_cast<void>(key);
        value = next++;
    }
    size_t const reaches = reach_ids.size(), nodes = g.points.size();
    std::vector<size_t> reach(work.size()), first(reaches, work.size()), last(reaches, 0U);
    std::vector<double> reach_length(reaches, 0.0), normal_depth(reaches);
    for (size_t edge = 0U; edge < work.size(); ++edge)
    {
        size_t const id = reach_ids.at(work[edge].row.start);
        reach[edge] = id;
        first[id] = std::min(first[id], edge);
        last[id] = std::max(last[id], edge);
        reach_length[id] += g.chord_length[edge];
    }
    for (size_t id = 0U; id < reaches; ++id)
    {
        double const slope = std::max((work[first[id]].terrain[0][1]
            - work[last[id]].terrain[1][1]) / reach_length[id], MINIMUM_SLOPE);
        normal_depth[id] = manningDepth(work[first[id]].row.discharge_m3_s,
            2.0 * work[first[id]].row.halfwidth, slope, context.geometry.bank_fraction);
    }
    std::vector<double> depth(nodes), stage(nodes), bed(nodes), target(reaches);
    std::vector<V21FineWitness> failures;
    for (uint32_t iteration = 0U; iteration < 64U; ++iteration)
    {
        checkCancellation(token);
        std::fill(depth.begin(), depth.end(), 0.0);
        for (size_t edge = 0U; edge < work.size(); ++edge)
        {
            depth[g.u[edge]] = std::max(depth[g.u[edge]], normal_depth[reach[edge]]);
            depth[g.v[edge]] = std::max(depth[g.v[edge]], normal_depth[reach[edge]]);
        }
        for (auto const& [node, value] : g.fixed)
        {
            depth[node] = std::max(depth[node], value - g.terrain[node] + FREEBOARD);
        }
        std::vector<double> lower(nodes, -INF);
        for (auto const& [node, value] : g.fixed)
        {
            lower[node] = value;
        }
        for (auto node = g.order.rbegin(); node != g.order.rend(); ++node)
        {
            for (size_t edge : g.outgoing[*node])
            {
                lower[*node] = std::max(lower[*node], lower[g.v[edge]] + drop(g, depth, edge));
            }
        }
        for (size_t node = 0U; node < nodes; ++node)
        {
            stage[node] = std::min(g.terrain[node], std::max(g.terrain[node] - FREEBOARD, lower[node]));
        }
        for (auto const& [node, value] : g.fixed)
        {
            stage[node] = value;
        }
        failures.clear();
        for (size_t node : g.order)
        {
            for (size_t edge : g.outgoing[node])
            {
                size_t const right = g.v[edge];
                double const ceiling = stage[node] - drop(g, depth, edge);
                auto const fixed = g.fixed.find(right);
                if (fixed != g.fixed.end())
                {
                    if (ceiling < fixed->second - 1e-7)
                    {
                        failures.push_back({.edge = static_cast<int64_t>(edge),
                            .node = static_cast<int64_t>(right), .required_stage = fixed->second,
                            .maximum_cut_only_stage = ceiling, .deficit = fixed->second - ceiling});
                    }
                }
                else
                {
                    stage[right] = std::min(stage[right], ceiling);
                }
            }
        }
        for (size_t node = 0U; node < nodes; ++node)
        {
            bed[node] = stage[node] - depth[node];
        }
        double convergence = 0.0;
        for (size_t id = 0U; id < reaches; ++id)
        {
            double const slope = std::max((bed[g.u[first[id]]] - bed[g.v[last[id]]])
                / reach_length[id], MINIMUM_SLOPE);
            target[id] = manningDepth(work[first[id]].row.discharge_m3_s,
                2.0 * work[first[id]].row.halfwidth, slope, context.geometry.bank_fraction);
            convergence = std::max(convergence, std::abs(target[id] - normal_depth[id]));
        }
        if (iteration == 63U)
        {
            if (convergence >= 1e-8)
            {
                fail(V21FineFailureKind::IterationScheduleNotConverged,
                    "fixed 64-iteration schedule did not converge");
            }
            break;
        }
        for (size_t id = 0U; id < reaches; ++id)
        {
            normal_depth[id] = (normal_depth[id] + target[id]) * .5;
        }
    }
    if (!failures.empty() && std::all_of(failures.begin(), failures.end(),
            [](auto const& row) { return row.deficit <= 1e-6; }))
    {
        auto projected = stage;
        bool valid = true;
        for (auto node = g.order.rbegin(); node != g.order.rend(); ++node)
        {
            for (size_t edge : g.outgoing[*node])
            {
                double const required = projected[g.v[edge]] + drop(g, depth, edge);
                if (required > projected[*node])
                {
                    if (g.fixed.contains(*node) || required > g.terrain[*node] + 1e-6)
                    {
                        valid = false;
                        break;
                    }
                    projected[*node] = required;
                }
            }
        }
        if (valid)
        {
            stage = std::move(projected);
            for (size_t node = 0U; node < nodes; ++node)
            {
                bed[node] = stage[node] - depth[node];
            }
            failures.clear();
        }
    }
    if (!failures.empty())
    {
        for (auto& witness : failures)
        {
            size_t const edge = static_cast<size_t>(witness.edge);
            size_t const left = g.u[edge], right = g.v[edge];
            witness.source_vertex = work[edge].row.start;
            witness.xy0 = g.points[left];
            witness.xy1 = g.points[right];
            witness.substrate0 = g.terrain[left];
            witness.substrate1 = g.terrain[right];
            witness.depth0 = depth[left];
            witness.depth1 = depth[right];
            witness.stage0 = stage[left];
            witness.stage1 = stage[right];
            witness.last_chord_m = g.chord_length[edge];
        }
        fail(V21FineFailureKind::ImmutableStandingOutletInfeasible,
            "fine depression below immutable standing outlet", std::move(failures));
    }
    for (size_t edge = 0U; edge < work.size(); ++edge)
    {
        auto& row = work[edge].row;
        row.grade0 = bed[g.u[edge]];
        row.grade1 = bed[g.v[edge]];
        row.water_stage0 = stage[g.u[edge]];
        row.water_stage1 = stage[g.v[edge]];
        size_t const id = reach[edge];
        row.normal_slope = std::max((bed[g.u[first[id]]] - bed[g.v[last[id]]])
            / reach_length[id], MINIMUM_SLOPE);
        row.normal_depth = manningDepth(work[first[id]].row.discharge_m3_s,
            2.0 * work[first[id]].row.halfwidth, row.normal_slope, context.geometry.bank_fraction);
    }
}

} // namespace

namespace
{
void reconcileShore(Context const& context, std::vector<WorkChord>& work,
    std::map<PortKey, double>& forced, std::stop_token token)
{
    std::vector<V21FinePoint> endpoints;
    endpoints.reserve(work.size() * 2U);
    std::map<int32_t, std::vector<size_t>> groups;
    for (size_t edge = 0U; edge < work.size(); ++edge)
    {
        endpoints.push_back(work[edge].row.a);
        endpoints.push_back(add(work[edge].row.a, work[edge].row.delta));
        if (work[edge].row.shore_truncated)
        {
            groups.try_emplace(work[edge].row.start);
        }
    }
    for (size_t edge = 0U; edge < work.size(); ++edge)
    {
        auto const found = groups.find(work[edge].row.start);
        if (found != groups.end())
        {
            found->second.push_back(edge);
        }
    }
    auto const owners = standing(context, endpoints, token);
    std::vector<uint8_t> keep(work.size(), 1U);
    struct Bracket final { size_t edge; double stage; int32_t owner; double lo = 0.0, hi = 1.0; };
    std::vector<Bracket> brackets;
    for (auto const& [source, edges] : groups)
    {
        static_cast<void>(source);
        size_t const last = edges.back();
        double const stage = work[last].end_stage;
        int32_t const body = owners[2U * last + 1U].owner;
        std::optional<std::pair<size_t, size_t>> hit;
        for (size_t row = 0U; row < edges.size() && !hit; ++row)
        {
            size_t const edge = edges[row];
            for (size_t side = 0U; side < 2U; ++side)
            {
                if (*std::min_element(work[edge].terrain[side].begin(),
                        work[edge].terrain[side].end()) <= stage
                    && owners[2U * edge + side].owner == body)
                {
                    hit = {row, side};
                    break;
                }
            }
        }
        if (!hit)
        {
            continue;
        }
        auto const [row, side] = *hit;
        size_t edge = edges[row];
        if (side == 0U && row == 0U)
        {
            for (size_t selected : edges)
            {
                keep[selected] = 0U;
            }
            forced[portKey(work[edge].row.a)] = stage;
            continue;
        }
        if (side == 0U)
        {
            edge = edges[row - 1U];
        }
        for (size_t selected : edges)
        {
            if (selected > edge)
            {
                keep[selected] = 0U;
            }
        }
        brackets.push_back({edge, stage, body});
    }
    for (uint32_t iteration = 0U; iteration < 12U && !brackets.empty(); ++iteration)
    {
        std::vector<V21FinePoint> points, bank_points;
        points.reserve(brackets.size());
        bank_points.reserve(brackets.size() * 3U);
        for (auto const& bracket : brackets)
        {
            auto const& chord = work[bracket.edge];
            auto const point = add(chord.row.a, chord.row.delta, (bracket.lo + bracket.hi) * .5);
            points.push_back(point);
            for (double side : {-1.0, 0.0, 1.0})
            {
                bank_points.push_back(add(point, chord.normal, chord.row.halfwidth * side));
            }
        }
        auto const terrain = heights(context.callbacks.substrate, bank_points, token);
        auto const authority = standing(context, points, token);
        for (size_t i = 0U; i < brackets.size(); ++i)
        {
            auto& bracket = brackets[i];
            double const minimum = std::min({terrain[3U * i], terrain[3U * i + 1U], terrain[3U * i + 2U]});
            double const middle = (bracket.lo + bracket.hi) * .5;
            if (minimum <= bracket.stage && authority[i].owner == bracket.owner)
            {
                bracket.hi = middle;
            }
            else
            {
                bracket.lo = middle;
            }
        }
    }
    for (auto const& bracket : brackets)
    {
        auto& chord = work[bracket.edge];
        chord.row.delta[0] *= bracket.lo;
        chord.row.delta[1] *= bracket.lo;
        chord.end_stage = bracket.stage;
        auto values = heights(context.callbacks.substrate, bankEndpoints(std::span{&chord, 1U}), token);
        assignTerrain(std::span{&chord, 1U}, values);
    }
    size_t output = 0U;
    for (size_t i = 0U; i < work.size(); ++i)
    {
        if (keep[i])
        {
            work[output++] = std::move(work[i]);
        }
    }
    work.resize(output);
    if (work.empty())
    {
        fail(V21FineFailureKind::IterationScheduleNotConverged,
            "planner did not execute all64 fixed hydraulic updates after all reaches submerged");
    }
}

void bankSupport(Context const& context, std::span<WorkChord> work, std::stop_token token)
{
    constexpr std::array<double, 9U> DISTANCES{0.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0, 128.0, 256.0};
    // The full frozen first-crossing calculation has the same results as the
    // adaptive FastPlan sampler. Chunk requests to bound temporary storage.
    for (size_t offset = 0U; offset < work.size(); offset += 2048U)
    {
        auto batch = work.subspan(offset, std::min(size_t{2048U}, work.size() - offset));
        std::vector<V21FinePoint> samples;
        samples.reserve(batch.size() * 18U);
        for (auto const& chord : batch)
        {
            auto const midpoint = add(chord.row.a, chord.row.delta, .5);
            for (double side : {-1.0, 1.0})
            {
                for (double distance : DISTANCES)
                {
                    samples.push_back(add(midpoint, chord.normal, (chord.row.halfwidth + distance) * side));
                }
            }
        }
        auto const terrain = heights(context.callbacks.substrate, samples, token);
        auto const base = heights(context.callbacks.base, samples, token);
        auto const authority = standing(context, samples, token);
        for (size_t edge = 0U; edge < batch.size(); ++edge)
        {
            auto& row = batch[edge].row;
            double const stage = (row.water_stage0 + row.water_stage1) * .5;
            std::array<double, 2U> slopes{}, coarse_slopes{};
            std::array<size_t, 2U> first{}, coarse_first{};
            for (size_t side = 0U; side < 2U; ++side)
            {
                size_t const begin = edge * 18U + side * 9U;
                double const incision = std::max(terrain[begin] - stage, 0.0);
                double const desired = incision / context.geometry.repose_slope + 2.0;
                if (desired > MAXIMUM_SUPPORT)
                {
                    fail(V21FineFailureKind::BankSupportInfeasible,
                        "incision requires bank support beyond bounded plan",
                        {V21FineWitness{.edge = static_cast<int64_t>(offset + edge),
                            .side = static_cast<int32_t>(side), .deficit = incision,
                            .required_support_m = desired}});
                }
                size_t const anchor = static_cast<size_t>(
                    std::lower_bound(DISTANCES.begin(), DISTANCES.end(), desired) - DISTANCES.begin());
                slopes[side] = std::max(context.geometry.repose_slope,
                    (terrain[begin + anchor] - stage) / DISTANCES[anchor]);
                first[side] = DISTANCES.size();
                for (size_t d = 0U; d < DISTANCES.size(); ++d)
                {
                    if (terrain[begin + d] <= stage + slopes[side] * DISTANCES[d])
                    {
                        first[side] = d;
                        break;
                    }
                }
                if (first[side] == DISTANCES.size())
                {
                    fail(V21FineFailureKind::BankSupportInfeasible,
                        "bank does not meet actual terrain within bounded support",
                        {V21FineWitness{.edge = static_cast<int64_t>(offset + edge),
                            .side = static_cast<int32_t>(side), .required_support_m = MAXIMUM_SUPPORT}});
                }
                auto const upper = [&](size_t d) {
                    size_t const i = begin + d;
                    return base[i] + .25 * std::abs(base[i] - authority[i].stage);
                };
                coarse_slopes[side] = std::max(slopes[side], (upper(8U) - stage) / MAXIMUM_SUPPORT);
                coarse_first[side] = 0U;
                for (size_t d = 0U; d < DISTANCES.size(); ++d)
                {
                    if (upper(d) <= stage + coarse_slopes[side] * DISTANCES[d] + 1e-9)
                    {
                        coarse_first[side] = d;
                        break;
                    }
                }
            }
            row.bank_slope_left = slopes[0U];
            row.bank_slope_right = slopes[1U];
            row.coarse_bank_slope_left = coarse_slopes[0U];
            row.coarse_bank_slope_right = coarse_slopes[1U];
            row.support_m = std::min(row.halfwidth + DISTANCES[std::max(first[0U], first[1U])] + 2.0,
                row.halfwidth + MAXIMUM_SUPPORT);
            row.support_coarse_m = std::min(
                row.halfwidth + DISTANCES[std::max(coarse_first[0U], coarse_first[1U])] + 2.0,
                row.halfwidth + MAXIMUM_SUPPORT);
        }
    }
}

void contractZeroSpanShore(std::vector<WorkChord>& work,
    std::map<PortKey, double>& forced, std::stop_token token)
{
    std::vector<uint8_t> remove(work.size(), 0U);
    bool found_contact = false;
    for (size_t edge = 0U; edge < work.size(); ++edge)
    {
        if ((edge % CALLBACK_BATCH) == 0U)
        {
            checkCancellation(token);
        }
        auto const& row = work[edge].row;
        if (row.shore_truncated && portKey(row.a) == portKey(add(row.a, row.delta)))
        {
            remove[edge] = 1U;
            found_contact = true;
        }
    }
    if (!found_contact)
    {
        return;
    }

    std::map<PortKey, bool> outgoing;
    for (size_t edge = 0U; edge < work.size(); ++edge)
    {
        if ((edge % CALLBACK_BATCH) == 0U)
        {
            checkCancellation(token);
        }
        if (!remove[edge])
        {
            outgoing.emplace(portKey(work[edge].row.a), true);
        }
    }
    std::map<PortKey, std::vector<size_t>> terminal_contacts;
    for (size_t edge = 0U; edge < work.size(); ++edge)
    {
        if ((edge % CALLBACK_BATCH) == 0U)
        {
            checkCancellation(token);
        }
        if (remove[edge])
        {
            auto const key = portKey(work[edge].row.a);
            if (!outgoing.contains(key))
            {
                terminal_contacts[key].push_back(edge);
            }
        }
    }
    auto witness = [&](size_t edge, double stage0, double stage1) {
        auto const& row = work[edge].row;
        return V21FineWitness{
            .edge = static_cast<int64_t>(edge),
            .source_vertex = row.start,
            .xy0 = row.a,
            .xy1 = add(row.a, row.delta),
            .stage0 = stage0,
            .stage1 = stage1,
        };
    };
    for (auto const& [key, edges] : terminal_contacts)
    {
        auto const [minimum, maximum] = std::minmax_element(edges.begin(), edges.end(),
            [&](size_t left, size_t right) { return work[left].end_stage < work[right].end_stage; });
        double const minimum_stage = work[*minimum].end_stage;
        double const maximum_stage = work[*maximum].end_stage;
        if (maximum_stage - minimum_stage > 1e-6)
        {
            fail(V21FineFailureKind::ConflictingStandingOutlet, "conflicting actual standing outlets",
                {witness(*minimum, minimum_stage, maximum_stage),
                    witness(*maximum, minimum_stage, maximum_stage)});
        }
        size_t const selected = edges.back();
        double const stage = work[selected].end_stage;
        auto const existing = forced.find(key);
        if (existing != forced.end() && std::abs(existing->second - stage) > 1e-6)
        {
            fail(V21FineFailureKind::ConflictingStandingOutlet, "conflicting actual standing outlets",
                {witness(selected, existing->second, stage)});
        }
        if (existing == forced.end())
        {
            forced.emplace(key, stage);
        }
    }

    size_t output = 0U;
    for (size_t edge = 0U; edge < work.size(); ++edge)
    {
        if ((edge % CALLBACK_BATCH) == 0U)
        {
            checkCancellation(token);
        }
        if (!remove[edge])
        {
            if (output != edge)
            {
                work[output] = std::move(work[edge]);
            }
            ++output;
        }
    }
    work.resize(output);
}

std::vector<V21FineChord> buildComponent(Context const& context, Component const& component,
    std::span<uint64_t const> prefix, std::stop_token token)
{
    auto work = prepareChords(context, component, prefix, token);
    std::map<PortKey, double> forced;
    reconcileShore(context, work, forced, token);
    contractZeroSpanShore(work, forced, token);
    Graph const topology = graph(work, forced);
    solveStages(context, work, topology, token);
    bankSupport(context, work, token);
    std::vector<V21FineChord> output;
    output.reserve(work.size());
    for (auto& chord : work)
    {
        output.push_back(std::move(chord.row));
    }
    return output;
}

std::pair<double, double> circularInterval(double origin, double spacing, uint32_t count)
{
    std::vector<double> values(count);
    for (uint32_t i = 0U; i < count; ++i)
    {
        values[i] = canonical(origin + i * spacing);
    }
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    if (values.size() == 1U)
    {
        return {values.front(), values.front()};
    }
    size_t gap = 0U;
    double maximum = -1.0;
    for (size_t i = 0U; i < values.size(); ++i)
    {
        double const next = i + 1U == values.size() ? values.front() + WORLD : values[i + 1U];
        if (next - values[i] > maximum)
        {
            maximum = next - values[i];
            gap = i;
        }
    }
    return {values[(gap + 1U) % values.size()], values[gap] + (gap + 1U < values.size() ? WORLD : 0.0)};
}

bool axisIntersects(double minimum, double maximum, std::pair<double, double> interval)
{
    auto const [lo, hi] = interval;
    if (hi - lo >= WORLD - 1e-7)
    {
        return true;
    }
    double const shift = std::nearbyint(((minimum + maximum) * .5 - (lo + hi) * .5) / WORLD) * WORLD;
    for (double image : {shift - WORLD, shift, shift + WORLD})
    {
        if (minimum <= hi + image && maximum >= lo + image)
        {
            return true;
        }
    }
    return false;
}

void validate(Context const& context)
{
    auto const& g = context.geometry;
    auto const& c = context.callbacks;
    if (g.immutable_identity.empty() || g.vertex_owner.size() != 256U * 256U
        || !c.substrate || !c.base || !c.standing || !c.protect
        || !std::isfinite(g.bank_fraction) || !(g.bank_fraction > 0.0 && g.bank_fraction <= .5)
        || !std::isfinite(g.repose_slope) || g.repose_slope <= 0.0)
    {
        fail(V21FineFailureKind::InvalidInput, "complete immutable V21 fine geometry and native callbacks required");
    }
    for (auto const& chord : g.chords)
    {
        double const chord_length = length(chord.delta);
        if (!std::isfinite(chord.a[0]) || !std::isfinite(chord.a[1])
            || !std::isfinite(chord_length) || chord_length <= 0.0 || chord_length > WORLD
            || !std::isfinite(chord.halfwidth) || chord.halfwidth <= 0.0 || chord.halfwidth >= WORLD / 2.0
            || !std::isfinite(chord.water_stage0) || !std::isfinite(chord.water_stage1)
            || !std::isfinite(chord.discharge_m3_s) || chord.discharge_m3_s < 0.0
            || chord.start < 0 || chord.target < 0
            || static_cast<size_t>(chord.start) >= g.vertex_owner.size()
            || static_cast<size_t>(chord.target) >= g.vertex_owner.size())
        {
            fail(V21FineFailureKind::InvalidInput, "malformed immutable V21 coarse course chord");
        }
    }
}

V21FineFailure callbackFailure(std::exception const& error)
{
    return {V21FineFailureKind::CallbackFailure,
        std::string{"V21 fine native callback failed: "} + error.what(), {}};
}
} // namespace

struct V21FinePlan::Impl final
{
    std::shared_ptr<Context const> context;
    std::vector<V21FineChord> chords;
    std::vector<std::string> component_ids;
    Index index;
    Impl(std::shared_ptr<Context const> input, std::vector<V21FineChord> rows,
        std::vector<std::string> ids)
        : context{std::move(input)}, chords{std::move(rows)}, component_ids{std::move(ids)}, index{chords} {}
};

struct V21FineProvider::Impl final
{
    std::shared_ptr<Context const> context;
    std::vector<Component> components;
    std::map<int32_t, std::vector<size_t>> component_bins;
    std::vector<uint64_t> prefix;
    std::optional<V21FineFailure> startup_failure;
    struct Record final
    {
        std::vector<V21FineChord> rows;
        uint64_t touched = 0U;
    };
    mutable std::map<size_t, Record> cache;
    mutable size_t cache_bytes = 0U;
    mutable uint64_t clock = 0U;
    mutable std::mutex mutex;
};

V21FinePlan::V21FinePlan(std::shared_ptr<Impl const> impl) : m_impl{std::move(impl)} {}
std::span<V21FineChord const> V21FinePlan::chords() const noexcept { return m_impl->chords; }
std::span<std::string const> V21FinePlan::connectedComponentIds() const noexcept { return m_impl->component_ids; }
std::string const& V21FinePlan::sourceIdentity() const noexcept
{
    return m_impl->context->geometry.immutable_identity;
}

V21FineProvider::V21FineProvider(V21FineGeometry geometry, V21FineCallbacks callbacks)
    : m_impl{std::make_unique<Impl>()}
{
    m_impl->context = std::make_shared<Context const>(Context{std::move(geometry), std::move(callbacks)});
    try
    {
        validate(*m_impl->context);
        m_impl->components = components(*m_impl->context);
        // The envelope matches the exact region predicate below. Bin candidates
        // only prune impossible components; selection still uses that predicate.
        for (size_t component = 0U; component < m_impl->components.size(); ++component)
        {
            std::vector<V21FineChord> envelopes;
            for (size_t edge : m_impl->components[component].coarse)
            {
                auto row = m_impl->context->geometry.chords[edge];
                row.support_m = row.halfwidth + MAXIMUM_SUPPORT;
                row.support_coarse_m = row.support_m;
                envelopes.push_back(row);
            }
            Index const index{envelopes};
            for (auto const& [bin, edges] : index.bins)
            {
                static_cast<void>(edges);
                m_impl->component_bins[bin].push_back(component);
            }
        }
        m_impl->prefix.push_back(0U);
        for (auto const& chord : m_impl->context->geometry.chords)
        {
            m_impl->prefix.push_back(m_impl->prefix.back()
                + static_cast<uint64_t>(std::max(1.0, std::ceil(length(chord.delta) / 2.0))));
        }
    }
    catch (FineException const& error)
    {
        m_impl->startup_failure = error.failure;
    }
}

V21FineProvider::~V21FineProvider() = default;

std::expected<std::shared_ptr<V21FinePlan const>, V21FineFailure> V21FineProvider::forRegion(
    V21FineGrid const& grid, uint32_t lod, std::stop_token token
) const
{
    try
    {
        checkCancellation(token);
        if (m_impl->startup_failure)
        {
            return std::unexpected{*m_impl->startup_failure};
        }
        uint64_t const points = uint64_t{grid.rows} * grid.columns;
        if (lod > 3U || !std::isfinite(grid.origin_x_m) || !std::isfinite(grid.origin_y_m)
            || !std::isfinite(grid.spacing_m) || grid.spacing_m <= 0.0)
        {
            fail(V21FineFailureKind::InvalidInput, "finite V21 grid coordinates and lod0..3 required");
        }
        if (points > MAX_QUERY_POINTS)
        {
            fail(V21FineFailureKind::LimitExceeded, "region query points exceed1048576");
        }
        if (lod == 0U)
        {
            auto impl = std::make_shared<V21FinePlan::Impl const>(m_impl->context,
                std::vector<V21FineChord>{}, std::vector<std::string>{});
            return std::shared_ptr<V21FinePlan const>{new V21FinePlan{std::move(impl)}};
        }
        std::vector<size_t> selected;
        size_t fine_count = 0U;
        if (points != 0U)
        {
            auto const x = circularInterval(grid.origin_x_m, grid.spacing_m, grid.columns);
            auto const y = circularInterval(grid.origin_y_m, grid.spacing_m, grid.rows);
            std::vector<size_t> candidates;
            int32_t const x0 = x.second - x.first >= WORLD - 1e-7
                ? 0 : static_cast<int32_t>(std::floor(x.first / 256.0));
            int32_t const x1 = x.second - x.first >= WORLD - 1e-7
                ? 255 : static_cast<int32_t>(std::floor(x.second / 256.0));
            int32_t const y0 = y.second - y.first >= WORLD - 1e-7
                ? 0 : static_cast<int32_t>(std::floor(y.first / 256.0));
            int32_t const y1 = y.second - y.first >= WORLD - 1e-7
                ? 255 : static_cast<int32_t>(std::floor(y.second / 256.0));
            for (int32_t by = y0; by <= y1; ++by)
            {
                for (int32_t bx = x0; bx <= x1; ++bx)
                {
                    auto const bin = m_impl->component_bins.find((by % 256) * 256 + bx % 256);
                    if (bin != m_impl->component_bins.end())
                    {
                        candidates.insert(candidates.end(), bin->second.begin(), bin->second.end());
                    }
                }
            }
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
            for (size_t component : candidates)
            {
                bool hit = false;
                for (size_t edge : m_impl->components[component].coarse)
                {
                    auto const& chord = m_impl->context->geometry.chords[edge];
                    double const radius = chord.halfwidth + MAXIMUM_SUPPORT;
                    if (axisIntersects(std::min(chord.a[0], chord.a[0] + chord.delta[0]) - radius,
                            std::max(chord.a[0], chord.a[0] + chord.delta[0]) + radius, x)
                        && axisIntersects(std::min(chord.a[1], chord.a[1] + chord.delta[1]) - radius,
                            std::max(chord.a[1], chord.a[1] + chord.delta[1]) + radius, y))
                    {
                        hit = true;
                        break;
                    }
                }
                if (hit)
                {
                    selected.push_back(component);
                    fine_count += m_impl->components[component].fine_count;
                }
            }
        }
        if (selected.size() > 256U || fine_count > MAX_FINE_CHORDS)
        {
            fail(V21FineFailureKind::LimitExceeded, "region exceeds whole-component fine plan caps");
        }
        std::vector<V21FineChord> rows;
        std::vector<std::string> ids;
        std::lock_guard lock{m_impl->mutex};
        rows.reserve(fine_count);
        for (size_t component : selected)
        {
            checkCancellation(token);
            auto record = m_impl->cache.find(component);
            if (record == m_impl->cache.end())
            {
                auto built = buildComponent(*m_impl->context, m_impl->components[component], m_impl->prefix, token);
                size_t const bytes = built.size() * sizeof(V21FineChord);
                while (!m_impl->cache.empty() && m_impl->cache_bytes + bytes > MAX_CACHE_BYTES)
                {
                    auto const oldest = std::min_element(m_impl->cache.begin(), m_impl->cache.end(),
                        [](auto const& a, auto const& b) { return a.second.touched < b.second.touched; });
                    m_impl->cache_bytes -= oldest->second.rows.size() * sizeof(V21FineChord);
                    m_impl->cache.erase(oldest);
                }
                record = m_impl->cache.emplace(component, Impl::Record{std::move(built), ++m_impl->clock}).first;
                m_impl->cache_bytes += bytes;
            }
            record->second.touched = ++m_impl->clock;
            rows.insert(rows.end(), record->second.rows.begin(), record->second.rows.end());
            ids.push_back(m_impl->components[component].id);
        }
        std::stable_sort(rows.begin(), rows.end(), [](auto const& a, auto const& b) {
            return a.global_order < b.global_order;
        });
        for (size_t i = 1U; i < rows.size(); ++i)
        {
            if (rows[i - 1U].global_order == rows[i].global_order)
            {
                fail(V21FineFailureKind::ComponentOrderError, "selected components overlap in global course order");
            }
        }
        auto impl = std::make_shared<V21FinePlan::Impl const>(m_impl->context, std::move(rows), std::move(ids));
        return std::shared_ptr<V21FinePlan const>{new V21FinePlan{std::move(impl)}};
    }
    catch (FineException const& error)
    {
        return std::unexpected{error.failure};
    }
    catch (std::exception const& error)
    {
        if (token.stop_requested())
        {
            return std::unexpected{V21FineFailure{V21FineFailureKind::Cancelled,
                "fine component plan cancelled", {}}};
        }
        return std::unexpected{callbackFailure(error)};
    }
}

std::expected<std::vector<double>, V21FineFailure> V21FinePlan::applyProfile(
    std::span<double const> background, std::span<double const> x, std::span<double const> y,
    uint32_t lod, std::stop_token token
) const
{
    try
    {
        checkCancellation(token);
        if (lod > 3U || background.size() != x.size() || x.size() != y.size())
        {
            fail(V21FineFailureKind::InvalidInput, "matching background/coordinate arrays and lod0..3 required");
        }
        if (x.size() > MAX_QUERY_POINTS)
        {
            fail(V21FineFailureKind::LimitExceeded, "profile points exceed1048576");
        }
        std::vector<V21FinePoint> points(x.size());
        for (size_t i = 0U; i < x.size(); ++i)
        {
            if (!std::isfinite(background[i]) || !std::isfinite(x[i]) || !std::isfinite(y[i]))
            {
                fail(V21FineFailureKind::InvalidInput, "finite background and coordinates required");
            }
            points[i] = {x[i], y[i]};
        }
        std::vector<double> result(background.begin(), background.end());
        if (lod == 0U || m_impl->chords.empty())
        {
            return result;
        }
        auto const authority = standing(*m_impl->context, points, token);
        for (size_t point = 0U; point < points.size(); ++point)
        {
            if (point % 4096U == 0U)
            {
                checkCancellation(token);
            }
            double const px = canonical(points[point][0]), py = canonical(points[point][1]);
            int32_t const bin = static_cast<int32_t>(std::floor(py / 256.0)) * 256
                + static_cast<int32_t>(std::floor(px / 256.0));
            auto const candidates = m_impl->index.bins.find(bin);
            if (candidates == m_impl->index.bins.end())
            {
                continue;
            }
            double core_value = background[point], nearest = INF;
            bool core = false;
            for (size_t edge : candidates->second)
            {
                auto const& chord = m_impl->chords[edge];
                double const dx = displacement(px - chord.a[0]), dy = displacement(py - chord.a[1]);
                double const length2 = chord.delta[0] * chord.delta[0] + chord.delta[1] * chord.delta[1];
                double const t = std::clamp((dx * chord.delta[0] + dy * chord.delta[1]) / length2, 0.0, 1.0);
                double const ox = dx - t * chord.delta[0], oy = dy - t * chord.delta[1];
                double const distance = std::sqrt(ox * ox + oy * oy);
                double const support = lod == 3U ? chord.support_m : chord.support_coarse_m;
                bool const same = authority[point].owner == m_impl->context->geometry.vertex_owner[chord.start];
                if (distance > support || (!same && distance > chord.halfwidth))
                {
                    continue;
                }
                double const grade = chord.grade0 + t * (chord.grade1 - chord.grade0);
                double const stage = chord.water_stage0 + t * (chord.water_stage1 - chord.water_stage0);
                double const bottom = chord.halfwidth * (1.0 - 2.0 * m_impl->context->geometry.bank_fraction);
                double const cross = std::clamp((distance - bottom) / (chord.halfwidth - bottom), 0.0, 1.0);
                double const across = dx * -chord.delta[1] + dy * chord.delta[0];
                double const slope = lod == 3U
                    ? (across < 0.0 ? chord.bank_slope_left : chord.bank_slope_right)
                    : (across < 0.0 ? chord.coarse_bank_slope_left : chord.coarse_bank_slope_right);
                double const target = distance <= chord.halfwidth ? grade + (stage - grade) * cross
                    : stage + slope * (distance - chord.halfwidth);
                if (distance > chord.halfwidth)
                {
                    result[point] = std::min(result[point], target);
                }
                else if (distance / chord.halfwidth < nearest)
                {
                    core_value = std::min(background[point], target);
                    nearest = distance / chord.halfwidth;
                    core = true;
                }
            }
            if (core)
            {
                result[point] = core_value;
            }
        }
        std::vector<double> protected_result;
        protected_result.reserve(result.size());
        for (size_t offset = 0U; offset < result.size(); offset += CALLBACK_BATCH)
        {
            checkCancellation(token);
            size_t const count = std::min(CALLBACK_BATCH, result.size() - offset);
            auto values = m_impl->context->callbacks.protect(std::span{result}.subspan(offset, count),
                std::span{points}.subspan(offset, count));
            if (values.size() != count || !std::all_of(values.begin(), values.end(),
                    [](double value) { return std::isfinite(value); }))
            {
                fail(V21FineFailureKind::InvalidInput, "finite protected-contact profile matching coordinates required");
            }
            for (size_t i = 0U; i < count; ++i)
            {
                protected_result.push_back(std::min(background[offset + i], values[i]));
            }
        }
        return protected_result;
    }
    catch (FineException const& error)
    {
        return std::unexpected{error.failure};
    }
    catch (std::exception const& error)
    {
        if (token.stop_requested())
        {
            return std::unexpected{V21FineFailure{V21FineFailureKind::Cancelled,
                "fine profile cancelled", {}}};
        }
        return std::unexpected{callbackFailure(error)};
    }
}

} // namespace generation::detail
