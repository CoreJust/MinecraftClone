#include "V10FineTerrain.hpp"

#include "Sha256.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace generation::detail
{
namespace
{

constexpr std::string_view SOURCE_ID =
    "canonical-parent-conditioned-fine-terrain/native-v2;"
    "oracle=6365ed7e79067c12fbc4d717e17ce2e1a239fbb672abf170ecf9cf4b1c255717;"
    "trial=e52e2e82b3e9873e14e543e0a7583fb978a182f1555997bda6febcd9ff25061a;"
    "draws-sha256=b308886ce670e6d23fc973a95b2eb633f96807c1b08641277211305abd3dc65b;"
    "regional-rng=numpy-pcg64-seedsequence-[42,0x52454732];"
    "regional-draws-sha256=262b0a3b6afd2f81b7322d89416e4cf01ffc87ed984b9e3c053dc8e86de75c2a";
constexpr size_t MOUNTAIN_SITE_COUNT = 64U;
constexpr size_t RING_SIZE = 8U;
constexpr size_t REGIONAL_GRID_SIDE = 256U;
constexpr size_t REGIONAL_SITE_COUNT = REGIONAL_GRID_SIDE * REGIONAL_GRID_SIDE;
constexpr size_t REGIONAL_DRAWS_PER_SITE = 8U;
constexpr size_t MOUNTAIN_DRAW_OFFSET = 0U;
constexpr size_t MOUNTAIN_FALLBACK_OFFSET = 128U;
constexpr size_t MOUNTAIN_LENGTH_OFFSET = 192U;
constexpr size_t MOUNTAIN_WIDTH_OFFSET = 256U;
constexpr size_t MOUNTAIN_AMPLITUDE_OFFSET = 320U;
constexpr size_t MOUNTAIN_PHASE_OFFSET = 384U;
constexpr size_t MOUNTAIN_PEAK_SPACING_OFFSET = 448U;
constexpr size_t RIVER_WIDTH_OFFSET = 512U;
constexpr size_t RIVER_PHASE_OFFSET = 576U;
constexpr size_t RIVER_TRIBUTARY_A_OFFSET = 640U;
constexpr size_t RIVER_TRIBUTARY_B_OFFSET = 704U;
constexpr double TAU = 2.0 * std::numbers::pi;
constexpr std::string_view DRAW_TABLE_SHA256 = "b308886ce670e6d23fc973a95b2eb633f96807c1b08641277211305abd3dc65b";
constexpr std::string_view REGIONAL_DRAW_TABLE_SHA256 = "262b0a3b6afd2f81b7322d89416e4cf01ffc87ed984b9e3c053dc8e86de75c2a";
// Exact unit draws from NumPy Generator(PCG64(SeedSequence([42, stream]))).
// Mountain and river blocks follow the offsets below; placements stay parent-derived.
constexpr std::array<double, 768U> RANDOM_DRAWS{{
    // DONT_CHECK INCLUDE_ORDER: the generated values belong inside this initializer.
#include "V10FineTerrainDraws.inc"
}};
static_assert(RIVER_TRIBUTARY_B_OFFSET + MOUNTAIN_SITE_COUNT == RANDOM_DRAWS.size());

struct Point final
{
    double x;
    double y;
};

struct MountainSite final
{
    double x;
    double y;
    double cosine;
    double sine;
    double length;
    double width;
    double amplitude;
    double phase;
    double peak_spacing;
};

struct ValleyNetwork final
{
    double x;
    double y;
    double cosine;
    double sine;
    double length;
    double width;
    double depth;
    double phase;
    double tributary_a;
    double tributary_b;
};

struct RegionalSite final
{
    double x;
    double y;
    double hill_cosine;
    double hill_sine;
    double hill_length;
    double hill_width;
    double hill_amplitude;
    double downhill_cosine;
    double downhill_sine;
    double valley_length;
    double valley_width;
    double valley_amplitude;
    double phase;
    double relief;
    double height;
};

struct ParentContext final
{
    double height;
    double slope;
    double shore_crossing;
};

struct RegionalRelief final
{
    double hills;
    double valleys;
    double coast;
    double compact_hills;
};

struct L2Parts final
{
    double correction;
    double ridge;
    double valley;
    double coast;
    double regional_hill;
    double regional_valley;
    double inland_uplift;
};

RegionalRelief regionalRelief(Point point, ParentContext context, std::vector<RegionalSite> const& sites);

double draw(size_t index)
{
    return RANDOM_DRAWS[index];
}

double uniform(size_t index, double low, double high)
{
    return low + (high - low) * draw(index);
}

double wrap(double coordinate)
{
    double result = std::fmod(coordinate, V10_FINE_TERRAIN_PERIOD_M);
    if (result < 0.0)
    {
        result += V10_FINE_TERRAIN_PERIOD_M;
    }
    return result >= V10_FINE_TERRAIN_PERIOD_M ? 0.0 : result;
}

double validateParentHeight(double const result)
{
    if (!std::isfinite(result))
    {
        throw std::invalid_argument("Fine terrain parent returned a non-finite bed height");
    }
    return result;
}

double queryParent(V10FineTerrainParent const& parent, double x, double y)
{
    return validateParentHeight(parent.bed(wrap(x), wrap(y)));
}

double smoothstep(double edge0, double edge1, double value)
{
    double const t = std::clamp((value - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

uint64_t mix64(uint64_t value)
{
    value = (value ^ (value >> 30U)) * 0xbf58'476d'1ce4'e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d0'49bb'1331'11ebULL;
    return value ^ (value >> 31U);
}

double latticeValue(int64_t ix, int64_t iy, uint32_t seed, uint64_t cells)
{
    uint64_t const wrapped_x = static_cast<uint64_t>(ix) % cells;
    uint64_t const wrapped_y = static_cast<uint64_t>(iy) % cells;
    uint64_t key = wrapped_x * 0x9e37'79b1'85eb'ca87ULL;
    key ^= wrapped_y * 0xc2b2'ae3d'27d4'eb4fULL;
    key ^= static_cast<uint64_t>(seed) * 0x1656'67b1'9e37'79f9ULL;
    uint64_t const bits = mix64(key) >> 11U;
    return static_cast<double>(bits) * (1.0 / 9'007'199'254'740'992.0) * 2.0 - 1.0;
}

double periodicNoise(Point point, uint32_t seed, double spacing)
{
    uint64_t const cells = static_cast<uint64_t>(std::llround(V10_FINE_TERRAIN_PERIOD_M / spacing));
    double const gx = point.x / spacing;
    double const gy = point.y / spacing;
    int64_t const ix = static_cast<int64_t>(std::floor(gx));
    int64_t const iy = static_cast<int64_t>(std::floor(gy));
    double tx = gx - static_cast<double>(ix);
    double ty = gy - static_cast<double>(iy);
    tx = tx * tx * tx * (tx * (tx * 6.0 - 15.0) + 10.0);
    ty = ty * ty * ty * (ty * (ty * 6.0 - 15.0) + 10.0);
    double const a = latticeValue(ix, iy, seed, cells);
    double const b = latticeValue(ix + 1, iy, seed, cells);
    double const c = latticeValue(ix, iy + 1, seed, cells);
    double const d = latticeValue(ix + 1, iy + 1, seed, cells);
    return (a * (1.0 - tx) + b * tx) * (1.0 - ty)
        + (c * (1.0 - tx) + d * tx) * ty;
}

Point toroidalDelta(Point point, Point center)
{
    double const dx = point.x - center.x;
    double const dy = point.y - center.y;
    return {
        dx - V10_FINE_TERRAIN_PERIOD_M * std::floor(dx / V10_FINE_TERRAIN_PERIOD_M + 0.5),
        dy - V10_FINE_TERRAIN_PERIOD_M * std::floor(dy / V10_FINE_TERRAIN_PERIOD_M + 0.5),
    };
}

std::vector<MountainSite> prepareMountainSites(V10FineTerrainParent const& parent, size_t& queries)
{
    constexpr double stride = V10_FINE_TERRAIN_PERIOD_M / 8.0;
    std::array<Point, MOUNTAIN_SITE_COUNT * (RING_SIZE + 1U)> candidates{};
    std::array<double, MOUNTAIN_SITE_COUNT> fallback_angles{};
    std::array<double, MOUNTAIN_SITE_COUNT> lengths{};
    std::array<double, MOUNTAIN_SITE_COUNT> widths{};
    std::array<double, MOUNTAIN_SITE_COUNT> amplitudes{};
    std::array<double, MOUNTAIN_SITE_COUNT> phases{};
    std::array<double, MOUNTAIN_SITE_COUNT> peak_spacings{};
    std::array<Point, RING_SIZE> ring{};
    for (size_t direction = 0U; direction < RING_SIZE; ++direction)
    {
        double const angle = static_cast<double>(direction) * (TAU / static_cast<double>(RING_SIZE));
        ring[direction] = {stride * 0.23 * std::cos(angle), stride * 0.23 * std::sin(angle)};
    }

    for (size_t cell_y = 0U; cell_y < 8U; ++cell_y)
    {
        for (size_t cell_x = 0U; cell_x < 8U; ++cell_x)
        {
            size_t const site = cell_y * 8U + cell_x;
            double const center_x = (static_cast<double>(cell_x) + 0.5) * stride
                + uniform(MOUNTAIN_DRAW_OFFSET + site * 2U, -stride * 0.18, stride * 0.18);
            double const center_y = (static_cast<double>(cell_y) + 0.5) * stride
                + uniform(MOUNTAIN_DRAW_OFFSET + site * 2U + 1U, -stride * 0.18, stride * 0.18);
            Point const center{wrap(center_x), wrap(center_y)};
            candidates[site * (RING_SIZE + 1U)] = center;
            for (size_t direction = 0U; direction < RING_SIZE; ++direction)
            {
                candidates[site * (RING_SIZE + 1U) + direction + 1U] = {
                    wrap(center.x + ring[direction].x), wrap(center.y + ring[direction].y)};
            }
        }
    }

    std::array<size_t, MOUNTAIN_SITE_COUNT> selected{};
    std::array<double, MOUNTAIN_SITE_COUNT * (RING_SIZE + 1U)> heights{};
    for (size_t i = 0U; i < candidates.size(); ++i)
    {
        heights[i] = queryParent(parent, candidates[i].x, candidates[i].y);
        ++queries;
    }
    std::array<Point, MOUNTAIN_SITE_COUNT> centers{};
    for (size_t site = 0U; site < MOUNTAIN_SITE_COUNT; ++site)
    {
        double highest = 0.0;
        bool found_land = false;
        size_t best = 0U;
        for (size_t candidate = 0U; candidate <= RING_SIZE; ++candidate)
        {
            double const height = heights[site * (RING_SIZE + 1U) + candidate];
            if (height > 0.0 && (!found_land || height > highest))
            {
                highest = height;
                best = candidate;
                found_land = true;
            }
        }
        selected[site] = best;
        centers[site] = candidates[site * (RING_SIZE + 1U) + best];
    }

    constexpr double step = 512.0;
    std::array<Point, MOUNTAIN_SITE_COUNT * 5U> gradient_points{};
    for (size_t site = 0U; site < MOUNTAIN_SITE_COUNT; ++site)
    {
        Point const center = centers[site];
        size_t const offset = site * 5U;
        gradient_points[offset] = center;
        gradient_points[offset + 1U] = {wrap(center.x + step), center.y};
        gradient_points[offset + 2U] = {wrap(center.x - step), center.y};
        gradient_points[offset + 3U] = {center.x, wrap(center.y + step)};
        gradient_points[offset + 4U] = {center.x, wrap(center.y - step)};
    }
    std::array<double, MOUNTAIN_SITE_COUNT * 5U> gradient_heights{};
    for (size_t i = 0U; i < gradient_points.size(); ++i)
    {
        gradient_heights[i] = queryParent(parent, gradient_points[i].x, gradient_points[i].y);
        ++queries;
    }

    for (size_t site = 0U; site < MOUNTAIN_SITE_COUNT; ++site)
    {
        fallback_angles[site] = uniform(MOUNTAIN_FALLBACK_OFFSET + site, 0.0, TAU);
    }
    for (size_t site = 0U; site < MOUNTAIN_SITE_COUNT; ++site)
    {
        lengths[site] = uniform(MOUNTAIN_LENGTH_OFFSET + site, 5800.0, 10'800.0);
    }
    for (size_t site = 0U; site < MOUNTAIN_SITE_COUNT; ++site)
    {
        widths[site] = uniform(MOUNTAIN_WIDTH_OFFSET + site, 180.0, 650.0);
    }
    for (size_t site = 0U; site < MOUNTAIN_SITE_COUNT; ++site)
    {
        amplitudes[site] = uniform(MOUNTAIN_AMPLITUDE_OFFSET + site, 60.0, 175.0);
    }
    for (size_t site = 0U; site < MOUNTAIN_SITE_COUNT; ++site)
    {
        phases[site] = uniform(MOUNTAIN_PHASE_OFFSET + site, 0.0, TAU);
    }
    for (size_t site = 0U; site < MOUNTAIN_SITE_COUNT; ++site)
    {
        peak_spacings[site] = uniform(MOUNTAIN_PEAK_SPACING_OFFSET + site, 1350.0, 3100.0);
    }
    for (size_t site = 0U; site < MOUNTAIN_SITE_COUNT; ++site)
    {
        size_t const offset = site * 5U;
        double const gx = (gradient_heights[offset + 1U] - gradient_heights[offset + 2U]) / (2.0 * step);
        double const gy = (gradient_heights[offset + 3U] - gradient_heights[offset + 4U]) / (2.0 * step);
        double const contour_angle = std::atan2(-gx, gy);
        double const angle = std::hypot(gx, gy) > 1e-6 ? contour_angle : fallback_angles[site];
        centers[site] = {std::cos(angle), std::sin(angle)};
    }

    std::vector<MountainSite> result;
    result.reserve(MOUNTAIN_SITE_COUNT);
    for (size_t site = 0U; site < MOUNTAIN_SITE_COUNT; ++site)
    {
        Point const center = candidates[site * (RING_SIZE + 1U) + selected[site]];
        result.push_back({center.x, center.y, centers[site].x, centers[site].y,
            lengths[site], widths[site], amplitudes[site], phases[site], peak_spacings[site]});
    }
    return result;
}

std::vector<ValleyNetwork> prepareValleyNetworks(
    V10FineTerrainParent const& parent, std::vector<MountainSite> const& mountains, size_t& queries
)
{
    std::array<Point, RING_SIZE> ring{};
    double const radius = V10_FINE_TERRAIN_PERIOD_M / 8.0 * 0.32;
    for (size_t direction = 0U; direction < RING_SIZE; ++direction)
    {
        double const angle = static_cast<double>(direction) * (TAU / static_cast<double>(RING_SIZE));
        ring[direction] = {radius * std::cos(angle), radius * std::sin(angle)};
    }
    std::array<Point, MOUNTAIN_SITE_COUNT * (RING_SIZE + 1U)> probes{};
    std::array<double, MOUNTAIN_SITE_COUNT * (RING_SIZE + 1U)> heights{};
    for (size_t site = 0U; site < mountains.size(); ++site)
    {
        Point const center{mountains[site].x, mountains[site].y};
        size_t const offset = site * (RING_SIZE + 1U);
        probes[offset] = center;
        for (size_t direction = 0U; direction < RING_SIZE; ++direction)
        {
            probes[offset + direction + 1U] = {
                wrap(center.x + ring[direction].x), wrap(center.y + ring[direction].y)};
        }
    }
    for (size_t i = 0U; i < probes.size(); ++i)
    {
        heights[i] = queryParent(parent, probes[i].x, probes[i].y);
        ++queries;
    }

    struct Candidate final
    {
        Point source;
        Point outlet;
        double source_height;
        double outlet_height;
        double length;
        double drop;
        bool valid;
    };
    std::array<Candidate, MOUNTAIN_SITE_COUNT> candidates{};
    for (size_t site = 0U; site < MOUNTAIN_SITE_COUNT; ++site)
    {
        size_t const offset = site * (RING_SIZE + 1U);
        double source_height = 0.0;
        double outlet_height = heights[offset];
        size_t source_index = 0U;
        size_t outlet_index = 0U;
        bool found_land = false;
        for (size_t probe = 0U; probe <= RING_SIZE; ++probe)
        {
            double const height = heights[offset + probe];
            if (height < outlet_height)
            {
                outlet_height = height;
                outlet_index = probe;
            }
            if (height > 0.0 && (!found_land || height > source_height))
            {
                source_height = height;
                source_index = probe;
                found_land = true;
            }
        }
        Point const source = probes[offset + source_index];
        Point const outlet = probes[offset + outlet_index];
        Point const delta = toroidalDelta(outlet, source);
        double const length = std::sqrt(delta.x * delta.x + delta.y * delta.y);
        double const drop = source_height - outlet_height;
        candidates[site] = {source, outlet, source_height, outlet_height, length, drop,
            found_land && length >= 900.0 && drop >= 4.0};
    }

    std::array<double, MOUNTAIN_SITE_COUNT> widths{};
    std::array<double, MOUNTAIN_SITE_COUNT> phases{};
    std::array<double, MOUNTAIN_SITE_COUNT> tributary_a{};
    std::array<double, MOUNTAIN_SITE_COUNT> tributary_b{};
    for (size_t site = 0U; site < MOUNTAIN_SITE_COUNT; ++site)
    {
        widths[site] = uniform(RIVER_WIDTH_OFFSET + site, 48.0, 165.0);
    }
    for (size_t site = 0U; site < MOUNTAIN_SITE_COUNT; ++site)
    {
        phases[site] = uniform(RIVER_PHASE_OFFSET + site, 0.0, TAU);
    }
    for (size_t site = 0U; site < MOUNTAIN_SITE_COUNT; ++site)
    {
        tributary_a[site] = draw(RIVER_TRIBUTARY_A_OFFSET + site);
    }
    for (size_t site = 0U; site < MOUNTAIN_SITE_COUNT; ++site)
    {
        tributary_b[site] = draw(RIVER_TRIBUTARY_B_OFFSET + site);
    }

    std::vector<ValleyNetwork> result;
    result.reserve(MOUNTAIN_SITE_COUNT);
    for (size_t site = 0U; site < MOUNTAIN_SITE_COUNT; ++site)
    {
        Candidate const& candidate = candidates[site];
        if (!candidate.valid)
        {
            continue;
        }
        Point const delta = toroidalDelta(candidate.outlet, candidate.source);
        double const depth = std::clamp(7.0 + 0.15 * std::max(candidate.drop, 0.0), 12.0, 58.0);
        result.push_back({wrap(candidate.source.x + delta.x * 0.5),
            wrap(candidate.source.y + delta.y * 0.5), delta.x / candidate.length, delta.y / candidate.length,
            candidate.length, widths[site], depth, phases[site], tributary_a[site], tributary_b[site]});
    }
    return result;
}

double orogenicRelief(
    Point point, double height, double slope, std::vector<MountainSite> const& mountains
)
{
    double const highland = smoothstep(200.0, 520.0, std::max(height, 0.0));
    double const rugged = smoothstep(0.035, 0.24, slope);
    double const response = highland * (0.30 + 0.70 * rugged);
    if (response == 0.0)
    {
        return 0.0;
    }

    double result = 0.0;
    for (MountainSite const& site : mountains)
    {
        Point const delta = toroidalDelta(point, {site.x, site.y});
        double const u = delta.x * site.cosine + delta.y * site.sine;
        double const v = -delta.x * site.sine + delta.y * site.cosine;
        double const envelope = smoothstep(0.0, 1.0, 1.0 - std::abs(u) / (site.length * 0.5));
        if (envelope == 0.0)
        {
            continue;
        }
        double const warp = 0.16 * site.width * std::sin(TAU * u / site.length + site.phase);
        double const summit = 0.73 + 0.27 * (0.5 + 0.5 * std::cos(TAU * u / site.peak_spacing + site.phase));
        double const main = site.amplitude * envelope * summit
            * std::exp(-0.5 * std::pow((v - warp) / site.width, 2.0));
        double const flank_l = 0.34 * site.amplitude * envelope
            * std::exp(-0.5 * std::pow((v - warp - 1.45 * site.width) / (0.72 * site.width), 2.0));
        double const flank_r = 0.29 * site.amplitude * envelope
            * std::exp(-0.5 * std::pow((v - warp + 1.65 * site.width) / (0.78 * site.width), 2.0));
        result = std::max(result, std::max(main, std::max(flank_l, flank_r)));
    }
    return result * response;
}

double valleyRelief(Point point, std::vector<ValleyNetwork> const& networks)
{
    double relief = 0.0;
    for (ValleyNetwork const& network : networks)
    {
        Point const delta = toroidalDelta(point, {network.x, network.y});
        double const u = delta.x * network.cosine + delta.y * network.sine;
        double const v = -delta.x * network.sine + delta.y * network.cosine;
        double const half = network.length * 0.5;
        double const t = std::clamp((u + half) / network.length, 0.0, 1.0);
        double const centerline = 0.055 * network.width * std::sin(TAU * t + network.phase);
        double const channel_width = network.width * (0.12 + 0.12 * t);
        double const valley_width = network.width * (0.78 + 0.88 * t);
        double const distance = std::abs(v - centerline);
        double main = 0.78 * network.depth * std::exp(-0.5 * std::pow(distance / valley_width, 2.0));
        main += 0.22 * network.depth * std::exp(-0.5 * std::pow(distance / channel_width, 2.0));
        main *= smoothstep(0.0, 0.10 * network.length, u + half);
        main *= smoothstep(0.0, 0.10 * network.length, half - u);
        double branches = 0.0;
        for (size_t branch_index = 0U; branch_index < 4U; ++branch_index)
        {
            double const join_t = 0.11 + 0.17 * static_cast<double>(branch_index) + 0.08 * network.tributary_a;
            double const start_t = std::max(0.02, join_t - (0.27 + 0.12 * network.tributary_b));
            double const side = branch_index % 2U == 0U ? 1.0 : -1.0;
            double const u0 = -half + start_t * network.length;
            double const u1 = -half + join_t * network.length;
            double const v0 = side * network.width
                * (1.8 + 0.25 * static_cast<double>(branch_index) + 0.35 * network.tributary_b);
            double const du = u1 - u0;
            double const dv = -v0;
            double const fraction = std::clamp(((u - u0) * du + (v - v0) * dv) / (du * du + dv * dv), 0.0, 1.0);
            double const branch_distance = std::hypot(u - (u0 + fraction * du), v - (v0 + fraction * dv));
            double const branch_width = network.width * (0.36 + 0.08 * static_cast<double>(branch_index));
            double branch_valley = 0.38 * network.depth
                * std::exp(-0.5 * std::pow(branch_distance / (branch_width * 1.8), 2.0));
            branch_valley *= smoothstep(0.0, 0.14, fraction);
            branches = std::max(branches, branch_valley);
        }
        relief = std::max(relief, std::max(main, branches));
    }
    return relief;
}

L2Parts l2Correction(
    Point point, ParentContext context, std::vector<MountainSite> const& mountains,
    std::vector<ValleyNetwork> const& networks, std::vector<RegionalSite> const& regional_sites
)
{
    uint32_t const seed = V10_FINE_TERRAIN_SEED;
    double const noise = 5.0 * periodicNoise(point, seed ^ 0x4c32'01U, 4096.0)
        + 2.2 * periodicNoise(point, seed ^ 0x4c32'02U, 1024.0)
        + 0.85 * periodicNoise(point, seed ^ 0x4c32'03U, 256.0)
        + 0.30 * periodicNoise(point, seed ^ 0x4c32'04U, 64.0);
    double const highland = smoothstep(200.0, 520.0, std::max(context.height, 0.0));
    double const parent_texture = smoothstep(0.002, 0.018, context.slope);
    double const plain_relief = noise * (0.25 + 0.45 * (1.0 - highland)) * parent_texture;
    double const ridge = orogenicRelief(point, context.height, context.slope, mountains);
    double const valley = valleyRelief(point, networks);
    RegionalRelief const regional = regionalRelief(point, context, regional_sites);
    double const land = context.height > 0.0 ? smoothstep(0.0, 2.0, context.height) : 0.0;
    double correction = (plain_relief + ridge + regional.hills + regional.coast - valley - regional.valleys) * land;
    double const minimum_bed = 0.42 * std::max(context.height, 0.0);
    correction = std::max(correction, minimum_bed - std::max(context.height, 0.0));
    return {correction, ridge * land, (valley + regional.valleys) * land, regional.coast * land,
        regional.hills * land, regional.valleys * land,
        regional.compact_hills * smoothstep(35.0, 85.0, context.height) * land};
}

double nestedGullyRelief(Point point, std::vector<ValleyNetwork> const& networks)
{
    double relief = 0.0;
    for (ValleyNetwork const& network : networks)
    {
        Point const delta = toroidalDelta(point, {network.x, network.y});
        double const u = delta.x * network.cosine + delta.y * network.sine;
        double const v = -delta.x * network.sine + delta.y * network.cosine;
        double const half = network.length * 0.5;
        for (size_t branch_index = 0U; branch_index < 6U; ++branch_index)
        {
            double const join_t = 0.10 + 0.105 * static_cast<double>(branch_index)
                + 0.04 * network.tributary_a;
            double const start_t = std::max(0.015, join_t - (0.11 + 0.07 * network.tributary_b));
            double const side = branch_index % 2U == 0U ? 1.0 : -1.0;
            double const u0 = -half + start_t * network.length;
            double const u1 = -half + join_t * network.length;
            double const v0 = side * network.width * (0.42 + 0.11 * static_cast<double>(branch_index));
            double const du = u1 - u0;
            double const dv = -v0;
            double const fraction = std::clamp(((u - u0) * du + (v - v0) * dv) / (du * du + dv * dv), 0.0, 1.0);
            double const distance = std::hypot(u - (u0 + fraction * du), v - (v0 + fraction * dv));
            double const gully_width = network.width * (0.10 + 0.025 * static_cast<double>(branch_index));
            double gully = std::min(8.0, network.depth * 0.18)
                * std::exp(-0.5 * std::pow(distance / (gully_width * 1.8), 2.0));
            gully *= smoothstep(0.0, 0.12, fraction);
            relief = std::max(relief, gully);
        }
    }
    return relief;
}

double l3Detail(Point point, double height, double slope, double ridge,
    std::vector<ValleyNetwork> const& networks)
{
    uint32_t const seed = V10_FINE_TERRAIN_SEED;
    double const highland = smoothstep(80.0, 340.0, std::max(height, 0.0));
    double const exposure = std::clamp(
        0.92 * highland + 0.8 * smoothstep(0.025, 0.12, slope), 0.0, 1.0);
    double const fine = 2.9 * periodicNoise(point, seed ^ 0x4c33'01U, 512.0)
        + 1.45 * periodicNoise(point, seed ^ 0x4c33'02U, 128.0)
        + 0.68 * periodicNoise(point, seed ^ 0x4c33'03U, 32.0)
        + 0.30 * periodicNoise(point, seed ^ 0x4c33'04U, 8.0)
        + 0.12 * periodicNoise(point, seed ^ 0x4c33'05U, 2.0);
    double const ridged = 1.0 - std::abs(periodicNoise(point, seed ^ 0x5441'4c55U, 128.0));
    double const talus = 3.6 * ridged * std::exp(-0.5 * std::pow((ridge - 20.0) / 35.0, 2.0)) * highland;
    double const nested_gully = nestedGullyRelief(point, networks);
    return exposure * fine + talus - nested_gully;
}

void appendU64(std::vector<uint8_t>& bytes, uint64_t value)
{
    for (uint32_t shift = 0U; shift < 64U; shift += 8U)
    {
        bytes.push_back(static_cast<uint8_t>(value >> shift));
    }
}

void appendString(std::vector<uint8_t>& bytes, std::string_view value)
{
    appendU64(bytes, static_cast<uint64_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

void appendDouble(std::vector<uint8_t>& bytes, double value)
{
    appendU64(bytes, std::bit_cast<uint64_t>(value));
}

std::string sha256Hex(std::span<uint8_t const> bytes)
{
    constexpr char HEX[] = "0123456789abcdef";
    std::string result;
    result.reserve(64U);
    for (uint8_t const byte : sha256Digest(bytes))
    {
        result.push_back(HEX[byte >> 4U]);
        result.push_back(HEX[byte & 0x0fU]);
    }
    return result;
}

uint64_t multiplyHigh64(uint64_t left, uint64_t right) noexcept
{
    uint64_t const left_low = static_cast<uint32_t>(left);
    uint64_t const left_high = left >> 32U;
    uint64_t const right_low = static_cast<uint32_t>(right);
    uint64_t const right_high = right >> 32U;
    uint64_t const product_low = left_low * right_low;
    uint64_t const product_middle_a = left_high * right_low;
    uint64_t const product_middle_b = left_low * right_high;
    uint64_t const carry = ((product_low >> 32U) + static_cast<uint32_t>(product_middle_a)
        + static_cast<uint32_t>(product_middle_b)) >> 32U;
    return left_high * right_high + (product_middle_a >> 32U) + (product_middle_b >> 32U) + carry;
}

class RegionalPcg64 final
{
public:
    uint64_t next() noexcept
    {
        uint64_t const previous_low = state_low;
        uint64_t const previous_high = state_high;
        uint64_t const product_low = previous_low * MULTIPLIER_LOW;
        uint64_t const product_high = multiplyHigh64(previous_low, MULTIPLIER_LOW)
            + previous_high * MULTIPLIER_LOW + previous_low * MULTIPLIER_HIGH;
        state_low = product_low + INCREMENT_LOW;
        state_high = product_high + INCREMENT_HIGH + static_cast<uint64_t>(state_low < product_low);
        uint64_t const rotated = state_high ^ state_low;
        return std::rotr(rotated, static_cast<int>(state_high >> 58U));
    }

    double nextUnit() noexcept
    {
        return static_cast<double>(next() >> 11U) * (1.0 / 9'007'199'254'740'992.0);
    }

private:
    static constexpr uint64_t MULTIPLIER_LOW = 0x4385'df64'9fcc'f645ULL;
    static constexpr uint64_t MULTIPLIER_HIGH = 0x2360'ed05'1fc6'5da4ULL;
    static constexpr uint64_t INITIAL_STATE_LOW = 0x4a02'52de'717f'7c34ULL;
    static constexpr uint64_t INITIAL_STATE_HIGH = 0x6131'932a'365b'f316ULL;
    static constexpr uint64_t INCREMENT_LOW = 0x95f6'4da9'38bc'3075ULL;
    static constexpr uint64_t INCREMENT_HIGH = 0x7691'0da8'584f'568fULL;
    uint64_t state_low = INITIAL_STATE_LOW;
    uint64_t state_high = INITIAL_STATE_HIGH;
};

std::vector<RegionalSite> prepareRegionalSites(V10FineTerrainParent const& parent, size_t& queries,
    std::string& draws_hash, std::string& feature_hash)
{
    constexpr double cell = V10_FINE_TERRAIN_PERIOD_M / static_cast<double>(REGIONAL_GRID_SIDE);
    constexpr double step = 256.0;
    constexpr double diagonal = step * 0.707'106'781'186'547'6;
    constexpr std::array<Point, 9U> RING{{
        {0.0, 0.0}, {step, 0.0}, {-step, 0.0}, {0.0, step}, {0.0, -step},
        {diagonal, diagonal}, {-diagonal, diagonal}, {diagonal, -diagonal}, {-diagonal, -diagonal},
    }};
    RegionalPcg64 generator;
    std::vector<uint8_t> draw_bytes;
    draw_bytes.reserve(REGIONAL_SITE_COUNT * REGIONAL_DRAWS_PER_SITE * sizeof(double));
    auto const next_unit = [&]() {
        double const value = generator.nextUnit();
        appendDouble(draw_bytes, value);
        return value;
    };
    auto const uniform = [&](double const low, double const high) {
        return low + (high - low) * next_unit();
    };

    std::vector<RegionalSite> sites(REGIONAL_SITE_COUNT);
    std::vector<std::array<double, RING.size()>> neighborhoods(REGIONAL_SITE_COUNT);
    for (size_t index = 0U; index < REGIONAL_SITE_COUNT; ++index)
    {
        size_t const cell_x = index % REGIONAL_GRID_SIDE;
        size_t const cell_y = index / REGIONAL_GRID_SIDE;
        RegionalSite& site = sites[index];
        site.x = wrap((static_cast<double>(cell_x) + 0.5) * cell
            + uniform(-0.19 * cell, 0.19 * cell));
        site.y = wrap((static_cast<double>(cell_y) + 0.5) * cell
            + uniform(-0.19 * cell, 0.19 * cell));
        auto& heights = neighborhoods[index];
        double minimum_height = std::numeric_limits<double>::infinity();
        double maximum_height = -std::numeric_limits<double>::infinity();
        for (size_t probe = 0U; probe < RING.size(); ++probe)
        {
            heights[probe] = queryParent(parent, site.x + RING[probe].x, site.y + RING[probe].y);
            ++queries;
            minimum_height = std::min(minimum_height, heights[probe]);
            maximum_height = std::max(maximum_height, heights[probe]);
        }
        double const lowland = 1.0 - smoothstep(120.0, 280.0, std::max(heights[0U], 0.0));
        double const dry = smoothstep(1.0, 8.0, heights[0U]);
        double const relief = maximum_height - minimum_height;
        double const terrain_gate = smoothstep(0.9, 6.0, relief);
        double const valley_gate = smoothstep(1.5, 8.0, relief);
        site.height = heights[0U];
        site.relief = relief;
        site.hill_amplitude = std::clamp(3.0 + 0.30 * (relief - 0.9), 0.0, 22.0)
            * lowland * dry * terrain_gate;
        site.valley_amplitude = std::clamp(2.0 + 0.20 * (relief - 1.5), 0.0, 14.0)
            * lowland * dry * valley_gate;
    }
    std::vector<double> fallback_angles(REGIONAL_SITE_COUNT);
    for (double& angle : fallback_angles)
    {
        angle = uniform(0.0, TAU);
    }
    for (size_t index = 0U; index < REGIONAL_SITE_COUNT; ++index)
    {
        RegionalSite& site = sites[index];
        auto const& heights = neighborhoods[index];
        double const gx = (heights[1U] - heights[2U]) / (2.0 * step);
        double const gy = (heights[3U] - heights[4U]) / (2.0 * step);
        double const slope = std::hypot(gx, gy);
        double const angle = slope > 1e-5 ? std::atan2(-gx, gy) : fallback_angles[index];
        site.hill_cosine = std::cos(angle);
        site.hill_sine = std::sin(angle);
        site.downhill_cosine = slope > 1e-5 ? -gx / std::max(slope, 1e-12) : std::cos(fallback_angles[index]);
        site.downhill_sine = slope > 1e-5 ? -gy / std::max(slope, 1e-12) : std::sin(fallback_angles[index]);
    }
    for (RegionalSite& site : sites)
    {
        site.hill_length = uniform(180.0, 480.0);
    }
    for (RegionalSite& site : sites)
    {
        site.hill_width = uniform(42.0, 128.0);
    }
    for (RegionalSite& site : sites)
    {
        site.valley_length = uniform(240.0, 620.0);
    }
    for (RegionalSite& site : sites)
    {
        site.valley_width = uniform(34.0, 112.0);
    }
    for (RegionalSite& site : sites)
    {
        site.phase = uniform(0.0, TAU);
    }
    draws_hash = sha256Hex(draw_bytes);
    if (draws_hash != REGIONAL_DRAW_TABLE_SHA256)
    {
        throw std::logic_error("V10 fine terrain regional PCG64 stream differs from its source identity");
    }
    std::vector<uint8_t> feature_bytes;
    feature_bytes.reserve(sites.size() * 15U * sizeof(double));
    for (RegionalSite const& site : sites)
    {
        for (double const value : {site.x, site.y, site.hill_cosine, site.hill_sine, site.hill_length,
                site.hill_width, site.hill_amplitude, site.downhill_cosine, site.downhill_sine,
                site.valley_length, site.valley_width, site.valley_amplitude, site.phase, site.relief, site.height})
        {
            appendDouble(feature_bytes, value);
        }
    }
    feature_hash = sha256Hex(feature_bytes);
    return sites;
}

ParentContext sampleParentContext(Point point, V10FineTerrainParent const& parent,
    std::optional<double> parent_center_height = std::nullopt, size_t* query_count = nullptr)
{
    constexpr double step = 256.0;
    constexpr double diagonal = step * 0.707'106'781'186'547'6;
    constexpr std::array<Point, 9U> RING{{
        {0.0, 0.0}, {step, 0.0}, {-step, 0.0}, {0.0, step}, {0.0, -step},
        {diagonal, diagonal}, {-diagonal, diagonal}, {diagonal, -diagonal}, {-diagonal, -diagonal},
    }};
    std::array<double, RING.size()> heights{};
    size_t const first_query = parent_center_height.has_value() ? 1U : 0U;
    if (parent_center_height.has_value())
    {
        heights[0U] = validateParentHeight(*parent_center_height);
    }
    for (size_t index = first_query; index < RING.size(); ++index)
    {
        heights[index] = queryParent(parent, point.x + RING[index].x, point.y + RING[index].y);
        if (query_count != nullptr)
        {
            ++*query_count;
        }
    }
    double const gx = (heights[1U] - heights[2U]) / (2.0 * step);
    double const gy = (heights[3U] - heights[4U]) / (2.0 * step);
    double const slope = std::hypot(gx, gy);
    double negative_margin = 0.0;
    for (size_t index = 1U; index < heights.size(); ++index)
    {
        negative_margin = std::max(negative_margin, -heights[index]);
    }
    double const normalized_margin = std::max(negative_margin, 0.0) / std::max(slope * step, 1e-12);
    double const shore_crossing = smoothstep(0.0, 0.30, normalized_margin) * (heights[0U] > 0.0 ? 1.0 : 0.0);
    return {heights[0U], slope, shore_crossing};
}

double quinticSmoothstep(double edge0, double edge1, double value)
{
    double const t = std::clamp((value - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

RegionalRelief regionalRelief(Point point, ParentContext context, std::vector<RegionalSite> const& sites)
{
    constexpr double cell = V10_FINE_TERRAIN_PERIOD_M / static_cast<double>(REGIONAL_GRID_SIDE);
    double hills = 0.0;
    double valleys = 0.0;
    double compact_hills = 0.0;
    int64_t const base_x = static_cast<int64_t>(std::floor(point.x / cell));
    int64_t const base_y = static_cast<int64_t>(std::floor(point.y / cell));
    for (int64_t dy = -1; dy <= 1; ++dy)
    {
        for (int64_t dx = -1; dx <= 1; ++dx)
        {
            size_t const site_x = static_cast<size_t>((base_x + dx + static_cast<int64_t>(REGIONAL_GRID_SIDE))
                % static_cast<int64_t>(REGIONAL_GRID_SIDE));
            size_t const site_y = static_cast<size_t>((base_y + dy + static_cast<int64_t>(REGIONAL_GRID_SIDE))
                % static_cast<int64_t>(REGIONAL_GRID_SIDE));
            RegionalSite const& site = sites[site_y * REGIONAL_GRID_SIDE + site_x];
            Point const delta = toroidalDelta(point, {site.x, site.y});
            double const radius = std::hypot(delta.x, delta.y);
            double const support = 1.0 - quinticSmoothstep(cell, 320.0, radius);
            double const u = delta.x * site.hill_cosine + delta.y * site.hill_sine;
            double const v = -delta.x * site.hill_sine + delta.y * site.hill_cosine;
            double const length = site.hill_length;
            double const width = site.hill_width;
            double const window = 1.0 - smoothstep(0.40 * length, 0.56 * length, std::abs(u));
            double const peak_a = std::exp(-0.5 * (std::pow((u - 0.22 * length) / (0.12 * length), 2.0)
                + std::pow(v / (0.72 * width), 2.0)));
            double const peak_b = std::exp(-0.5 * (std::pow((u + 0.22 * length) / (0.12 * length), 2.0)
                + std::pow(v / (0.72 * width), 2.0)));
            double const spine = 0.18 * std::exp(-0.5 * std::pow(v / width, 2.0));
            double const flanks = 0.17 * (std::exp(-0.5 * std::pow((v - 1.55 * width) / (0.75 * width), 2.0))
                + std::exp(-0.5 * std::pow((v + 1.55 * width) / (0.75 * width), 2.0)));
            double const saddle = 0.19 * std::exp(-0.5 * (std::pow(u / (0.11 * length), 2.0)
                + std::pow(v / (0.56 * width), 2.0)));
            hills += site.hill_amplitude * window * (spine + 0.68 * (peak_a + peak_b) + flanks - saddle) * support;

            // One curved interfluve and its adjacent draw share the same smooth support.
            double const turn = 0.48 + 0.18 * std::sin(site.phase);
            double const along = (u + turn * v) / (1.35 * length);
            double const bend = width * (0.33 * std::sin(3.4 * along + site.phase) + 0.22 * along);
            double const across = (v - turn * u - bend) / (width * (1.10 + 0.14 * std::sin(site.phase)));
            double const along_weight = quinticSmoothstep(0.92, 0.18, std::abs(along));
            double const crest = quinticSmoothstep(1.45, 0.0, std::abs(across));
            double const side = std::cos(site.phase) < 0.0 ? -1.0 : 1.0;
            double const draw = quinticSmoothstep(1.28, 0.0, std::abs(across - side * 1.35));
            compact_hills += 0.88 * site.hill_amplitude * along_weight * (crest - 0.68 * draw) * support;

            double const valley_u = delta.x * site.downhill_cosine + delta.y * site.downhill_sine;
            double const valley_v = -delta.x * site.downhill_sine + delta.y * site.downhill_cosine;
            double const t = std::clamp(valley_u / site.valley_length + 0.5, 0.0, 1.0);
            double const centerline = 0.13 * site.valley_width * std::sin(TAU * t + site.phase);
            double const local_width = site.valley_width * (0.75 + 0.45 * t);
            double const valley_window = 1.0 - smoothstep(0.40 * site.valley_length,
                0.55 * site.valley_length, std::abs(valley_u));
            double const incision = site.valley_amplitude * valley_window
                * (0.72 * std::exp(-0.5 * std::pow((valley_v - centerline) / local_width, 2.0))
                    + 0.28 * std::exp(-0.5 * std::pow((valley_v - centerline) / (0.32 * local_width), 2.0)))
                * support;
            valleys += incision;
        }
    }
    hills = 22.0 * std::tanh(hills / 22.0);
    valleys = 14.0 * std::tanh(valleys / 14.0);
    double const distance = std::max(context.height, 0.0) / std::max(context.slope, 0.006);
    double const shore_weight = context.shore_crossing * smoothstep(520.0, 120.0, distance)
        * smoothstep(0.006, 0.018, context.slope);
    hills *= 1.0 - shore_weight;
    valleys *= 1.0 - shore_weight;
    double const berm = 7.0 * std::exp(-0.5 * std::pow((distance - 42.0) / 26.0, 2.0));
    double const swale = 3.8 * std::exp(-0.5 * std::pow((distance - 92.0) / 38.0, 2.0));
    double const bluff = 10.0 * std::exp(-0.5 * std::pow((distance - 184.0) / 48.0, 2.0));
    double const coast = shore_weight * (berm - swale + bluff);
    double const inland_compact_hills = 12.0 * std::tanh(compact_hills / 12.0)
        * (1.0 - shore_weight);
    return {hills, valleys, coast, inland_compact_hills};
}

std::string randomDrawsHash()
{
    std::vector<uint8_t> bytes;
    bytes.reserve(RANDOM_DRAWS.size() * sizeof(double));
    for (double const value : RANDOM_DRAWS)
    {
        appendDouble(bytes, value);
    }
    return sha256Hex(bytes);
}

std::string identityFor(
    std::string_view level, std::string_view operators, std::string_view parent_identity,
    std::string_view plan_hash
)
{
    std::vector<uint8_t> bytes;
    appendString(bytes, SOURCE_ID);
    appendString(bytes, level);
    appendString(bytes, operators);
    appendString(bytes, parent_identity);
    appendU64(bytes, V10_FINE_TERRAIN_SEED);
    appendDouble(bytes, V10_FINE_TERRAIN_PERIOD_M);
    appendString(bytes, plan_hash);
    return sha256Hex(bytes);
}

} // namespace

struct V10FineTerrain::Impl final
{
    V10FineTerrainParent parent;
    std::vector<MountainSite> mountains;
    std::vector<ValleyNetwork> networks;
    std::vector<RegionalSite> regional_sites;
    V10FineTerrainReceipt receipt;

    [[nodiscard]] double bed(double x_m, double y_m, V10FineTerrainLevel level,
        std::optional<double> parent_center_height = std::nullopt, double* inland_uplift = nullptr) const
    {
        if (!std::isfinite(x_m) || !std::isfinite(y_m))
        {
            throw std::invalid_argument("Fine terrain requires finite XY coordinates");
        }
        if (level != V10FineTerrainLevel::L2 && level != V10FineTerrainLevel::L3)
        {
            throw std::invalid_argument("Fine terrain level must be exactly L2 or L3");
        }
        Point const point{wrap(x_m), wrap(y_m)};
        ParentContext const context = sampleParentContext(point, parent, parent_center_height);
        double const height = context.height;
        L2Parts const l2 = l2Correction(point, context, mountains, networks, regional_sites);
        if (inland_uplift)
        {
            *inland_uplift = l2.inland_uplift;
        }
        if (level == V10FineTerrainLevel::L2)
        {
            return height + l2.correction;
        }
        double const land = height > 0.0 ? smoothstep(0.0, 2.0, height) : 0.0;
        double l3 = l3Detail(point, height, context.slope, l2.ridge, networks) * land;
        double const minimum_bed = 0.42 * std::max(height, 0.0);
        double const l3_floor = height > 0.0 ? minimum_bed - (height + l2.correction) : 0.0;
        l3 = std::max(l3, l3_floor);
        return height + l2.correction + l3;
    }
};

V10FineTerrain::V10FineTerrain(V10FineTerrainParent parent)
{
    if (randomDrawsHash() != DRAW_TABLE_SHA256)
    {
        throw std::logic_error("V10 fine terrain random draw table differs from its source identity");
    }
    if (parent.seed != V10_FINE_TERRAIN_SEED)
    {
        throw std::invalid_argument("V10 fine terrain currently supports only seed 42");
    }
    if (parent.period_m != V10_FINE_TERRAIN_PERIOD_M)
    {
        throw std::invalid_argument("V10 fine terrain parent period must be exactly 65536 m");
    }
    if (parent.identity_sha256.empty())
    {
        throw std::invalid_argument("V10 fine terrain parent identity is required");
    }
    if (!parent.bed)
    {
        throw std::invalid_argument("V10 fine terrain parent bed sampler is required");
    }

    auto impl = std::make_unique<Impl>();
    impl->parent = std::move(parent);
    size_t queries = 0U;
    std::string regional_draws_hash;
    std::string regional_feature_hash;
    impl->mountains = prepareMountainSites(impl->parent, queries);
    impl->networks = prepareValleyNetworks(impl->parent, impl->mountains, queries);
    impl->regional_sites = prepareRegionalSites(impl->parent, queries,
        regional_draws_hash, regional_feature_hash);
    std::vector<uint8_t> plan_bytes;
    appendString(plan_bytes, SOURCE_ID);
    appendString(plan_bytes, impl->parent.identity_sha256);
    appendU64(plan_bytes, V10_FINE_TERRAIN_SEED);
    appendDouble(plan_bytes, V10_FINE_TERRAIN_PERIOD_M);
    appendU64(plan_bytes, static_cast<uint64_t>(impl->mountains.size()));
    for (MountainSite const& site : impl->mountains)
    {
        for (double value : {site.x, site.y, site.cosine, site.sine, site.length, site.width,
                site.amplitude, site.phase, site.peak_spacing})
        {
            appendDouble(plan_bytes, value);
        }
    }
    appendU64(plan_bytes, static_cast<uint64_t>(impl->networks.size()));
    for (ValleyNetwork const& network : impl->networks)
    {
        for (double value : {network.x, network.y, network.cosine, network.sine, network.length,
                network.width, network.depth, network.phase, network.tributary_a, network.tributary_b})
        {
            appendDouble(plan_bytes, value);
        }
    }
    appendU64(plan_bytes, static_cast<uint64_t>(impl->regional_sites.size()));
    for (RegionalSite const& site : impl->regional_sites)
    {
        for (double const value : {site.x, site.y, site.hill_cosine, site.hill_sine, site.hill_length,
                site.hill_width, site.hill_amplitude, site.downhill_cosine, site.downhill_sine,
                site.valley_length, site.valley_width, site.valley_amplitude, site.phase, site.relief, site.height})
        {
            appendDouble(plan_bytes, value);
        }
    }
    std::string const plan_hash = sha256Hex(plan_bytes);
    std::string const l2_identity = identityFor("L2",
        "orogenic-ridges;parent-conditioned-regional-hills-and-downhill-swales;dry-valley-cuts;signed-height-coast",
        impl->parent.identity_sha256, plan_hash);
    std::string const l3_identity = identityFor("L3",
        "L2;nested-gullies;talus;sub-L2-rock-relief;parent-conditioned-regional-forms",
        impl->parent.identity_sha256, plan_hash);
    std::vector<uint8_t> field_bytes;
    appendString(field_bytes, l2_identity);
    appendString(field_bytes, l3_identity);
    impl->receipt = {
        .source_id = std::string{SOURCE_ID},
        .parent_identity_sha256 = impl->parent.identity_sha256,
        .field_identity_sha256 = sha256Hex(field_bytes),
        .l2_identity_sha256 = l2_identity,
        .l3_identity_sha256 = l3_identity,
        .plan_sha256 = plan_hash,
        .prepared_parent_queries = queries,
        .mountain_site_count = impl->mountains.size(),
        .valley_network_count = impl->networks.size(),
        .regional_site_count = impl->regional_sites.size(),
        .regional_draws_sha256 = std::move(regional_draws_hash),
        .regional_feature_sha256 = std::move(regional_feature_hash),
    };
    m_impl = std::move(impl);
}

V10FineTerrain::~V10FineTerrain() = default;
V10FineTerrain::V10FineTerrain(V10FineTerrain&&) noexcept = default;
V10FineTerrain& V10FineTerrain::operator=(V10FineTerrain&&) noexcept = default;

double V10FineTerrain::bed(double x_m, double y_m, V10FineTerrainLevel level) const
{
    return m_impl->bed(x_m, y_m, level);
}

double V10FineTerrain::bedWithParentCenter(
    double x_m, double y_m, V10FineTerrainLevel level, double const parent_center_height) const
{
    return m_impl->bed(x_m, y_m, level, parent_center_height);
}

double V10FineTerrain::bedWithParentCenterAndUplift(
    double x_m, double y_m, V10FineTerrainLevel level, double parent_center_height, double& uplift
) const
{
    return m_impl->bed(x_m, y_m, level, parent_center_height, &uplift);
}

V10FineTerrainReceipt const& V10FineTerrain::receipt() const noexcept
{
    return m_impl->receipt;
}

} // namespace generation::detail
