#include "V21Geomorphology.hpp"

#include "Numerics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <functional>
#include <limits>
#include <numeric>
#include <queue>
#include <stdexcept>

// NumPy evaluates the ported array expressions without implicit fused operations.
#pragma STDC FP_CONTRACT OFF

namespace generation::detail {
namespace {

constexpr int32_t N = 256;
constexpr int32_t TOTAL = N * N;
constexpr double DOMAIN_M = 65'536.;
constexpr double CELL = 256.;
constexpr double BIN = 512.;
constexpr int32_t BINS = 128;

struct Point {
    double x;
    double y;
};

struct Segment {
    Point start;
    Point delta;
    double width = 0.;
    double floor0 = 0.;
    double floor1 = 0.;
};

void checkMorphologyCancellation(std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
        throw GenerationCancelled();
    }
}

double wrap(double displacement) {
    double remainder = std::fmod(displacement + DOMAIN_M / 2., DOMAIN_M);
    if (remainder < 0.) {
        remainder += DOMAIN_M;
    }
    return remainder - DOMAIN_M / 2.;
}

int32_t node(int32_t x, int32_t y) {
    return ((y + N) % N) * N + (x + N) % N;
}

Point position(int32_t index) {
    return {static_cast<double>(index % N) * CELL, static_cast<double>(index / N) * CELL};
}

Point difference(Point end, Point start) {
    return {wrap(end.x - start.x), wrap(end.y - start.y)};
}

double parameter(Point point, Segment const& segment) {
    Point const offset = difference(point, segment.start);
    double const length2 = segment.delta.x * segment.delta.x + segment.delta.y * segment.delta.y;
    return std::clamp(
        (offset.x * segment.delta.x + offset.y * segment.delta.y) / std::max(length2, 1e-30), 0., 1.
    );
}

double distance(Point point, Segment const& segment, double t) {
    Point const offset = difference(point, segment.start);
    double const x = offset.x - t * segment.delta.x;
    double const y = offset.y - t * segment.delta.y;
    return std::sqrt(x * x + y * y);
}

using SegmentIndex = std::vector<std::vector<size_t>>;

SegmentIndex makeIndex(std::span<Segment const> segments, double radius, std::stop_token cancellation) {
    SegmentIndex result(BINS * BINS);
    for (size_t edge = 0; edge < segments.size(); ++edge) {
        checkMorphologyCancellation(cancellation);
        Segment const& segment = segments[edge];
        double const r = radius > 0. ? radius : segment.width;
        int32_t const lo_x = static_cast<int32_t>(std::floor(
            (std::min(segment.start.x, segment.start.x + segment.delta.x) - r) / BIN
        ));
        int32_t const hi_x = static_cast<int32_t>(std::floor(
            (std::max(segment.start.x, segment.start.x + segment.delta.x) + r) / BIN
        ));
        int32_t const lo_y = static_cast<int32_t>(std::floor(
            (std::min(segment.start.y, segment.start.y + segment.delta.y) - r) / BIN
        ));
        int32_t const hi_y = static_cast<int32_t>(std::floor(
            (std::max(segment.start.y, segment.start.y + segment.delta.y) + r) / BIN
        ));
        for (int32_t by = lo_y; by <= hi_y; ++by) {
            for (int32_t bx = lo_x; bx <= hi_x; ++bx) {
                result[((by + BINS) % BINS) * BINS + (bx + BINS) % BINS].push_back(edge);
            }
        }
    }
    return result;
}

std::vector<Segment> shores(
    std::span<double const> bed, std::span<uint8_t const> ocean, std::stop_token cancellation
) {
    std::vector<Segment> result;
    for (int32_t index = 0; index < TOTAL; ++index) {
        if (index % N == 0) {
            checkMorphologyCancellation(cancellation);
        }
        int32_t const x = index % N;
        int32_t const y = index / N;
        std::array<std::array<int32_t, 3>, 2> const triangles = {{
            {index, node(x + 1, y), node(x + 1, y + 1)},
            {index, node(x + 1, y + 1), node(x, y + 1)},
        }};
        for (auto const& triangle : triangles) {
            double const low = std::min({bed[triangle[0]], bed[triangle[1]], bed[triangle[2]]});
            double const high = std::max({bed[triangle[0]], bed[triangle[1]], bed[triangle[2]]});
            bool const ocean_triangle = ocean.empty()
                || ocean[triangle[0]] || ocean[triangle[1]] || ocean[triangle[2]];
            if (!(low < 0. && high > 0. && ocean_triangle)) {
                continue;
            }
            Point const origin = position(triangle[0]);
            std::array<Point, 3> xy;
            for (size_t i = 0; i < xy.size(); ++i) {
                Point const offset = difference(position(triangle[i]), origin);
                xy[i] = {origin.x + offset.x, origin.y + offset.y};
            }
            std::array<Point, 2> intersections;
            size_t count = 0;
            for (auto const& pair : std::array<std::array<size_t, 2>, 3>{{{0, 1}, {1, 2}, {2, 0}}}) {
                size_t const i = pair[0];
                size_t const j = pair[1];
                if ((bed[triangle[i]] < 0.) != (bed[triangle[j]] < 0.)) {
                    double const t = -bed[triangle[i]] / (bed[triangle[j]] - bed[triangle[i]]);
                    intersections[count++] = {
                        xy[i].x + t * (xy[j].x - xy[i].x),
                        xy[i].y + t * (xy[j].y - xy[i].y),
                    };
                }
            }
            if (count == 2) {
                result.push_back({
                    intersections[0],
                    {intersections[1].x - intersections[0].x, intersections[1].y - intersections[0].y},
                });
            }
        }
    }
    return result;
}

std::vector<double> localRelief(std::span<double const> bed, std::stop_token cancellation) {
    std::vector<double> relief(TOTAL);
    for (int32_t index = 0; index < TOTAL; ++index) {
        if (index % N == 0) {
            checkMorphologyCancellation(cancellation);
        }
        double low = std::numeric_limits<double>::infinity();
        double high = -std::numeric_limits<double>::infinity();
        for (int32_t dy = -3; dy <= 3; ++dy) {
            for (int32_t dx = -3; dx <= 3; ++dx) {
                double const height = bed[node(index % N + dx, index / N + dy)];
                low = std::min(low, height);
                high = std::max(high, height);
            }
        }
        relief[index] = high - low;
    }
    return relief;
}

} // namespace

