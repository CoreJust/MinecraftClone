#include "V9TerrainMorphology.hpp"

#include "Numerics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
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

std::vector<double> evolveV9Terrain(
    std::span<double const> bed, std::span<uint8_t const> ocean, std::stop_token cancellation
) {
    if (bed.size() != TOTAL || ocean.size() != TOTAL
        || !std::all_of(bed.begin(), bed.end(), [](double height) { return std::isfinite(height); })) {
        throw std::invalid_argument("V9 terrain evolution requires a finite 256 by 256 bed and ocean mask");
    }
    checkMorphologyCancellation(cancellation);
    std::vector<Segment> const marine_shores = shores(bed, ocean, cancellation);
    if (marine_shores.empty()) {
        throw std::runtime_error("V9 terrain evolution requires an ocean shoreline");
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