std::vector<double> evolveV21Terrain(
    std::span<double const> bed, std::span<uint8_t const> ocean, std::stop_token cancellation
) {
    if (bed.size() != TOTAL || ocean.size() != TOTAL
        || !std::all_of(bed.begin(), bed.end(), [](double height) { return std::isfinite(height); })) {
        throw std::invalid_argument("V21 terrain evolution requires a finite 256 by 256 bed and ocean mask");
    }
    checkMorphologyCancellation(cancellation);
    std::vector<Segment> const marine_shores = shores(bed, ocean, cancellation);
    if (marine_shores.empty()) {
        throw std::runtime_error("V21 terrain evolution requires an ocean shoreline");
    }
    SegmentIndex const marine_index = makeIndex(marine_shores, 2'048., cancellation);
    std::vector<double> coastal(bed.begin(), bed.end());
    for (int32_t index = 0; index < TOTAL; ++index) {
        if (index % N == 0) {
            checkMorphologyCancellation(cancellation);
        }
        Point const point = position(index);
        double nearest = std::numeric_limits<double>::infinity();
        for (size_t edge : marine_index[(index / N / 2) * BINS + index % N / 2]) {
            Segment const& segment = marine_shores[edge];
            nearest = std::min(nearest, distance(point, segment, parameter(point, segment)));
        }
        if (bed[index] >= 0.) {
            double const u = std::clamp(nearest / 32., 0., 1.);
            double const weight = std::pow(1. - u * u, 3.);
            double const shore_distance = std::isfinite(nearest) ? nearest : 0.;
            coastal[index] = bed[index] + (.06 * shore_distance - bed[index]) * weight;
        } else if (nearest < 2'048.) {
            double blend = std::clamp(nearest / 2'048., 0., 1.);
            blend = blend * blend * (3. - 2. * blend);
            double const profile = -(.025 * nearest + .00008 * nearest * nearest);
            coastal[index] = profile + (bed[index] - profile) * blend;
        }
    }
    std::vector<int32_t> receiver(TOTAL, -1);
    std::vector<double> best(TOTAL, 0.);
    // Ordering is significant: strict greater-than retains the first equal-slope receiver.
    for (auto const& offset : std::array<std::array<int32_t, 2>, 6>{{
        {-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, 1},
    }}) {
        checkMorphologyCancellation(cancellation);
        for (int32_t index = 0; index < TOTAL; ++index) {
            int32_t const target = node(index % N + offset[1], index / N + offset[0]);
            double const slope = (coastal[index] - coastal[target])
                / (CELL * std::hypot(offset[1], offset[0]));
            if (slope > best[index]) {
                best[index] = slope;
                receiver[index] = target;
            }
        }
    }
    std::vector<int32_t> descending(TOTAL);
    std::iota(descending.begin(), descending.end(), 0);
    std::stable_sort(descending.begin(), descending.end(), [&](int32_t a, int32_t b) {
        return coastal[a] > coastal[b];
    });
    std::vector<double> area(TOTAL, 1.);
    for (int32_t source : descending) {
        if (receiver[source] >= 0) {
            area[receiver[source]] += area[source];
        }
    }
    std::vector<double> const relief = localRelief(coastal, cancellation);
    std::vector<int32_t> ascending(TOTAL);
    std::iota(ascending.begin(), ascending.end(), 0);
    std::stable_sort(ascending.begin(), ascending.end(), [&](int32_t a, int32_t b) {
        return coastal[a] < coastal[b];
    });
    std::vector<double> baselevel = coastal;
    for (int32_t index = 0; index < TOTAL; ++index) {
        if (coastal[index] < 0.) {
            baselevel[index] = 0.;
        }
    }
    for (int32_t source : ascending) {
        if (receiver[source] >= 0 && coastal[source] >= 0.) {
            baselevel[source] = baselevel[receiver[source]];
        }
    }
    std::vector<double> floor(TOTAL);
    for (int32_t index = 0; index < TOTAL; ++index) {
        double const node_area = area[index] * (CELL * CELL) / 1e6;
        double const depth = std::clamp(.28 * relief[index] * std::log1p(node_area / .065) / std::log(33.), 0., 220.);
        floor[index] = std::max(coastal[index] - depth, baselevel[index]);
    }
    for (int32_t source : descending) {
        if (receiver[source] >= 0) {
            floor[receiver[source]] = std::min(floor[receiver[source]], floor[source]);
        }
    }
    std::vector<Segment> trunks;
    for (int32_t source = 0; source < TOTAL; ++source) {
        if (receiver[source] < 0 || coastal[source] <= 0. || area[source] < 12.) {
            continue;
        }
        int32_t const target = receiver[source];
        double const source_area = area[source] * (CELL * CELL) / 1e6;
        trunks.push_back({
            position(source), difference(position(target), position(source)),
            std::clamp(130. + 220. * std::sqrt(source_area), 130., 950.), floor[source], floor[target],
        });
    }
    SegmentIndex const valley_index = makeIndex(trunks, 0., cancellation);
    std::vector<Segment> const all_shores = shores(coastal, {}, cancellation);
    SegmentIndex const shore_index = makeIndex(all_shores, 96., cancellation);
    std::vector<double> final = coastal;
    for (int32_t index = 0; index < TOTAL; ++index) {
        if (index % N == 0) {
            checkMorphologyCancellation(cancellation);
        }
        Point const point = position(index);
        int32_t const bin = (index / N / 2) * BINS + index % N / 2;
        double carved = coastal[index];
        for (size_t edge : valley_index[bin]) {
            Segment const& segment = trunks[edge];
            double const t = parameter(point, segment);
            double const d = distance(point, segment, t);
            double const u = std::clamp(d / segment.width, 0., 1.);
            double const bed_floor = segment.floor0 + t * (segment.floor1 - segment.floor0);
            double const potential = std::max(coastal[index] - bed_floor, 0.);
            double const excavation = 260. * numpyTanh(potential / 260.);
            double const candidate = coastal[index] - excavation * std::pow(1. - u * u, 3.) * .65;
            carved = std::min(carved, std::max(candidate, coastal[index] - 260.));
        }
        double nearest = 96.;
        for (size_t edge : shore_index[bin]) {
            Segment const& segment = all_shores[edge];
            double const length2 = segment.delta.x * segment.delta.x + segment.delta.y * segment.delta.y;
            if (length2 == 0.) {
                continue;
            }
            nearest = std::min(nearest, distance(point, segment, parameter(point, segment)));
        }
        double const u = std::clamp(nearest / 96., 0., 1.);
        double const taper = coastal[index] < 0. ? 0. : u * u * (3. - 2. * u);
        final[index] = coastal[index] + (carved - coastal[index]) * taper;
    }
    checkMorphologyCancellation(cancellation);
    return final;
}

} // namespace generation::detail

namespace generation::detail
{
namespace
{

std::vector<uint8_t> largestOcean(std::span<double const> bed, std::stop_token cancellation)
{
    std::vector<uint8_t> seen(TOTAL, 0U), ocean(TOTAL, 0U);
    std::vector<int32_t> component, largest;
    for (int32_t root = 0; root < TOTAL; ++root)
    {
        if (root % N == 0)
        {
            checkMorphologyCancellation(cancellation);
        }
        if (bed[root] >= 0.0 || seen[root])
        {
            continue;
        }
        component.clear();
        component.push_back(root);
        seen[root] = 1U;
        for (size_t cursor = 0U; cursor < component.size(); ++cursor)
        {
            int32_t const i = component[cursor];
            for (int32_t j : {node(i % N - 1, i / N), node(i % N + 1, i / N),
                    node(i % N, i / N - 1), node(i % N, i / N + 1)})
            {
                if (bed[j] < 0.0 && !seen[j])
                {
                    seen[j] = 1U;
                    component.push_back(j);
                }
            }
        }
        if (component.size() > largest.size())
        {
            largest = component;
        }
    }
    for (int32_t i : largest)
    {
        ocean[i] = 1U;
    }
    return ocean;
}

struct DryRouting final
{
    std::vector<int32_t> downstream;
    std::vector<float> flow;
};

DryRouting routeDryArea(std::span<double const> bed, std::stop_token cancellation)
{
    auto const ocean = largestOcean(bed, cancellation);
    std::vector<uint8_t> visited = ocean;
    std::vector<double> filled(bed.begin(), bed.end());
    DryRouting result{std::vector<int32_t>(TOTAL, -1), std::vector<float>(TOTAL)};
    using HeapItem = std::pair<double, int32_t>;
    std::priority_queue<HeapItem, std::vector<HeapItem>, std::greater<HeapItem>> heap;
    for (int32_t i = 0; i < TOTAL; ++i)
    {
        if (ocean[i])
        {
            heap.emplace(filled[i], i);
        }
    }
    if (heap.empty())
    {
        int32_t const sink = static_cast<int32_t>(
            std::min_element(bed.begin(), bed.end()) - bed.begin());
        visited[sink] = 1U;
        heap.emplace(filled[sink], sink);
    }
    while (!heap.empty())
    {
        auto const [height, i] = heap.top();
        heap.pop();
        for (int32_t j : {node(i % N - 1, i / N), node(i % N + 1, i / N),
                node(i % N, i / N - 1), node(i % N, i / N + 1)})
        {
            if (!visited[j])
            {
                visited[j] = 1U;
                filled[j] = std::max(filled[j], height);
                result.downstream[j] = i;
                heap.emplace(filled[j], j);
            }
        }
    }
    // prototype.route publishes float32 water_level before strict D8 refinement.
    std::vector<float> level(TOTAL);
    for (int32_t i = 0; i < TOTAL; ++i)
    {
        level[i] = static_cast<float>(filled[i]);
    }
    std::vector<double> best(TOTAL, 0.0);
    for (auto const& offset : std::array<std::array<int32_t, 2U>, 8U>{{
        {-1, -1}, {-1, 0}, {-1, 1}, {0, -1}, {0, 1}, {1, -1}, {1, 0}, {1, 1},
    }})
    {
        checkMorphologyCancellation(cancellation);
        int32_t const dy = offset[0], dx = offset[1];
        double const step = CELL * std::hypot(dx, dy);
        for (int32_t i = 0; i < TOTAL; ++i)
        {
            int32_t const other = node(i % N + dx, i / N + dy);
            double const slope = static_cast<float>(level[i] - level[other]) / step;
            bool take = slope > best[i] && !ocean[i];
            if (dx != 0 && dy != 0)
            {
                float const corners = level[node(i % N, i / N + dy)]
                    + level[node(i % N + dx, i / N)];
                take = take && corners <= 2.0F * level[i] + 1e-5F
                    && 2.0F * level[other] <= corners + 1e-5F;
            }
            if (take)
            {
                result.downstream[i] = other;
                best[i] = slope;
            }
        }
    }
    std::vector<int32_t> incoming(TOTAL, 0);
    for (int32_t target : result.downstream)
    {
        if (target >= 0)
        {
            ++incoming[target];
        }
    }
    std::deque<int32_t> queue;
    std::vector<double> flow(TOTAL);
    for (int32_t i = 0; i < TOTAL; ++i)
    {
        flow[i] = !ocean[i] && bed[i] > 0.0 ? CELL * CELL : 0.0;
        if (incoming[i] == 0)
        {
            queue.push_back(i);
        }
    }
    size_t visited_count = 0U;
    while (!queue.empty())
    {
        int32_t const i = queue.front();
        queue.pop_front();
        ++visited_count;
        int32_t const j = result.downstream[i];
        if (j >= 0)
        {
            flow[j] += flow[i];
            if (--incoming[j] == 0)
            {
                queue.push_back(j);
            }
        }
    }
    if (visited_count != TOTAL)
    {
        throw std::runtime_error{"V21 plains have a cyclic strict D8 receiver graph"};
    }
    for (int32_t i = 0; i < TOTAL; ++i)
    {
        result.flow[i] = static_cast<float>(flow[i]);
    }
    return result;
}

// Separable squared Euclidean distance transform. The lower envelope is exact
// for the regular sample geometry; sampling is the physical (row,column) step.
std::vector<double> interiorDistance(
    std::span<uint8_t const> mask, int32_t rows, int32_t columns,
    double row_step = 1.0, double column_step = 1.0
)
{
    std::vector<double> work(mask.size()), result(mask.size());
    bool const all_inside = std::all_of(mask.begin(), mask.end(), [](uint8_t v) { return v != 0U; });
    if (all_inside)
    {
        for (int32_t y = 0; y < rows; ++y)
        {
            for (int32_t x = 0; x < columns; ++x)
            {
                result[y * columns + x] = std::sqrt(
                    (y + 1.0) * (y + 1.0) * row_step * row_step
                    + x * x * column_step * column_step);
            }
        }
        return result;
    }
    for (size_t i = 0U; i < mask.size(); ++i)
    {
        work[i] = mask[i] ? 1e30 : 0.0;
    }
    int32_t const maximum = std::max(rows, columns);
    std::vector<double> input(maximum), output(maximum), boundary(maximum + 1);
    std::vector<int32_t> vertices(maximum);
    auto const transform = [&](int32_t count, double spacing) {
        double const scale = spacing * spacing;
        int32_t k = 0;
        vertices[0] = 0;
        boundary[0] = -std::numeric_limits<double>::infinity();
        boundary[1] = std::numeric_limits<double>::infinity();
        for (int32_t q = 1; q < count; ++q)
        {
            double s;
            do
            {
                int32_t const v = vertices[k];
                s = ((input[q] + scale * q * q) - (input[v] + scale * v * v))
                    / (2.0 * scale * (q - v));
                if (s <= boundary[k])
                {
                    --k;
                }
                else
                {
                    break;
                }
            } while (k >= 0);
            ++k;
            vertices[k] = q;
            boundary[k] = s;
            boundary[k + 1] = std::numeric_limits<double>::infinity();
        }
        k = 0;
        for (int32_t q = 0; q < count; ++q)
        {
            while (boundary[k + 1] < q)
            {
                ++k;
            }
            double const delta = static_cast<double>(q - vertices[k]);
            output[q] = scale * delta * delta + input[vertices[k]];
        }
    };
    for (int32_t x = 0; x < columns; ++x)
    {
        for (int32_t y = 0; y < rows; ++y)
        {
            input[y] = work[y * columns + x];
        }
        transform(rows, row_step);
        for (int32_t y = 0; y < rows; ++y)
        {
            result[y * columns + x] = output[y];
        }
    }
    for (int32_t y = 0; y < rows; ++y)
    {
        for (int32_t x = 0; x < columns; ++x)
        {
            input[x] = result[y * columns + x];
        }
        transform(columns, column_step);
        for (int32_t x = 0; x < columns; ++x)
        {
            work[y * columns + x] = std::sqrt(output[x]);
        }
    }
    return work;
}

double smoothstep(double value)
{
    value = std::clamp(value, 0.0, 1.0);
    return value * value * (3.0 - 2.0 * value);
}

double plainTarget(V21PlainFeature const& plain, double x_m, double y_m)
{
    double const dx = wrap(x_m - plain.outlet_m[0]);
    double const dy = wrap(y_m - plain.outlet_m[1]);
    double const s = dx * plain.along[0] + dy * plain.along[1];
    double const d = dx * plain.normal[0] + dy * plain.normal[1];
    return plain.baselevel_m + 4.0 + .003 * s + (.0005 / 500.0) * d * d;
}

struct PlainChain final
{
    double grade = 0.0;
    int32_t end = 0;
    std::vector<int32_t> path;
    double distance_m = 0.0;
};

std::vector<PlainChain> plainChains(
    std::span<double const> bed, DryRouting const& routing,
    std::vector<std::vector<int32_t>>& reverse
)
{
    std::vector<uint8_t> actual(TOTAL, 0U), targets(TOTAL, 0U);
    for (int32_t i = 0; i < TOTAL; ++i)
    {
        int32_t const j = routing.downstream[i];
        if (j >= 0 && i != j)
        {
            reverse[j].push_back(i);
        }
        if (routing.flow[i] >= 12.0 * CELL * CELL && bed[i] > 0.0 && j >= 0)
        {
            actual[i] = 1U;
            targets[j] = 1U;
        }
    }
    std::vector<PlainChain> result;
    for (int32_t end = 0; end < TOTAL; ++end)
    {
        if (!targets[end] || actual[end])
        {
            continue;
        }
        PlainChain chain;
        chain.end = end;
        chain.path.push_back(end);
        while (true)
        {
            int32_t const last = chain.path.back();
            int32_t upstream = -1;
            for (int32_t candidate : reverse[last])
            {
                if (actual[candidate] && (upstream < 0
                        || routing.flow[candidate] > routing.flow[upstream]))
                {
                    upstream = candidate;
                }
            }
            if (upstream < 0)
            {
                break;
            }
            Point const delta = difference(position(upstream), position(last));
            chain.distance_m += std::sqrt(delta.x * delta.x + delta.y * delta.y);
            chain.path.push_back(upstream);
        }
        if (chain.distance_m >= 2000.0)
        {
            chain.grade = (bed[chain.path.back()] - std::max(bed[end], 0.0)) / chain.distance_m;
            if (chain.grade >= 0.0 && chain.grade <= .05)
            {
                result.push_back(std::move(chain));
            }
        }
    }
    std::sort(result.begin(), result.end(), [](PlainChain const& a, PlainChain const& b) {
        return a.grade != b.grade ? a.grade < b.grade : a.end < b.end;
    });
    return result;
}

V21PlainFeature makePlain(
    PlainChain const& chain, uint32_t rank, std::span<double const> bed,
    std::vector<std::vector<int32_t>> const& reverse
)
{
    V21PlainFeature feature;
    feature.candidate_rank = rank;
    feature.outlet_vertex = chain.end;
    feature.path_vertices = chain.path;
    feature.source_valley_grade = chain.grade;
    feature.actual_path_length_m = chain.distance_m;
    Point const outlet = position(chain.end);
    Point const delta = difference(position(chain.path.back()), outlet);
    double const length = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    feature.outlet_m = {outlet.x, outlet.y};
    feature.along = {delta.x / length, delta.y / length};
    feature.normal = {-feature.along[1], feature.along[0]};
    feature.length_m = std::max(2000.0, length);
    feature.baselevel_m = std::max(bed[chain.end], 0.0);
    std::vector<uint8_t> catchment(TOTAL, 0U);
    std::deque<int32_t> queue{chain.end};
    catchment[chain.end] = 1U;
    while (!queue.empty())
    {
        int32_t const next = queue.front();
        queue.pop_front();
        for (int32_t i : reverse[next])
        {
            if (!catchment[i])
            {
                catchment[i] = 1U;
                queue.push_back(i);
            }
        }
    }
    std::vector<uint8_t> tiled(9U * TOTAL);
    for (int32_t y = 0; y < 3 * N; ++y)
    {
        for (int32_t x = 0; x < 3 * N; ++x)
        {
            tiled[y * 3 * N + x] = catchment[(y % N) * N + x % N];
        }
    }
    auto const distance = interiorDistance(tiled, 3 * N, 3 * N);
    feature.interior_distance_m.resize(TOTAL);
    for (int32_t y = 0; y < N; ++y)
    {
        for (int32_t x = 0; x < N; ++x)
        {
            feature.interior_distance_m[y * N + x] = distance[(y + N) * 3 * N + x + N] * CELL;
        }
    }
    return feature;
}

} // namespace

double v21PlainWeight(V21PlainFeature const& plain, double x_m, double y_m)
{
    if (plain.interior_distance_m.size() != TOTAL || !std::isfinite(x_m) || !std::isfinite(y_m))
    {
        throw std::invalid_argument{"V21 plain sampling requires finite coordinates and a canonical interior"};
    }
    double const dx = wrap(x_m - plain.outlet_m[0]);
    double const dy = wrap(y_m - plain.outlet_m[1]);
    double const s = dx * plain.along[0] + dy * plain.along[1];
    double const d = dx * plain.normal[0] + dy * plain.normal[1];
    double const width = 1.0 - smoothstep((std::abs(d) - 500.0) / 350.0);
    double const ends = smoothstep((s - 150.0) / 300.0)
        * (1.0 - smoothstep((s - (plain.length_m - 400.0)) / 300.0));
    double px = std::fmod(x_m / CELL, static_cast<double>(N));
    double py = std::fmod(y_m / CELL, static_cast<double>(N));
    if (px < 0.0)
    {
        px += N;
    }
    if (py < 0.0)
    {
        py += N;
    }
    int32_t const ix = static_cast<int32_t>(std::floor(px));
    int32_t const iy = static_cast<int32_t>(std::floor(py));
    double const u = px - ix, v = py - iy;
    auto const at = [&](int32_t x, int32_t y) { return plain.interior_distance_m[node(x, y)]; };
    double const interior = ((1.0 - u) * (1.0 - v)) * at(ix, iy)
        + (u * (1.0 - v)) * at(ix + 1, iy)
        + ((1.0 - u) * v) * at(ix, iy + 1) + (u * v) * at(ix + 1, iy + 1);
    return width * ends * smoothstep((interior - 128.0) / 256.0);
}

double v21ApplyPlainHistory(
    std::span<V21PlainFeature const> plains, double bed_m, double x_m, double y_m
)
{
    for (auto const& plain : plains)
    {
        double const weight = v21PlainWeight(plain, x_m, y_m);
        bed_m = bed_m * (1.0 - weight) + plainTarget(plain, x_m, y_m) * weight;
    }
    return bed_m;
}

double v21PlainResidual(
    std::span<V21PlainFeature const> plains, double detail_m, double x_m, double y_m
)
{
    for (auto const& plain : plains)
    {
        detail_m *= 1.0 - v21PlainWeight(plain, x_m, y_m);
    }
    return detail_m;
}

double v21InterpolateEvolvedBed(
    std::span<double const> bed,
    double x_m,
    double y_m,
    std::span<uint8_t const> protected_vertices
)
{
    if (bed.size() != TOTAL || (!protected_vertices.empty() && protected_vertices.size() != TOTAL)
        || !std::isfinite(x_m) || !std::isfinite(y_m))
    {
        throw std::invalid_argument{"V21 interpolation requires a canonical bed and finite coordinates"};
    }
    auto const at = [&](int32_t x, int32_t y) {
        double const value = bed[node(x, y)];
        if (!std::isfinite(value))
        {
            throw std::invalid_argument{"V21 interpolation received a nonfinite terrain sample"};
        }
        return value;
    };
    auto const slopes = [&](int32_t x, int32_t y) {
        double const z = at(x, y);
        if (!protected_vertices.empty() && protected_vertices[node(x, y)] != 0U)
        {
            return std::array<double, 2U>{0.0, 0.0};
        }
        auto const harmonic = [](double before, double after) {
            return before * after > 0.0 ? 2.0 * before * after / (before + after) : 0.0;
        };
        double const fx = harmonic((z - at(x - 1, y)) / CELL, (at(x + 1, y) - z) / CELL);
        double const fy = harmonic((z - at(x, y - 1)) / CELL, (at(x, y + 1) - z) / CELL);
        double scale = 1.0;
        for (int32_t sx : {-1, 1})
        {
            for (int32_t sy : {-1, 1})
            {
                double const z10 = at(x + sx, y), z01 = at(x, y + sy);
                double const z11 = at(x + sx, y + sy);
                double const lo = std::min({z, z10, z01, z11});
                double const hi = std::max({z, z10, z01, z11});
                for (double delta : {sx * fx * CELL / 3.0, sy * fy * CELL / 3.0,
                        (sx * fx + sy * fy) * CELL / 3.0})
                {
                    double const limit = std::abs(delta) > 0.0
                        ? (delta > 0.0 ? hi - z : z - lo) / std::abs(delta) : 1.0;
                    scale = std::min(scale, std::clamp(limit, 0.0, 1.0));
                }
            }
        }
        return std::array<double, 2U>{fx * scale, fy * scale};
    };
    double px = std::fmod(x_m, DOMAIN_M), py = std::fmod(y_m, DOMAIN_M);
    if (px < 0.0)
    {
        px += DOMAIN_M;
    }
    if (py < 0.0)
    {
        py += DOMAIN_M;
    }
    px /= CELL;
    py /= CELL;
    int32_t const ix = static_cast<int32_t>(std::floor(px));
    int32_t const iy = static_cast<int32_t>(std::floor(py));
    auto const basis = [](double t) {
        return std::array<double, 4U>{(2.0 * t - 3.0) * t * t + 1.0,
            (-2.0 * t + 3.0) * t * t, (t - 2.0) * t * t + t, (t - 1.0) * t * t};
    };
    auto const hx = basis(px - ix), hy = basis(py - iy);
    double result = 0.0;
    for (int32_t j = 0; j < 2; ++j)
    {
        for (int32_t i = 0; i < 2; ++i)
        {
            auto const slope = slopes(ix + i, iy + j);
            result += at(ix + i, iy + j) * hx[i] * hy[j]
                + CELL * slope[0] * hx[2 + i] * hy[j]
                + CELL * slope[1] * hx[i] * hy[2 + j];
        }
    }
    return result;
}

double v21PlainComposedBase(
    std::span<double const> evolved_bed_m, std::span<V21PlainFeature const> plains,
    double x_m, double y_m, std::span<uint8_t const> protected_vertices
)
{
    return v21ApplyPlainHistory(
        plains, v21InterpolateEvolvedBed(evolved_bed_m, x_m, y_m, protected_vertices), x_m, y_m
    );
}

V21GeomorphologyResult prepareV21Geomorphology(
    std::span<double const> canonical_h0_m, std::stop_token cancellation
)
{
    if (canonical_h0_m.size() != TOTAL || !std::all_of(canonical_h0_m.begin(),
            canonical_h0_m.end(), [](double z) { return std::isfinite(z); }))
    {
        throw std::invalid_argument{"V21 geomorphology requires finite canonical 256-square H0"};
    }
    auto const ocean = largestOcean(canonical_h0_m, cancellation);
    V21GeomorphologyResult result;
    result.evolved_bed_m = evolveV21Terrain(canonical_h0_m, ocean, cancellation);
    auto const& bed = result.evolved_bed_m;
    auto const routing = routeDryArea(bed, cancellation);
    std::vector<std::vector<int32_t>> reverse(TOTAL);
    auto const chains = plainChains(bed, routing, reverse);
    result.admissible_valleys = static_cast<uint32_t>(chains.size());
    std::vector<uint8_t> occupied(TOTAL, 0U);
    for (uint32_t rank = 0U; rank < 64U && result.plains.size() < 4U; ++rank)
    {
        checkMorphologyCancellation(cancellation);
        if (rank >= chains.size())
        {
            result.plain_ledger.push_back({rank, -1, V21PlainCandidateStatus::Exhausted});
            break;
        }
        V21PlainFeature feature = makePlain(chains[rank], rank, bed, reverse);
        std::vector<double> weight(TOTAL);
        bool overlap = false, has_core = false;
        double minimum_delta = 0.0, maximum_delta = 0.0;
        double eroded = 0.0, substrate_fill = 0.0, alluvium = 0.0;
        for (int32_t i = 0; i < TOTAL; ++i)
        {
            Point const p = position(i);
            double const w = v21PlainWeight(feature, p.x, p.y);
            weight[i] = w;
            overlap = overlap || (occupied[i] && w > 0.0);
            has_core = has_core || w > .999;
            double const target = plainTarget(feature, p.x, p.y);
            double const proposed = bed[i] * (1.0 - w) + target * w;
            minimum_delta = std::min(minimum_delta, proposed - bed[i]);
            maximum_delta = std::max(maximum_delta, proposed - bed[i]);
            double const substrate = bed[i] * (1.0 - w) + (target - 4.0) * w;
            eroded += std::max(bed[i] - substrate, 0.0);
            substrate_fill += std::max(substrate - bed[i], 0.0);
            alluvium += 4.0 * w;
        }
        feature.sediment_export_m3 = (eroded - substrate_fill - alluvium) * CELL * CELL;
        std::vector<uint8_t> local(55U * 65U);
        double const along_step = feature.length_m / 64.0;
        double const cross_step = 1700.0 / 54.0;
        auto const localPoint = [&](int32_t y, int32_t x) {
            double const s = x == 64 ? feature.length_m : x * along_step;
            double const d = y == 54 ? 850.0 : -850.0 + y * cross_step;
            return Point{
                feature.outlet_m[0] + s * feature.along[0] + d * feature.normal[0],
                feature.outlet_m[1] + s * feature.along[1] + d * feature.normal[1],
            };
        };
        for (int32_t y = 0; y < 55; ++y)
        {
            for (int32_t x = 0; x < 65; ++x)
            {
                Point const p = localPoint(y, x);
                local[y * 65 + x] = v21PlainWeight(feature, p.x, p.y) > .999;
            }
        }
        auto const local_distance = interiorDistance(local, 55, 65, cross_step, along_step);
        auto const center_index = std::max_element(local_distance.begin(), local_distance.end())
            - local_distance.begin();
        Point const center = localPoint(static_cast<int32_t>(center_index / 65),
            static_cast<int32_t>(center_index % 65));
        feature.inspection_center_m = {center.x, center.y};
        bool small_core = true;
        for (int32_t y = 0; y < 9; ++y)
        {
            for (int32_t x = 0; x < 9; ++x)
            {
                small_core = small_core && v21PlainWeight(feature,
                    center.x - 128.0 + x * 32.0, center.y - 128.0 + y * 32.0) > .999;
            }
        }
        uint32_t large_core = 0U;
        for (int32_t y = 0; y < 33; ++y)
        {
            for (int32_t x = 0; x < 33; ++x)
            {
                large_core += v21PlainWeight(feature,
                    center.x - 500.0 + x * 31.25, center.y - 500.0 + y * 31.25) > .999;
            }
        }
        feature.graded_core_1km_fraction = static_cast<double>(large_core) / (33U * 33U);
        V21PlainCandidateStatus status = V21PlainCandidateStatus::Selected;
        if (overlap)
        {
            status = V21PlainCandidateStatus::Overlap;
        }
        else if (!has_core)
        {
            status = V21PlainCandidateStatus::NoCanonicalInterior;
        }
        else if (!small_core || feature.graded_core_1km_fraction < .75)
        {
            status = V21PlainCandidateStatus::NoInspectionCore;
        }
        else if (minimum_delta < -200.0 || maximum_delta > 40.0)
        {
            status = V21PlainCandidateStatus::DisplacementCap;
        }
        else if (feature.sediment_export_m3 < 0.0)
        {
            status = V21PlainCandidateStatus::InsufficientMaterial;
        }
        result.plain_ledger.push_back({rank, feature.outlet_vertex, status});
        if (status == V21PlainCandidateStatus::Selected)
        {
            for (int32_t i = 0; i < TOTAL; ++i)
            {
                occupied[i] = occupied[i] || weight[i] > 0.0;
            }
            result.plains.push_back(std::move(feature));
        }
    }
    result.source_proxy_bed_m.resize(TOTAL);
    for (int32_t i = 0; i < TOTAL; ++i)
    {
        Point const p = position(i);
        result.source_proxy_bed_m[i] = v21ApplyPlainHistory(result.plains, bed[i], p.x, p.y);
    }
    return result;
}

} // namespace generation::detail
