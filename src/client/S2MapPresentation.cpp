#include <client/S2MapPresentation.hpp>

#include <client/GenerationMapPresentation.hpp>
#include <client/render/TextRenderer.hpp>

#include <generation/Generation.hpp>
#include <generation/RiverBedChannel.hpp>
#include <generation/V10Provider.hpp>
#include <generation/V9Provider.hpp>

#include <core/IO/Log.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace client {
struct S2RiverMapGeometry
{
    struct Polygon
    {
        std::vector<std::array<double, 2U>> xy;
        std::array<float, 3U> color{};
        bool standing = false;
        bool cold = false;
        bool contact = false;
        std::array<double, 4U> bounds{};
    };
    std::shared_ptr<generation::GlobalAtlas const> source;
    std::vector<Polygon> polygons;
    mutable std::timed_mutex mutex;
    mutable std::vector<uint32_t> cache_key;
    mutable std::shared_ptr<S2MapImage const> cache;
};

namespace
{
using RiverPoint = std::array<double, 2U>;
using RiverPolygon = std::vector<RiverPoint>;

RiverPolygon clipRiverPolygon(RiverPolygon const& input, size_t axis, double bound, bool lower)
{
    RiverPolygon result;
    if (input.empty())
    {
        return result;
    }
    RiverPoint a = input.back();
    bool inside_a = lower ? a[axis] >= bound : a[axis] <= bound;
    for (auto const& b : input)
    {
        bool const inside_b = lower ? b[axis] >= bound : b[axis] <= bound;
        if (inside_a != inside_b)
        {
            double const t = (bound - a[axis]) / (b[axis] - a[axis]);
            result.push_back({a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])});
        }
        if (inside_b)
        {
            result.push_back(b);
        }
        a = b;
        inside_a = inside_b;
    }
    return result;
}

double riverPolygonArea(RiverPolygon const& polygon)
{
    double area = 0.0;
    for (size_t i = 0U; i < polygon.size(); ++i)
    {
        auto const& a = polygon[i];
        auto const& b = polygon[(i + 1U) % polygon.size()];
        area += a[0] * b[1] - a[1] * b[0];
    }
    return std::abs(area) * 0.5;
}

void throwIfRasterCancelled(std::stop_token const cancellation)
{
    if (cancellation.stop_requested())
    {
        throw generation::GenerationCancelled{};
    }
}

std::span<MapImageDraw const> worldMapDraws(S2MapLayout const& layout) noexcept
{
    return {layout.world_draws.data(), std::min<size_t>(layout.world_draw_count, layout.world_draws.size())};
}

std::array<uint32_t, 2U> worldMapOutputSize(uint32_t const width, uint32_t const height) noexcept
{
    float const scale = std::min(1.0F, static_cast<float>(S2_WORLD_MAP_IMAGE_SIDE) / std::max(width, height));
    return {
        std::max(1U, static_cast<uint32_t>(std::lround(width * scale))),
        std::max(1U, static_cast<uint32_t>(std::lround(height * scale))),
    };
}

auto riverMapDrawKey(MapImageDraw const& draw) noexcept
{
    return std::array{
        draw.image,
        std::bit_cast<uint32_t>(draw.source[0]), std::bit_cast<uint32_t>(draw.source[1]),
        std::bit_cast<uint32_t>(draw.source[2]), std::bit_cast<uint32_t>(draw.source[3]),
        std::bit_cast<uint32_t>(draw.destination[0]), std::bit_cast<uint32_t>(draw.destination[1]),
        std::bit_cast<uint32_t>(draw.destination[2]), std::bit_cast<uint32_t>(draw.destination[3]),
        draw.scissor[0], draw.scissor[1], draw.scissor[2], draw.scissor[3],
    };
}

// Integrate the union of polygon intervals between every edge-order event.
// Shared banks and confluences therefore do not accumulate opacity twice.
double riverUnionArea(std::vector<RiverPolygon> const& polygons, std::stop_token const cancellation)
{
    throwIfRasterCancelled(cancellation);
    if (polygons.empty())
    {
        return 0.0;
    }
    if (polygons.size() == 1U)
    {
        return std::min(1.0, riverPolygonArea(polygons.front()));
    }
    struct Edge { double lo, hi, slope, offset; };
    std::vector<Edge> edges;
    std::vector<double> events;
    for (auto const& polygon : polygons)
    {
        throwIfRasterCancelled(cancellation);
        if (riverPolygonArea(polygon) >= 1.0)
        {
            return 1.0;
        }
        for (size_t i = 0U; i < polygon.size(); ++i)
        {
            if ((i & 31U) == 0U)
            {
                throwIfRasterCancelled(cancellation);
            }
            auto const& a = polygon[i];
            auto const& b = polygon[(i + 1U) % polygon.size()];
            events.push_back(a[0]);
            if (a[0] != b[0])
            {
                double const slope = (b[1] - a[1]) / (b[0] - a[0]);
                edges.push_back({std::min(a[0], b[0]), std::max(a[0], b[0]), slope, a[1] - slope * a[0]});
            }
        }
    }
    for (size_t i = 0U; i < edges.size(); ++i)
    {
        if ((i & 31U) == 0U)
        {
            throwIfRasterCancelled(cancellation);
        }
        for (size_t j = 0U; j < i; ++j)
        {
            if ((j & 255U) == 0U)
            {
                throwIfRasterCancelled(cancellation);
            }
            auto const& a = edges[i];
            auto const& b = edges[j];
            if (a.slope != b.slope)
            {
                double const x = (b.offset - a.offset) / (a.slope - b.slope);
                if (x > std::max(a.lo, b.lo) && x < std::min(a.hi, b.hi))
                {
                    events.push_back(x);
                }
            }
        }
    }
    std::sort(events.begin(), events.end());
    events.erase(std::unique(events.begin(), events.end()), events.end());
    double area = 0.0;
    for (size_t e = 1U; e < events.size(); ++e)
    {
        throwIfRasterCancelled(cancellation);
        double const x = (events[e - 1U] + events[e]) * 0.5;
        std::vector<std::array<double, 2U>> intervals;
        for (auto const& polygon : polygons)
        {
            throwIfRasterCancelled(cancellation);
            double lo = 1.0;
            double hi = 0.0;
            for (size_t i = 0U; i < polygon.size(); ++i)
            {
                auto const& a = polygon[i];
                auto const& b = polygon[(i + 1U) % polygon.size()];
                if (a[0] != b[0] && x >= std::min(a[0], b[0]) && x <= std::max(a[0], b[0]))
                {
                    double const y = a[1] + (x - a[0]) * (b[1] - a[1]) / (b[0] - a[0]);
                    lo = std::min(lo, y);
                    hi = std::max(hi, y);
                }
            }
            if (hi >= lo)
            {
                intervals.push_back({lo, hi});
            }
        }
        std::sort(intervals.begin(), intervals.end());
        double length = 0.0;
        double end = 0.0;
        for (auto const& interval : intervals)
        {
            length += std::max(0.0, interval[1] - std::max(end, interval[0]));
            end = std::max(end, interval[1]);
        }
        area += (events[e] - events[e - 1U]) * length;
    }
    return std::clamp(area, 0.0, 1.0);
}

std::vector<S2RiverMapGeometry::Polygon> prepareRiverContactPolygons(
    generation::GlobalAtlas const& atlas,
    S2MapImage const& base,
    std::array<float, 2U> const range,
    std::map<RiverPoint, generation::V10SurfaceSample> const& course_samples,
    std::span<std::array<int32_t, 2U> const> cells,
    std::stop_token const cancellation)
{
    std::vector<S2RiverMapGeometry::Polygon> polygons;
    std::map<RiverPoint, generation::V10SurfaceSample> contact_samples;
    auto const contact_sample = [&](RiverPoint const xy) -> generation::V10SurfaceSample const& {
        throwIfRasterCancelled(cancellation);
        if (auto const found = course_samples.find(xy); found != course_samples.end())
        {
            return found->second;
        }
        auto const found = contact_samples.find(xy);
        if (found != contact_samples.end())
        {
            return found->second;
        }
        return contact_samples.emplace(xy,
            generation::sampleV10AnnualMeanSurface(atlas, xy[0], xy[1], 0U)).first->second;
    };
    std::map<RiverPoint, generation::V10WaterGeometrySample> water_geometry_samples;
    auto const water_geometry = [&](RiverPoint const xy) -> generation::V10WaterGeometrySample const& {
        throwIfRasterCancelled(cancellation);
        if (auto const found = water_geometry_samples.find(xy); found != water_geometry_samples.end())
        {
            return found->second;
        }
        if (auto const found = course_samples.find(xy); found != course_samples.end())
        {
            auto const& surface = found->second;
            return water_geometry_samples.emplace(xy, generation::V10WaterGeometrySample{
                surface.elevation_m, surface.water_level_m, surface.ocean, surface.lake, surface.river}).first->second;
        }
        return water_geometry_samples.emplace(xy,
            generation::sampleV10WaterGeometry(atlas, xy[0], xy[1], 0U)).first->second;
    };
    auto const emit_contact = [&](RiverPolygon xy, bool standing, RiverPoint palette_sample) {
        throwIfRasterCancelled(cancellation);
        S2RiverMapGeometry::Polygon polygon;
        polygon.xy = std::move(xy);
        RiverPoint palette_xy{};
        for (auto const& point : polygon.xy)
        {
            palette_xy[0] += point[0] / polygon.xy.size();
            palette_xy[1] += point[1] / polygon.xy.size();
        }
        uint32_t const coarse_x = static_cast<uint32_t>(std::floor(
            std::fmod(palette_xy[0] + 65'536.0, 65'536.0) / 256.0));
        uint32_t const coarse_y = static_cast<uint32_t>(std::floor(
            std::fmod(palette_xy[1] + 65'536.0, 65'536.0) / 256.0));
        uint8_t const coarse_biome = base.v10_biome_ids[static_cast<size_t>(coarse_y) * 256U + coarse_x];
        if (!standing && coarse_biome != 10U && coarse_biome != 11U)
        {
            // Already-dry background keeps its exact viewport palette sampler.
            // Only newly exposed land needs to replace coarse standing water.
            return;
        }
        auto const& s = contact_sample(palette_sample);
        polygon.contact = true;
        polygon.standing = standing;
        polygon.cold = s.ocean ? s.sea_ice_thickness_m >= 0.1 : s.temperature_celsius < 0.0;
        polygon.color = s2NativeTerrainColor(static_cast<float>(s.elevation_m), range,
            {static_cast<float>(s.elevation_m), static_cast<float>(s.elevation_m)}, polygon.standing,
            static_cast<float>(s.water_level_m - s.elevation_m), 0U);
        if (!polygon.standing && s.biome_id != 12U)
        {
            polygon.color = s2NativeBiomeColor(polygon.color, s.biome_id);
        }
        if (polygon.standing && (coarse_biome == 10U || coarse_biome == 11U))
        {
            // Contact refinement changes occupancy. Retain the existing land
            // palette where no coarse standing-water pixel needs replacement.
            double const px = palette_xy[0] / 65'536.0 * base.width - 0.5;
            double const py = palette_xy[1] / 65'536.0 * base.height - 0.5;
            int64_t const ix = static_cast<int64_t>(std::floor(px));
            int64_t const iy = static_cast<int64_t>(std::floor(py));
            auto const pixel = [&](int64_t xx, int64_t yy, size_t channel) {
                xx = (xx % base.width + base.width) % base.width;
                yy = (yy % base.height + base.height) % base.height;
                return static_cast<double>(base.rgba[static_cast<size_t>(yy) * base.width
                    + static_cast<size_t>(xx)][channel]);
            };
            for (size_t channel = 0U; channel < 3U; ++channel)
            {
                polygon.color[channel] = static_cast<float>(std::lerp(
                    std::lerp(pixel(ix, iy, channel), pixel(ix + 1, iy, channel), px - std::floor(px)),
                    std::lerp(pixel(ix, iy + 1, channel), pixel(ix + 1, iy + 1, channel), px - std::floor(px)),
                    py - std::floor(py)));
            }
        }
        polygons.push_back(std::move(polygon));
    };
    auto const water_crossing = [&](RiverPoint lo, RiverPoint hi, bool wet_at_lo) {
        // Eight brackets locate a crossing within sqrt(2)/128m
        // on the edges of a two-metre leaf; membership is queried.
        for (uint32_t step = 0U; step < 8U; ++step)
        {
            throwIfRasterCancelled(cancellation);
            RiverPoint const mid{(lo[0] + hi[0]) * 0.5, (lo[1] + hi[1]) * 0.5};
            auto const& q = water_geometry(mid);
            if ((q.ocean || q.lake) == wet_at_lo)
            {
                lo = mid;
            }
            else
            {
                hi = mid;
            }
        }
        return RiverPoint{(lo[0] + hi[0]) * 0.5, (lo[1] + hi[1]) * 0.5};
    };
    auto const refine = [&](auto&& self, double x, double y, double size) -> void {
        throwIfRasterCancelled(cancellation);
        std::array<RiverPoint, 5U> const points{{{x, y}, {x + size, y}, {x + size, y + size},
            {x, y + size}, {x + size * 0.5, y + size * 0.5}}};
        std::array<bool, 5U> wet{};
        for (size_t i = 0U; i < points.size(); ++i)
        {
            auto const& s = water_geometry(points[i]);
            wet[i] = s.ocean || s.lake;
        }
        bool const uniform = std::all_of(wet.begin(), wet.end(), [&](bool w) { return w == wet.front(); });
        if (size > 16.0 || (!uniform && size > 2.0))
        {
            double const half = size * 0.5;
            self(self, x, y, half);
            self(self, x + half, y, half);
            self(self, x, y + half, half);
            self(self, x + half, y + half, half);
            return;
        }
        if (uniform)
        {
            emit_contact(RiverPolygon(points.begin(), points.begin() + 4), wet.back(), points.back());
            return;
        }
        // Mixed leaves follow the queried standing-water boundary instead of
        // painting the center's classification over the complete square.
        for (size_t corner = 0U; corner < 4U; ++corner)
        {
            std::array<size_t, 3U> const vertices{corner, (corner + 1U) % 4U, 4U};
            for (bool standing : {false, true})
            {
                RiverPolygon polygon;
                RiverPoint palette_sample{};
                bool has_sample = false;
                for (size_t edge = 0U; edge < 3U; ++edge)
                {
                    size_t const ia = vertices[(edge + 2U) % 3U];
                    size_t const ib = vertices[edge];
                    if (wet[ia] != wet[ib])
                    {
                        polygon.push_back(water_crossing(points[ia], points[ib], wet[ia]));
                    }
                    if (wet[ib] == standing)
                    {
                        polygon.push_back(points[ib]);
                        palette_sample = points[ib];
                        has_sample = true;
                    }
                }
                if (has_sample && polygon.size() >= 3U)
                {
                    emit_contact(std::move(polygon), standing, palette_sample);
                }
            }
        }
    };
    for (auto const& cell : cells)
    {
        throwIfRasterCancelled(cancellation);
        refine(refine, cell[0] * 256.0, cell[1] * 256.0, 256.0);
    }
    return polygons;
}

std::shared_ptr<S2RiverMapGeometry const> prepareRiverMapGeometry(
    shared::TerrainWorld const& world, S2MapImage const& base, std::array<float, 2U> const range,
    std::stop_token const cancellation)
{
    static std::timed_mutex preparation_mutex;
    static std::weak_ptr<S2RiverMapGeometry const> previous;
    std::unique_lock preparation_lock(preparation_mutex, std::defer_lock);
    while (!preparation_lock.try_lock_for(std::chrono::milliseconds{10}))
    {
        throwIfRasterCancelled(cancellation);
    }
    throwIfRasterCancelled(cancellation);
    if (auto cached = previous.lock(); cached && cached->source == world.atlas())
    {
        return cached;
    }
    auto result = std::make_shared<S2RiverMapGeometry>();
    result->source = world.atlas();
    throwIfRasterCancelled(cancellation);
    auto const courses = river_field_candidate::exportRiverMapCourses(*generation::v10RiverBedField(*result->source));
    throwIfRasterCancelled(cancellation);
    std::map<RiverPoint, generation::V10SurfaceSample> samples;
    auto const sample = [&](RiverPoint const xy) -> generation::V10SurfaceSample const& {
        throwIfRasterCancelled(cancellation);
        auto const found = samples.find(xy);
        if (found != samples.end())
        {
            return found->second;
        }
        return samples.emplace(xy, generation::sampleV10AnnualMeanSurface(*result->source, xy[0], xy[1], 0U)).first->second;
    };
    for (auto const& course : courses)
    {
        throwIfRasterCancelled(cancellation);
        if (course.receiver_extension)
        {
            continue;
        }
        for (size_t i = 1U; i < course.points.size(); ++i)
        {
            throwIfRasterCancelled(cancellation);
            auto const& a = course.points[i - 1U];
            auto const& b = course.points[i];
            auto const& sa = sample({a.xy.x_m, a.xy.y_m});
            auto const& sb = sample({b.xy.x_m, b.xy.y_m});
            if (!(sa.river || sb.river || sa.ocean || sb.ocean || sa.lake || sb.lake))
            {
                continue;
            }
            double const dx = b.xy.x_m - a.xy.x_m;
            double const dy = b.xy.y_m - a.xy.y_m;
            double const length = std::hypot(dx, dy);
            if (!(length > 0.0))
            {
                continue;
            }
            auto const normal_at = [&](size_t index) {
                auto const& before = course.points[index == 0U ? 0U : index - 1U].xy;
                auto const& after = course.points[index + 1U == course.points.size() ? index : index + 1U].xy;
                double const tx = after.x_m - before.x_m;
                double const ty = after.y_m - before.y_m;
                double const norm = std::hypot(tx, ty);
                return RiverPoint{-ty / norm, tx / norm};
            };
            auto const na = normal_at(i - 1U);
            auto const nb = normal_at(i);
            S2RiverMapGeometry::Polygon polygon;
            polygon.xy = {{a.xy.x_m - na[0] * a.nominal_halfwidth_m, a.xy.y_m - na[1] * a.nominal_halfwidth_m},
                {b.xy.x_m - nb[0] * b.nominal_halfwidth_m, b.xy.y_m - nb[1] * b.nominal_halfwidth_m},
                {b.xy.x_m + nb[0] * b.nominal_halfwidth_m, b.xy.y_m + nb[1] * b.nominal_halfwidth_m},
                {a.xy.x_m + na[0] * a.nominal_halfwidth_m, a.xy.y_m + na[1] * a.nominal_halfwidth_m}};
            polygon.cold = (sa.temperature_celsius + sb.temperature_celsius) < 0.0;
            result->polygons.push_back(std::move(polygon));
        }
    }
    for (auto const& course : courses)
    {
        throwIfRasterCancelled(cancellation);
        if (course.receiver_extension || course.points.empty())
        {
            continue;
        }
        for (size_t index : {size_t{0U}, course.points.size() - 1U})
        {
            throwIfRasterCancelled(cancellation);
            auto const& p = course.points[index];
            auto const& surface = sample({p.xy.x_m, p.xy.y_m});
            if (!(surface.river || surface.ocean || surface.lake))
            {
                continue;
            }
            S2RiverMapGeometry::Polygon cap;
            cap.cold = surface.temperature_celsius < 0.0;
            for (uint32_t vertex = 0U; vertex < 64U; ++vertex)
            {
                if ((vertex & 15U) == 0U)
                {
                    throwIfRasterCancelled(cancellation);
                }
                double const angle = static_cast<double>(vertex) * 6.283'185'307'179'586'476'9 / 64.0;
                cap.xy.push_back({p.xy.x_m + std::cos(angle) * p.nominal_halfwidth_m,
                    p.xy.y_m + std::sin(angle) * p.nominal_halfwidth_m});
            }
            result->polygons.push_back(std::move(cap));
        }
    }
    // Refine only native cells touching a receiving course. Mixed leaves reach
    // two metres before their standing-water edge crossings are located below.
    std::set<std::array<int32_t, 2U>> contact_cells;
    for (auto const& course : courses)
    {
        throwIfRasterCancelled(cancellation);
        if (!course.receiver_extension || course.points.empty())
        {
            continue;
        }
        auto const& p = course.points.front().xy;
        int32_t const cx = static_cast<int32_t>(std::floor(p.x_m / 256.0));
        int32_t const cy = static_cast<int32_t>(std::floor(p.y_m / 256.0));
        for (int32_t y = -1; y <= 1; ++y)
        {
            for (int32_t x = -1; x <= 1; ++x)
            {
                contact_cells.insert({(cx + x + 256) % 256, (cy + y + 256) % 256});
            }
        }
    }
    std::vector<std::array<int32_t, 2U>> const cells{contact_cells.begin(), contact_cells.end()};
    size_t const worker_count = std::min<size_t>(
        cells.size(), std::clamp(std::thread::hardware_concurrency(), 1U, 4U));
    std::vector<std::future<std::vector<S2RiverMapGeometry::Polygon>>> tasks;
    auto const refine_contacts = [&](std::span<std::array<int32_t, 2U> const> range_cells) {
        return prepareRiverContactPolygons(*result->source, base, range, samples, range_cells, cancellation);
    };
    auto const append = [&](std::vector<S2RiverMapGeometry::Polygon> polygons) {
        result->polygons.insert(result->polygons.end(), std::make_move_iterator(polygons.begin()),
            std::make_move_iterator(polygons.end()));
    };
    // Each range owns its sampling caches. Merge in cell order so overlapping
    // contacts keep the same precedence regardless of worker completion order.
    for (size_t worker = 0U; worker < worker_count; ++worker)
    {
        size_t const begin = cells.size() * worker / worker_count;
        size_t const end = cells.size() * (worker + 1U) / worker_count;
        auto const range_cells = std::span{cells}.subspan(begin, end - begin);
        if (worker + 1U < worker_count)
        {
            tasks.push_back(std::async(std::launch::async | std::launch::deferred, refine_contacts, range_cells));
        }
        else
        {
            auto polygons = refine_contacts(range_cells);
            for (auto& task : tasks)
            {
                append(task.get());
            }
            append(std::move(polygons));
        }
    }
    for (auto& polygon : result->polygons)
    {
        throwIfRasterCancelled(cancellation);
        polygon.bounds = {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
        for (auto const& xy : polygon.xy)
        {
            throwIfRasterCancelled(cancellation);
            polygon.bounds[0] = std::min(polygon.bounds[0], xy[0]);
            polygon.bounds[1] = std::min(polygon.bounds[1], xy[1]);
            polygon.bounds[2] = std::max(polygon.bounds[2], xy[0]);
            polygon.bounds[3] = std::max(polygon.bounds[3], xy[1]);
        }
    }
    throwIfRasterCancelled(cancellation);
    previous = result;
    return result;
}
} // namespace

namespace {

using Color = std::array<float, 3U>;

static constexpr std::array<Color, 13U> NATIVE_BIOME_COLORS{{
        {222.0F, 231.0F, 224.0F}, {153.0F, 172.0F, 154.0F}, {62.0F, 103.0F, 78.0F},
        {154.0F, 180.0F, 117.0F}, {67.0F, 119.0F, 75.0F}, {207.0F, 175.0F, 107.0F},
        {174.0F, 174.0F, 94.0F}, {115.0F, 151.0F, 83.0F}, {43.0F, 116.0F, 63.0F},
        {169.0F, 164.0F, 146.0F}, {35.0F, 69.0F, 87.0F}, {49.0F, 102.0F, 131.0F},
        {63.0F, 138.0F, 162.0F},
    }};
using Rgba = std::array<uint8_t, 4U>;

constexpr double WORLD_SIZE = static_cast<double>(generation::WORLD_SIZE_BLOCKS);
constexpr uint32_t WORLD_BLOCK_PERIOD = generation::WORLD_SIZE_BLOCKS;
constexpr uint32_t WORLD_PIXEL_COUNT = S2_WORLD_MAP_IMAGE_SIDE * S2_WORLD_MAP_IMAGE_SIDE;
constexpr uint32_t MINIMAP_PIXEL_COUNT = S2_MINIMAP_IMAGE_SIDE * S2_MINIMAP_IMAGE_SIDE;
constexpr std::array<Color, 6U> CLIMATE_COLORS{{
    {35.0F, 69.0F, 87.0F},
    {181.0F, 153.0F, 99.0F},
    {76.0F, 125.0F, 91.0F},
    {119.0F, 148.0F, 107.0F},
    {153.0F, 172.0F, 154.0F},
    {222.0F, 231.0F, 224.0F},
}};
constexpr Color PLATE_COLOR{213.0F, 164.0F, 237.0F};
constexpr Color PLATE_MOTION_COLOR{255.0F, 184.0F, 64.0F};
constexpr Color VECTOR_OUTLINE_COLOR{17.0F, 42.0F, 53.0F};
constexpr size_t MAX_S2_PLATE_BOUNDARY_SEGMENTS = 32'768U;

struct Range final {
    float minimum = 0.0F;
    float maximum = 0.0F;
};

struct AxisSegment final {
    float source_start = 0.0F;
    float source_extent = 0.0F;
    float destination_fraction = 0.0F;
    float destination_extent = 0.0F;
};

struct WrappedAxisSegment final {
    uint32_t world_start = 0U;
    uint32_t local_start = 0U;
    uint32_t extent = 0U;
};

[[nodiscard]]
double wrapWorld(double coordinate) noexcept
{
    coordinate = std::fmod(coordinate, WORLD_SIZE);
    return coordinate < 0.0 ? coordinate + WORLD_SIZE : coordinate;
}

[[nodiscard]]
double wrapUnit(double coordinate) noexcept
{
    coordinate = std::fmod(coordinate, 1.0);
    return coordinate < 0.0 ? coordinate + 1.0 : coordinate;
}

[[nodiscard]]
bool validL0Source(shared::TerrainWorld const& world, shared::TerrainLodArtifact const& lod) noexcept
{
    auto const atlas = world.atlas();
    if (!atlas || !shared::isValidTerrainIdentity(world.identity())
        || lod.terrain_identity != world.identity()
        || lod.model_identity != shared::terrainLodModel(world.atlas()->configuration.model_tag)
        || lod.level != shared::TerrainLodLevel::L0 || lod.extent != generation::WORLD_SIZE_BLOCKS
        || lod.origin_x != 0U || lod.origin_y != 0U
        || (atlas->configuration.resolution != 256U && atlas->configuration.resolution != 512U)) {
        return false;
    }
    uint32_t const expected_spacing = shared::terrainLodSpacing(
        atlas->configuration.resolution, shared::TerrainLodLevel::L0, atlas->configuration.model_tag
    );
    uint32_t const expected_side = generation::WORLD_SIZE_BLOCKS / expected_spacing + 1U;
    return lod.spacing == expected_spacing && lod.side == expected_side
        && lod.nodes.size() == static_cast<uint64_t>(expected_side) * expected_side;
}

template <typename TValue>
[[nodiscard]]
bool hasFiniteField(std::vector<TValue> const& field, size_t const count) noexcept
{
    if (field.size() != count) {
        return false;
    }
    if constexpr (std::is_floating_point_v<TValue>) {
        return std::all_of(field.begin(), field.end(), [](TValue const value) { return std::isfinite(value); });
    }
    return true;
}

[[nodiscard]]
bool hasBinaryMask(std::vector<uint8_t> const& field, size_t const count) noexcept
{
    return field.size() == count
        && std::all_of(field.begin(), field.end(), [](uint8_t const value) { return value <= 1U; });
}

template <typename TValue>
[[nodiscard]]
Range fieldRange(std::vector<TValue> const& field) noexcept
{
    auto const [minimum, maximum] = std::minmax_element(field.begin(), field.end());
    return {static_cast<float>(*minimum), static_cast<float>(*maximum)};
}

[[nodiscard]]
generation::V9RegisteredFieldView const* v9Field(
    std::span<generation::V9RegisteredFieldView const> fields,
    std::string_view const name
) noexcept
{
    auto const found = std::find_if(fields.begin(), fields.end(), [name](auto const& field) {
        return field.name == name;
    });
    return found == fields.end() ? nullptr : &*found;
}

[[nodiscard]]
bool hasV9Shape(
    generation::V9RegisteredFieldView const* const field,
    std::span<uint32_t const> expected_shape
) noexcept
{
    if (field == nullptr || !std::ranges::equal(field->shape, expected_shape)) {
        return false;
    }
    size_t expected_count = 1U;
    for (uint32_t const extent : expected_shape) {
        if (extent == 0U || expected_count > std::numeric_limits<size_t>::max() / extent) {
            return false;
        }
        expected_count *= extent;
    }
    return std::visit([expected_count](auto const values) {
        return values.size() == expected_count;
    }, field->values);
}

[[nodiscard]]
double v9ValueAt(generation::V9RegisteredFieldView const& field, size_t const index) noexcept
{
    return std::visit([index](auto const values) {
        return index < values.size()
            ? static_cast<double>(values[index])
            : std::numeric_limits<double>::quiet_NaN();
    }, field.values);
}

[[nodiscard]]
bool v9FiniteField(generation::V9RegisteredFieldView const& field) noexcept
{
    return std::visit([](auto const values) {
        return std::all_of(values.begin(), values.end(), [](auto const value) {
            return std::isfinite(static_cast<double>(value));
        });
    }, field.values);
}

[[nodiscard]]
bool v9BooleanField(generation::V9RegisteredFieldView const& field) noexcept
{
    return std::visit([](auto const values) {
        return std::all_of(values.begin(), values.end(), [](auto const value) {
            double const converted = static_cast<double>(value);
            return converted == 0.0 || converted == 1.0;
        });
    }, field.values);
}

[[nodiscard]]
double v9PhaseMean(
    generation::V9RegisteredFieldView const& field,
    size_t const phase_size,
    size_t const index
) noexcept
{
    double total = 0.0;
    for (size_t phase = 0U; phase < 8U; ++phase) {
        total += v9ValueAt(field, phase * phase_size + index);
    }
    return total / 8.0;
}

[[nodiscard]]
std::vector<float> v9MeanPhaseField(
    generation::V9RegisteredFieldView const& field,
    size_t const phase_size
)
{
    std::vector<float> result(phase_size);
    for (size_t index = 0U; index < phase_size; ++index) {
        result[index] = static_cast<float>(v9PhaseMean(field, phase_size, index));
    }
    return result;
}

[[nodiscard]]
Color materialColor(shared::Block const block) noexcept
{
    switch (block) {
    case shared::Block::Air: return {189.0F, 217.0F, 237.0F};
    case shared::Block::Stone:
    case shared::Block::Bedrock: return {128.0F, 128.0F, 128.0F};
    case shared::Block::Grass: return {92.0F, 147.0F, 69.0F};
    case shared::Block::Dirt: return {124.0F, 97.0F, 66.0F};
    case shared::Block::Snow: return {244.0F, 247.0F, 250.0F};
    case shared::Block::Ice: return {128.0F, 201.0F, 214.0F};
    case shared::Block::Sand: return {229.0F, 211.0F, 162.0F};
    }
    return {128.0F, 128.0F, 128.0F};
}

[[nodiscard]]
Color ramp(float value, std::span<std::pair<float, Color> const> stops) noexcept
{
    if (value <= stops.front().first) {
        return stops.front().second;
    }
    for (size_t index = 1U; index < stops.size(); ++index) {
        auto const& lower = stops[index - 1U];
        auto const& upper = stops[index];
        if (value <= upper.first) {
            float const amount = (value - lower.first) / (upper.first - lower.first);
            Color result{};
            for (size_t channel = 0U; channel < result.size(); ++channel) {
                result[channel] = lower.second[channel] + (upper.second[channel] - lower.second[channel]) * amount;
            }
            return result;
        }
    }
    return stops.back().second;
}

[[nodiscard]]
float niceStep(float value) noexcept
{
    if (!(value > 0.0F) || !std::isfinite(value)) {
        return 1.0F;
    }
    float const power = std::pow(10.0F, std::floor(std::log10(value)));
    float const fraction = value / power;
    float const multiplier = fraction < 1.5F ? 1.0F : (fraction < 3.5F ? 2.0F : (fraction < 7.5F ? 5.0F : 10.0F));
    return multiplier * power;
}

[[nodiscard]]
Color elevationColor(float value, Range const range) noexcept
{
    if (value < 0.0F) {
        static constexpr std::array<std::pair<float, Color>, 2U> SEA_STOPS{{
            {0.0F, {24.0F, 51.0F, 70.0F}},
            {1.0F, {101.0F, 150.0F, 155.0F}},
        }};
        float const minimum = std::min(-0.01F, range.minimum);
        float const position = std::clamp((value - minimum) / std::max(0.01F, -minimum), 0.0F, 1.0F);
        return ramp(position, SEA_STOPS);
    }
    static constexpr std::array<std::pair<float, Color>, 4U> LAND_STOPS{{
        {0.0F, {126.0F, 151.0F, 119.0F}},
        {0.35F, {177.0F, 170.0F, 123.0F}},
        {0.7F, {199.0F, 184.0F, 153.0F}},
        {1.0F, {237.0F, 234.0F, 217.0F}},
    }};
    return ramp(value / std::max(0.01F, range.maximum), LAND_STOPS);
}

[[nodiscard]]
Color temperatureColor(float const temperature) noexcept
{
    static constexpr std::array<std::pair<float, Color>, 5U> STOPS{{
        {-40.0F, {42.0F, 78.0F, 116.0F}},
        {-20.0F, {99.0F, 153.0F, 180.0F}},
        {0.0F, {202.0F, 215.0F, 204.0F}},
        {15.0F, {222.0F, 187.0F, 125.0F}},
        {30.0F, {205.0F, 120.0F, 83.0F}},
    }};
    return ramp(temperature, STOPS);
}

[[nodiscard]]
Color heatColor(S2WorldMapLayer const layer, float const value, Range const range) noexcept
{
    if (layer == S2WorldMapLayer::Temperature) {
        return temperatureColor(value);
    }
    float normalized = (value - range.minimum) / std::max(1.0e-6F, range.maximum - range.minimum);
    normalized = std::clamp(normalized, 0.0F, 1.0F);
    if (layer == S2WorldMapLayer::Precipitation) {
        normalized = std::log1p(std::max(0.0F, value - range.minimum))
            / std::log1p(std::max(1.0e-6F, range.maximum - range.minimum));
    }
    static constexpr std::array<std::pair<float, Color>, 3U> MOISTURE_STOPS{{
        {0.0F, {209.0F, 189.0F, 137.0F}},
        {0.5F, {109.0F, 183.0F, 158.0F}},
        {1.0F, {31.0F, 137.0F, 125.0F}},
    }};
    static constexpr std::array<std::pair<float, Color>, 3U> PRECIPITATION_STOPS{{
        {0.0F, {202.0F, 215.0F, 219.0F}},
        {0.5F, {121.0F, 165.0F, 181.0F}},
        {1.0F, {58.0F, 101.0F, 128.0F}},
    }};
    return layer == S2WorldMapLayer::Moisture
        ? ramp(normalized, MOISTURE_STOPS)
        : ramp(normalized, PRECIPITATION_STOPS);
}

void blend(Color& base, Color const tint, float const alpha) noexcept
{
    for (size_t channel = 0U; channel < base.size(); ++channel) {
        base[channel] = std::clamp(base[channel] + (tint[channel] - base[channel]) * alpha, 0.0F, 255.0F);
    }
}

void applyOpaqueIceMaterial(Color& color, uint8_t const ice_kind) noexcept
{
    Color material;
    if (ice_kind == 1U) {
        material = {236.0F, 242.0F, 235.0F};
    } else if (ice_kind == 2U) {
        material = {208.0F, 225.0F, 223.0F};
    } else if (ice_kind == 3U) {
        material = {203.0F, 227.0F, 231.0F};
    } else if (ice_kind == S2_MAP_PROJECTED_INLAND_ICE) {
        material = {208.0F, 225.0F, 223.0F};
    } else {
        return;
    }
    // Retain subdued height/contour brightness, never the uncovered terrain hue.
    float const brightness = .90F + .10F * std::clamp(
        (color[0U] + color[1U] + color[2U]) / (3.0F * 255.0F), 0.0F, 1.0F);
    for (size_t channel = 0U; channel < color.size(); ++channel) {
        color[channel] = material[channel] * brightness;
    }
}

void setPixel(S2MapImage& image, int32_t const x, int32_t const y, Color const color) noexcept
{
    if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= image.width || static_cast<uint32_t>(y) >= image.height) {
        return;
    }
    Rgba& pixel = image.rgba[static_cast<size_t>(y) * image.width + static_cast<uint32_t>(x)];
    pixel = {
        static_cast<uint8_t>(std::clamp(std::lround(color[0]), 0L, 255L)),
        static_cast<uint8_t>(std::clamp(std::lround(color[1]), 0L, 255L)),
        static_cast<uint8_t>(std::clamp(std::lround(color[2]), 0L, 255L)),
        255U,
    };
}

[[nodiscard]]
std::vector<AxisSegment> splitAxis(float start, float extent)
{
    std::vector<AxisSegment> result;
    float remaining = extent;
    float destination_start = 0.0F;
    start = static_cast<float>(wrapUnit(start));
    while (remaining > 1.0e-6F && result.size() < S2_WORLD_MAP_MAXIMUM_DRAWS) {
        float const segment_extent = std::min(remaining, 1.0F - start);
        result.push_back({
            .source_start = start,
            .source_extent = segment_extent,
            .destination_fraction = destination_start / extent,
            .destination_extent = segment_extent / extent,
        });
        remaining -= segment_extent;
        destination_start += segment_extent;
        start = 0.0F;
    }
    return result;
}

[[nodiscard]]
std::array<WrappedAxisSegment, 2U> splitWrappedAxis(uint32_t origin, uint32_t extent, uint32_t& count) noexcept
{
    std::array<WrappedAxisSegment, 2U> result{};
    count = 0U;
    uint32_t const first_extent = std::min(extent, WORLD_BLOCK_PERIOD - origin);
    result[count++] = {.world_start = origin, .local_start = 0U, .extent = first_extent};
    uint32_t const remaining = extent - first_extent;
    if (remaining > 0U) {
        result[count++] = {.world_start = 0U, .local_start = first_extent, .extent = remaining};
    }
    return result;
}

[[nodiscard]]
Color minimapMaterialColor(shared::TerrainLodSample const& sample) noexcept
{
    if (sample.water_present) {
        return sample.temperature <= -10.0
            ? Color{128.0F, 201.0F, 214.0F}
            : Color{64.0F, 124.0F, 157.0F};
    }
    return materialColor(sample.top_material);
}

[[nodiscard]]
bool validLocalLod(shared::TerrainLodArtifact const& lod) noexcept
{
    generation::TerrainModelTag const model_tag = lod.terrain_identity.terrain_version
        == generation::terrainVersion(generation::TerrainModelTag::V9)
        ? generation::TerrainModelTag::V9
        : (lod.terrain_identity.terrain_version == generation::terrainVersion(generation::TerrainModelTag::V10)
            ? generation::TerrainModelTag::V10
            : lod.terrain_identity.terrain_version == generation::terrainVersion(generation::TerrainModelTag::V21)
            ? generation::TerrainModelTag::V21
            : generation::TerrainModelTag::R6);
    uint64_t const expected_nodes = static_cast<uint64_t>(lod.side) * lod.side;
    bool const valid_spacing = (lod.level == shared::TerrainLodLevel::L1
        && (lod.spacing == 32U || lod.spacing == 64U))
        || (lod.level == shared::TerrainLodLevel::L0 && (lod.spacing == 128U || lod.spacing == 256U));
    return shared::isValidTerrainIdentity(lod.terrain_identity)
        && lod.model_identity == shared::terrainLodModel(model_tag)
        && valid_spacing
        && lod.spacing != 0U && lod.extent >= S2_MINIMAP_WORLD_EXTENT
        && lod.extent <= generation::WORLD_SIZE_BLOCKS && lod.extent % lod.spacing == 0U
        && lod.origin_x % lod.spacing == 0U && lod.origin_y % lod.spacing == 0U
        && lod.origin_x < generation::WORLD_SIZE_BLOCKS && lod.origin_y < generation::WORLD_SIZE_BLOCKS
        && lod.side == lod.extent / lod.spacing + 1U
        && lod.side <= S2_MINIMAP_IMAGE_SIDE * 2U + 1U && expected_nodes <= shared::TERRAIN_LOD_MAX_NODES
        && lod.nodes.size() == expected_nodes;
}

[[nodiscard]]
TextColor textColor(Color const color) noexcept
{
    return {
        color[0] / 255.0F,
        color[1] / 255.0F,
        color[2] / 255.0F,
        1.0F,
    };
}

void submitMapText(
    TextRenderer& output,
    std::string_view const value,
    Color const color,
    float const x,
    float const y,
    float const dpi_scale,
    bool const outlined = false,
    float const text_scale = 0.5F
)
{
    TextColor const foreground = textColor(color);
    TextPlacement const placement{
        .position = {x * dpi_scale, y * dpi_scale, 0.0F},
        .scale = text_scale * dpi_scale,
    };
    if (outlined) {
        TextColor const outline = textColor({12.0F, 29.0F, 36.0F});
        for (auto const [dx, dy] : std::array<std::array<float, 2U>, 8U>{{
            {-1.0F, 0.0F}, {1.0F, 0.0F}, {0.0F, -1.0F}, {0.0F, 1.0F},
            {-1.0F, -1.0F}, {1.0F, -1.0F}, {-1.0F, 1.0F}, {1.0F, 1.0F},
        }}) {
            TextPlacement const outline_placement{
                .position = {(x + dx) * dpi_scale, (y + dy) * dpi_scale, 0.0F},
                .scale = text_scale * dpi_scale,
            };
            static_cast<void>(output.submit(value, outline, outline_placement));
        }
    }
    static_cast<void>(output.submit(value, foreground, placement));
}

[[nodiscard]]
std::string fixedText(float const value, int const places)
{
    if (!std::isfinite(value)) {
        return "n/a";
    }
    std::array<char, 64U> buffer{};
    auto const result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
        std::chars_format::fixed, places);
    if (result.ec != std::errc{}) {
        return "n/a";
    }
    return {buffer.data(), result.ptr};
}

[[nodiscard]]
std::string numberText(float const value, int const places = 2)
{
    if (!std::isfinite(value)) {
        return "n/a";
    }
    float const factor = std::pow(10.0F, static_cast<float>(places));
    float const rounded = std::floor(value * factor + 0.5F) / factor;
    if (rounded == 0.0F) {
        return "0";
    }
    std::string result = fixedText(rounded, places);
    if (result.find('.') != std::string::npos) {
        while (!result.empty() && result.back() == '0') {
            result.pop_back();
        }
        if (!result.empty() && result.back() == '.') {
            result.pop_back();
        }
    }
    return result;
}

[[nodiscard]]
std::string rangeText(S2MapImage const& image, S2WorldMapLayer const layer)
{
    size_t const index = static_cast<size_t>(layer);
    if (!image.has_numeric_range[index]) {
        return "n/a";
    }
    auto const& range = image.layer_ranges[index];
    int const minimum_places = range[0] > 0.0F && range[0] < 0.01F ? 4 : 2;
    int const maximum_places = range[1] < 1.0F ? 2 : 1;
    return numberText(range[0], minimum_places) + ".." + numberText(range[1], maximum_places);
}

[[nodiscard]]
std::string fitMapText(std::string value, float const width, float const dpi_scale)
{
    size_t const maximum_characters = static_cast<size_t>(std::max(1.0F, std::floor(width / (8.0F * dpi_scale))));
    if (value.size() > maximum_characters) {
        if (maximum_characters <= 3U) {
            value.resize(maximum_characters);
        } else {
            value.resize(maximum_characters - 3U);
            value += "...";
        }
    }
    return value;
}

[[nodiscard]]
std::optional<size_t> mapSampleIndex(
    shared::TerrainWorld const* const world,
    S2MapImage const* const image,
    double const player_x,
    double const player_y
)
{
    if (!world || !image || image->source_identity != world->identity()
        || !std::isfinite(player_x) || !std::isfinite(player_y)) {
        return std::nullopt;
    }
    auto const atlas = world->atlas();
    if (!atlas || (atlas->configuration.resolution != 256U && atlas->configuration.resolution != 512U)) {
        return std::nullopt;
    }
    uint32_t const resolution = image->source_resolution != 0U
        ? image->source_resolution : atlas->configuration.resolution;
    uint32_t const x = static_cast<uint32_t>(wrapWorld(player_x) * resolution / WORLD_SIZE);
    uint32_t const y = static_cast<uint32_t>(wrapWorld(player_y) * resolution / WORLD_SIZE);
    return static_cast<size_t>(y % resolution) * resolution + (x % resolution);
}

[[nodiscard]]
float sanitizedDpi(float const dpi_scale) noexcept
{
    return std::isfinite(dpi_scale) && dpi_scale > 0.0F
        ? std::clamp(dpi_scale, 0.25F, 8.0F)
        : 1.0F;
}

using ScreenPoint = std::array<float, 2U>;
using PixelColor = std::array<uint8_t, 4U>;

struct ScreenRect final {
    float left = 0.0F;
    float top = 0.0F;
    float right = 0.0F;
    float bottom = 0.0F;
};

[[nodiscard]]
PixelColor pixelColor(Color const color, uint8_t const alpha = 255U) noexcept
{
    return {
        static_cast<uint8_t>(std::clamp(std::lround(color[0U]), 0L, 255L)),
        static_cast<uint8_t>(std::clamp(std::lround(color[1U]), 0L, 255L)),
        static_cast<uint8_t>(std::clamp(std::lround(color[2U]), 0L, 255L)),
        alpha,
    };
}

[[nodiscard]]
bool intersects(ScreenRect const& left, ScreenRect const& right) noexcept
{
    return left.left < right.right && left.right > right.left
        && left.top < right.bottom && left.bottom > right.top;
}

[[nodiscard]]
bool contains(ScreenRect const& outer, ScreenRect const& inner) noexcept
{
    return inner.left >= outer.left && inner.top >= outer.top
        && inner.right <= outer.right && inner.bottom <= outer.bottom;
}

[[nodiscard]]
bool clipSegmentToRect(ScreenPoint& start, ScreenPoint& end, ScreenRect const& rect) noexcept
{
    float const dx = end[0U] - start[0U];
    float const dy = end[1U] - start[1U];
    float first = 0.0F;
    float last = 1.0F;
    auto const clip = [&](float const p, float const q) {
        if (std::abs(p) < 1.0e-6F) {
            return q >= 0.0F;
        }
        float const ratio = q / p;
        if (p < 0.0F) {
            first = std::max(first, ratio);
        } else {
            last = std::min(last, ratio);
        }
        return first <= last;
    };
    if (!clip(-dx, start[0U] - rect.left) || !clip(dx, rect.right - start[0U])
        || !clip(-dy, start[1U] - rect.top) || !clip(dy, rect.bottom - start[1U])) {
        return false;
    }
    ScreenPoint const original = start;
    start = {original[0U] + first * dx, original[1U] + first * dy};
    end = {original[0U] + last * dx, original[1U] + last * dy};
    // Source UVs and screen pixels share this clipper; reject only an empty interval.
    return last > first;
}

constexpr size_t MAX_GUI_TRIANGLE_VERTICES =
    MAXIMUM_GUI_VECTOR_VERTICES - MAXIMUM_GUI_VECTOR_VERTICES % 3U;

void emitTriangle(
    std::vector<GuiVectorVertex>& output,
    ScreenPoint const a,
    ScreenPoint const b,
    ScreenPoint const c,
    PixelColor const color_a,
    PixelColor const color_b,
    PixelColor const color_c
)
{
    output.push_back({.position = a, .rgba = color_a});
    output.push_back({.position = b, .rgba = color_b});
    output.push_back({.position = c, .rgba = color_c});
}

void emitSolidQuad(
    std::vector<GuiVectorVertex>& output,
    ScreenPoint const a,
    ScreenPoint const b,
    ScreenPoint const c,
    ScreenPoint const d,
    PixelColor const color
)
{
    emitTriangle(output, a, b, c, color, color, color);
    emitTriangle(output, a, c, d, color, color, color);
}

void emitFringeQuad(
    std::vector<GuiVectorVertex>& output,
    ScreenPoint const outer_a,
    ScreenPoint const outer_b,
    ScreenPoint const inner_b,
    ScreenPoint const inner_a,
    PixelColor color
)
{
    PixelColor const transparent{color[0U], color[1U], color[2U], 0U};
    emitTriangle(output, outer_a, outer_b, inner_b, transparent, transparent, color);
    emitTriangle(output, outer_a, inner_b, inner_a, transparent, color, color);
}

[[nodiscard]]
ScreenPoint offsetPoint(ScreenPoint const point, ScreenPoint const direction, float const amount) noexcept
{
    return {point[0U] + direction[0U] * amount, point[1U] + direction[1U] * amount};
}

[[nodiscard]]
bool emitStroke(
    std::vector<GuiVectorVertex>& output,
    ScreenPoint const start,
    ScreenPoint const end,
    float const width,
    PixelColor const color,
    float const fringe_width = .55F
)
{
    float const dx = end[0U] - start[0U];
    float const dy = end[1U] - start[1U];
    float const length = std::hypot(dx, dy);
    if (!std::isfinite(length) || length < 1.0e-4F) {
        return false;
    }
    constexpr size_t STROKE_VERTEX_COUNT = 18U;
    if (output.size() + STROKE_VERTEX_COUNT > MAX_GUI_TRIANGLE_VERTICES) {
        return false;
    }
    ScreenPoint const normal{-dy / length, dx / length};
    float const outer_half = width * 0.5F;
    float const core_half = std::max(0.0F, outer_half - fringe_width);
    ScreenPoint const outer_left_start = offsetPoint(start, normal, outer_half);
    ScreenPoint const outer_left_end = offsetPoint(end, normal, outer_half);
    ScreenPoint const inner_left_start = offsetPoint(start, normal, core_half);
    ScreenPoint const inner_left_end = offsetPoint(end, normal, core_half);
    ScreenPoint const outer_right_start = offsetPoint(start, normal, -outer_half);
    ScreenPoint const outer_right_end = offsetPoint(end, normal, -outer_half);
    ScreenPoint const inner_right_start = offsetPoint(start, normal, -core_half);
    ScreenPoint const inner_right_end = offsetPoint(end, normal, -core_half);

    emitSolidQuad(output, inner_left_start, inner_left_end, inner_right_end, inner_right_start, color);
    emitFringeQuad(output, outer_left_start, outer_left_end, inner_left_end, inner_left_start, color);
    emitFringeQuad(output, outer_right_end, outer_right_start, inner_right_start, inner_right_end, color);
    return true;
}

[[nodiscard]]
bool appendScreenArrow(
    std::vector<GuiVectorVertex>& output,
    ScreenPoint const center,
    float const direction_x,
    float const direction_y,
    float const length,
    float const shaft_width,
    Color const color,
    ScreenRect const& viewport,
    std::span<ScreenRect const> const excluded_rectangles
)
{
    float const magnitude = std::hypot(direction_x, direction_y);
    if (!std::isfinite(magnitude) || !std::isfinite(length) || length <= 0.0F) {
        return false;
    }
    PixelColor const foreground = pixelColor(color);
    PixelColor const outline = pixelColor(VECTOR_OUTLINE_COLOR);
    if (magnitude < 1.0e-10F) {
        ScreenRect const bounds{center[0U], center[1U], center[0U] + 2.0F, center[1U] + 2.0F};
        if (!contains(viewport, bounds)
            || std::any_of(excluded_rectangles.begin(), excluded_rectangles.end(), [&](ScreenRect const& rect) {
                return intersects(bounds, rect);
            })
            || output.size() + 6U > MAX_GUI_TRIANGLE_VERTICES) {
            return false;
        }
        emitSolidQuad(output, center, {center[0U] + 2.0F, center[1U]},
            {center[0U] + 2.0F, center[1U] + 2.0F}, {center[0U], center[1U] + 2.0F}, foreground);
        return true;
    }

    auto const segments = s2FlowArrowSegments(center, {direction_x, direction_y}, length);
    std::array<ScreenPoint, 5U> const bounds_points{
        segments[0U], segments[1U], segments[3U], segments[4U], segments[5U]};
    float left = bounds_points[0U][0U];
    float right = left;
    float top = bounds_points[0U][1U];
    float bottom = top;
    for (size_t index = 1U; index < bounds_points.size(); ++index) {
        left = std::min(left, bounds_points[index][0U]);
        right = std::max(right, bounds_points[index][0U]);
        top = std::min(top, bounds_points[index][1U]);
        bottom = std::max(bottom, bounds_points[index][1U]);
    }
    float const edge_margin = shaft_width * 0.5F + 1.25F;
    ScreenRect const bounds{left - edge_margin, top - edge_margin, right + edge_margin, bottom + edge_margin};
    if (!contains(viewport, bounds)
        || std::any_of(excluded_rectangles.begin(), excluded_rectangles.end(), [&](ScreenRect const& rect) {
            return intersects(bounds, rect);
        })) {
        return false;
    }

    size_t constexpr REQUIRED_VERTICES = 3U * 2U * 18U;
    if (output.size() + REQUIRED_VERTICES > MAX_GUI_TRIANGLE_VERTICES) {
        return false;
    }
    auto stroke = [&](ScreenPoint const from, ScreenPoint const to) {
        static_cast<void>(emitStroke(output, from, to, shaft_width + 2.0F, outline));
        static_cast<void>(emitStroke(output, from, to, shaft_width, foreground));
    };
    for (size_t segment = 0U; segment < 3U; ++segment) {
        stroke(segments[segment * 2U], segments[segment * 2U + 1U]);
    }
    return true;
}

[[nodiscard]]
bool appendPointerTriangles(
    std::vector<GuiVectorVertex>& output,
    ScreenPoint const center,
    double const heading_degrees,
    float const reach,
    ScreenRect const& viewport
)
{
    constexpr size_t POINTER_VERTEX_COUNT = 78U;
    if (!std::isfinite(heading_degrees) || !std::isfinite(reach) || reach <= 0.0F
        || output.size() + POINTER_VERTEX_COUNT > MAX_GUI_TRIANGLE_VERTICES) {
        return false;
    }
    double const angle = heading_degrees * (3.141'592'653'589'793'238'46 / 180.0);
    float const cosine = static_cast<float>(std::cos(angle));
    float const sine = static_cast<float>(std::sin(angle));
    std::array<ScreenPoint, 4U> points{{
        {0.0F, -reach}, {reach * 0.65F, reach * 0.70F},
        {0.0F, reach * 0.35F}, {-reach * 0.65F, reach * 0.70F},
    }};
    for (ScreenPoint& point : points) {
        ScreenPoint const local = point;
        point = {
            center[0U] + local[0U] * cosine - local[1U] * sine,
            center[1U] + local[0U] * sine + local[1U] * cosine,
        };
    }
    ScreenRect bounds{points[0U][0U], points[0U][1U], points[0U][0U], points[0U][1U]};
    for (size_t index = 0U; index < points.size(); ++index) {
        bounds.left = std::min(bounds.left, points[index][0U] - 2.0F);
        bounds.right = std::max(bounds.right, points[index][0U] + 2.0F);
        bounds.top = std::min(bounds.top, points[index][1U] - 2.0F);
        bounds.bottom = std::max(bounds.bottom, points[index][1U] + 2.0F);
    }
    if (!contains(viewport, bounds)) {
        return false;
    }

    PixelColor const dark = pixelColor({16.0F, 33.0F, 43.0F});
    PixelColor const accent = pixelColor({255.0F, 224.0F, 128.0F});
    for (size_t index = 0U; index < points.size(); ++index) {
        size_t const next = (index + 1U) % points.size();
        if (!emitStroke(output, points[index], points[next], 4.0F, dark)) {
            return false;
        }
    }
    emitTriangle(output, points[0U], points[1U], points[2U], accent, accent, accent);
    emitTriangle(output, points[0U], points[2U], points[3U], accent, accent, accent);
    return true;
}

struct PlateAccumulator final {
    double sum_x = 0.0;
    double sum_y = 0.0;
    double center_x = 0.0;
    double center_y = 0.0;
    double nearest_distance = std::numeric_limits<double>::infinity();
    uint32_t count = 0U;
    uint32_t center_index = 0U;
};

void buildPlateMotions(
    S2MapImage& image,
    std::span<int16_t const> plate_ids,
    std::span<float const> vector_x,
    std::span<float const> vector_y,
    uint32_t const resolution
)
{
    if (plate_ids.empty() || plate_ids.size() != vector_x.size() || plate_ids.size() != vector_y.size()) {
        return;
    }
    int16_t const maximum_id = *std::max_element(plate_ids.begin(), plate_ids.end());
    if (maximum_id < 0) {
        return;
    }
    std::vector<PlateAccumulator> plates(static_cast<size_t>(maximum_id) + 1U);
    for (size_t index = 0U; index < plate_ids.size(); ++index) {
        int16_t const id = plate_ids[index];
        if (id < 0) {
            continue;
        }
        PlateAccumulator& plate = plates[static_cast<size_t>(id)];
        plate.sum_x += static_cast<double>(index % resolution);
        plate.sum_y += static_cast<double>(index / resolution);
        ++plate.count;
    }
    for (PlateAccumulator& plate : plates) {
        if (plate.count > 0U) {
            plate.center_x = plate.sum_x / static_cast<double>(plate.count);
            plate.center_y = plate.sum_y / static_cast<double>(plate.count);
        }
    }
    for (size_t index = 0U; index < plate_ids.size(); ++index) {
        int16_t const id = plate_ids[index];
        if (id < 0) {
            continue;
        }
        PlateAccumulator& plate = plates[static_cast<size_t>(id)];
        double const x = static_cast<double>(index % resolution);
        double const y = static_cast<double>(index / resolution);
        double const dx = x - plate.center_x;
        double const dy = y - plate.center_y;
        double const distance = dx * dx + dy * dy;
        if (distance < plate.nearest_distance) {
            plate.nearest_distance = distance;
            plate.center_index = static_cast<uint32_t>(index);
        }
    }
    size_t const plate_count = static_cast<size_t>(std::count_if(
        plates.begin(), plates.end(), [](PlateAccumulator const& plate) { return plate.count > 0U; }));
    image.plate_motions.reserve(plate_count);
    for (size_t id = 0U; id < plates.size(); ++id) {
        PlateAccumulator const& plate = plates[id];
        if (plate.count == 0U) {
            continue;
        }
        image.plate_motions.push_back({
            .id = static_cast<uint32_t>(id),
            .x = (static_cast<float>(plate.center_index % resolution) + 0.5F) / static_cast<float>(resolution),
            .y = (static_cast<float>(plate.center_index / resolution) + 0.5F) / static_cast<float>(resolution),
            .vector_x = vector_x[plate.center_index],
            .vector_y = vector_y[plate.center_index],
        });
    }
}

template <typename TPlateId>
void buildPlateBoundarySegments(
    S2MapImage& image,
    std::span<TPlateId const> plate_ids,
    uint32_t const resolution
)
{
    size_t const count = static_cast<size_t>(resolution) * resolution;
    if (resolution == 0U || plate_ids.size() != count) {
        return;
    }
    float const scale = 1.0F / static_cast<float>(resolution);
    auto append_edge = [&](uint32_t x, uint32_t y, bool vertical) {
        S2PlateBoundarySegment edge{
            .start = {static_cast<float>(x) * scale, static_cast<float>(y) * scale},
            .end = {static_cast<float>(x + (vertical ? 0U : 1U)) * scale,
                static_cast<float>(y + (vertical ? 1U : 0U)) * scale},
        };
        image.plate_boundaries.push_back(edge);
        if (vertical && x == 0U) {
            edge.start[0U] = edge.end[0U] = 1.0F;
            image.plate_boundaries.push_back(edge);
        } else if (!vertical && y == 0U) {
            edge.start[1U] = edge.end[1U] = 1.0F;
            image.plate_boundaries.push_back(edge);
        }
    };
    // Unit edges retain T junctions that used to land inside an already merged run.
    // Simplification may remove collinear points only: a diagonal shortcut can cross
    // another ownership boundary or erase a one-cell-wide plate corridor.
    for (uint32_t y = 0U; y < resolution; ++y) {
        for (uint32_t x = 0U; x < resolution; ++x) {
            auto const id = plate_ids[static_cast<size_t>(y) * resolution + x];
            if (id != plate_ids[static_cast<size_t>(y) * resolution + (x + resolution - 1U) % resolution]) {
                append_edge(x, y, true);
            }
            if (id != plate_ids[static_cast<size_t>((y + resolution - 1U) % resolution) * resolution + x]) {
                append_edge(x, y, false);
            }
        }
    }
    // Join the ownership-grid runs before drawing. Dropping short runs to fit the
    // GUI budget leaves holes in every staircase boundary; simplify whole chains
    // instead, retaining their endpoints and their connected shape.
    auto const runs = std::move(image.plate_boundaries);
    image.plate_boundaries.clear();
    auto const key = [resolution](std::array<float, 2U> const point) {
        uint64_t const x = static_cast<uint64_t>(std::lround(point[0U] * resolution));
        uint64_t const y = static_cast<uint64_t>(std::lround(point[1U] * resolution));
        return y * (static_cast<uint64_t>(resolution) + 1U) + x;
    };
    std::unordered_map<uint64_t, std::vector<size_t>> adjacent;
    for (size_t index = 0U; index < runs.size(); ++index) {
        adjacent[key(runs[index].start)].push_back(index);
        adjacent[key(runs[index].end)].push_back(index);
    }
    std::vector<bool> used(runs.size(), false);
    float const tolerance = 0.0F;
    auto const appendChain = [&](size_t const first_edge, std::array<float, 2U> start) {
        std::vector<std::array<float, 2U>> points{start};
        size_t edge = first_edge;
        while (!used[edge]) {
            used[edge] = true;
            auto const& run = runs[edge];
            auto const end = key(run.start) == key(start) ? run.end : run.start;
            points.push_back(end);
            auto const& next_edges = adjacent.at(key(end));
            if (next_edges.size() != 2U) {
                break;
            }
            edge = next_edges[0U] == edge ? next_edges[1U] : next_edges[0U];
            start = end;
        }
        // Iterative Douglas-Peucker preserves closed loops and shared endpoints.
        std::vector<std::pair<size_t, size_t>> pending{{0U, points.size() - 1U}};
        std::vector<bool> keep(points.size(), false);
        keep.front() = true;
        keep.back() = true;
        while (!pending.empty()) {
            auto const [first, last] = pending.back();
            pending.pop_back();
            float const dx = points[last][0U] - points[first][0U];
            float const dy = points[last][1U] - points[first][1U];
            float const length_squared = dx * dx + dy * dy;
            float maximum_distance = tolerance * tolerance;
            size_t split = first;
            for (size_t index = first + 1U; index < last; ++index) {
                float const px = points[index][0U] - points[first][0U];
                float const py = points[index][1U] - points[first][1U];
                float const t = length_squared > 0.0F
                    ? std::clamp((px * dx + py * dy) / length_squared, 0.0F, 1.0F) : 0.0F;
                float const distance = (px - t * dx) * (px - t * dx) + (py - t * dy) * (py - t * dy);
                if (distance > maximum_distance) {
                    maximum_distance = distance;
                    split = index;
                }
            }
            if (split != first) {
                keep[split] = true;
                pending.emplace_back(first, split);
                pending.emplace_back(split, last);
            }
        }
        size_t previous = 0U;
        for (size_t index = 1U; index < points.size(); ++index) {
            if (keep[index]) {
                image.plate_boundaries.push_back({.start = points[previous], .end = points[index]});
                previous = index;
            }
        }
    };
    for (size_t index = 0U; index < runs.size(); ++index) {
        if (!used[index] && adjacent.at(key(runs[index].start)).size() != 2U) {
            appendChain(index, runs[index].start);
        }
        if (!used[index] && adjacent.at(key(runs[index].end)).size() != 2U) {
            appendChain(index, runs[index].end);
        }
    }
    for (size_t index = 0U; index < runs.size(); ++index) {
        if (!used[index]) {
            appendChain(index, runs[index].start);
        }
    }
}

[[nodiscard]]
bool mapPointForSource(
    S2MapLayout const& map_layout,
    float const source_x,
    float const source_y,
    ScreenPoint& screen,
    ScreenRect& viewport,
    ScreenPoint const* const near_screen = nullptr
) noexcept
{
    uint32_t const draw_count = std::min<uint32_t>(map_layout.world_draw_count,
        static_cast<uint32_t>(map_layout.world_draws.size()));
    float best_distance = std::numeric_limits<float>::infinity();
    bool found = false;
    for (uint32_t draw_index = 0U; draw_index < draw_count; ++draw_index) {
        MapImageDraw const& draw = map_layout.world_draws[draw_index];
        if (!validMapImageDraw(draw) || draw.image != 0U
            || source_x < draw.source[0U] || source_x >= draw.source[0U] + draw.source[2U]
            || source_y < draw.source[1U] || source_y >= draw.source[1U] + draw.source[3U]) {
            continue;
        }
        ScreenPoint const candidate_screen{
            draw.destination[0U] + (source_x - draw.source[0U]) / draw.source[2U] * draw.destination[2U],
            draw.destination[1U] + (source_y - draw.source[1U]) / draw.source[3U] * draw.destination[3U],
        };
        ScreenRect candidate_viewport{
            draw.destination[0U],
            draw.destination[1U],
            draw.destination[0U] + draw.destination[2U],
            draw.destination[1U] + draw.destination[3U],
        };
        if (draw.scissor[2U] != 0U) {
            ScreenRect const scissor{
                static_cast<float>(draw.scissor[0U]),
                static_cast<float>(draw.scissor[1U]),
                static_cast<float>(draw.scissor[0U] + draw.scissor[2U]),
                static_cast<float>(draw.scissor[1U] + draw.scissor[3U]),
            };
            candidate_viewport.left = std::max(candidate_viewport.left, scissor.left);
            candidate_viewport.top = std::max(candidate_viewport.top, scissor.top);
            candidate_viewport.right = std::min(candidate_viewport.right, scissor.right);
            candidate_viewport.bottom = std::min(candidate_viewport.bottom, scissor.bottom);
        }
        float const distance = near_screen == nullptr
            ? 0.0F
            : std::hypot(candidate_screen[0U] - (*near_screen)[0U], candidate_screen[1U] - (*near_screen)[1U]);
        if (!found || distance < best_distance) {
            best_distance = distance;
            found = true;
            screen = candidate_screen;
            viewport = candidate_viewport;
        }
    }
    return found;
}

void appendPlateBoundaryOverlay(
    std::vector<GuiVectorVertex>& output,
    std::span<S2PlateBoundarySegment const> const segments,
    S2MapLayout const& map_layout,
    float const dpi_scale,
    std::span<ScreenRect const> const excluded_rectangles
)
{
    constexpr size_t MAX_BOUNDARY_VERTEX_BUDGET = MAX_GUI_TRIANGLE_VERTICES;
    constexpr size_t VERTICES_PER_SEGMENT = 12U;
    size_t const available_vertices = std::min(MAX_BOUNDARY_VERTEX_BUDGET,
        static_cast<size_t>(MAX_GUI_TRIANGLE_VERTICES));
    size_t const maximum_segments = available_vertices / VERTICES_PER_SEGMENT;
    if (segments.empty() || maximum_segments == 0U) {
        return;
    }
    float const margin = 3.0F * dpi_scale;
    float const foreground_width = 2.8F * dpi_scale;
    float const outline_width = foreground_width + 2.0F * dpi_scale;
    struct VisibleRange final {
        float first = 0.0F;
        float last = 1.0F;
    };
    struct VisibleStroke final {
        ScreenPoint start;
        ScreenPoint end;
        ScreenPoint normal;
        bool cap_start;
        bool cap_end;
    };
    std::vector<VisibleStroke> strokes;
    for (size_t segment_index = 0U;
         segment_index < segments.size();
         ++segment_index) {
        S2PlateBoundarySegment const& segment = segments[segment_index];
        for (uint32_t draw_index = 0U; draw_index < map_layout.world_draw_count; ++draw_index) {
            MapImageDraw const& draw = map_layout.world_draws[draw_index];
            if (!validMapImageDraw(draw) || draw.image != 0U) {
                continue;
            }
            ScreenPoint source_start = segment.start;
            ScreenPoint source_end = segment.end;
            ScreenRect const source_viewport{draw.source[0U], draw.source[1U],
                draw.source[0U] + draw.source[2U], draw.source[1U] + draw.source[3U]};
            if (!clipSegmentToRect(source_start, source_end, source_viewport)) {
                continue;
            }
            float const source_x0 = source_start[0U];
            float const source_y0 = source_start[1U];
            float const source_x1 = source_end[0U];
            float const source_y1 = source_end[1U];
            bool const vertical = std::abs(source_x1 - source_x0) < 1.0e-6F;
            ScreenPoint start{
                draw.destination[0U] + (source_x0 - draw.source[0U]) / draw.source[2U] * draw.destination[2U],
                draw.destination[1U] + (source_y0 - draw.source[1U]) / draw.source[3U] * draw.destination[3U],
            };
            ScreenPoint end{
                draw.destination[0U] + (source_x1 - draw.source[0U]) / draw.source[2U] * draw.destination[2U],
                draw.destination[1U] + (source_y1 - draw.source[1U]) / draw.source[3U] * draw.destination[3U],
            };
            ScreenRect viewport{
                draw.destination[0U], draw.destination[1U],
                draw.destination[0U] + draw.destination[2U], draw.destination[1U] + draw.destination[3U],
            };
            if (draw.scissor[2U] != 0U) {
                viewport.left = std::max(viewport.left, static_cast<float>(draw.scissor[0U]));
                viewport.top = std::max(viewport.top, static_cast<float>(draw.scissor[1U]));
                viewport.right = std::min(viewport.right,
                    static_cast<float>(draw.scissor[0U] + draw.scissor[2U]));
                viewport.bottom = std::min(viewport.bottom,
                    static_cast<float>(draw.scissor[1U] + draw.scissor[3U]));
            }
            float const scissor_left = static_cast<float>(draw.scissor[0U]);
            float const scissor_top = static_cast<float>(draw.scissor[1U]);
            float const scissor_right = static_cast<float>(draw.scissor[0U] + draw.scissor[2U]);
            float const scissor_bottom = static_cast<float>(draw.scissor[1U] + draw.scissor[3U]);
            bool const has_scissor = draw.scissor[2U] != 0U && draw.scissor[3U] != 0U;
            bool const outer_left = has_scissor && viewport.left <= scissor_left + 1.0e-3F;
            bool const outer_top = has_scissor && viewport.top <= scissor_top + 1.0e-3F;
            bool const outer_right = has_scissor && viewport.right >= scissor_right - 1.0e-3F;
            bool const outer_bottom = has_scissor && viewport.bottom >= scissor_bottom - 1.0e-3F;
            float const inset_left = outer_left ? std::min(margin, (viewport.right - viewport.left) * 0.5F) : 0.0F;
            float const inset_top = outer_top ? std::min(margin, (viewport.bottom - viewport.top) * 0.5F) : 0.0F;
            float const inset_right = outer_right ? std::min(margin, (viewport.right - viewport.left) * 0.5F) : 0.0F;
            float const inset_bottom = outer_bottom ? std::min(margin, (viewport.bottom - viewport.top) * 0.5F) : 0.0F;
            if (vertical && (segment.start[0U] == 0.0F || segment.start[0U] == 1.0F)) {
                if (outer_left && start[0U] <= viewport.left + 1.0e-3F) {
                    start[0U] += inset_left;
                } else if (outer_right && start[0U] >= viewport.right - 1.0e-3F) {
                    start[0U] -= inset_right;
                }
                end[0U] = start[0U];
            } else if (std::abs(source_y1 - source_y0) < 1.0e-6F
                && (segment.start[1U] == 0.0F || segment.start[1U] == 1.0F)) {
                if (outer_top && start[1U] <= viewport.top + 1.0e-3F) {
                    start[1U] += inset_top;
                } else if (outer_bottom && start[1U] >= viewport.bottom - 1.0e-3F) {
                    start[1U] -= inset_bottom;
                }
                end[1U] = start[1U];
            }
            viewport = {viewport.left + inset_left, viewport.top + inset_top,
                viewport.right - inset_right, viewport.bottom - inset_bottom};
            ScreenPoint const unclipped_start = start;
            ScreenPoint const unclipped_end = end;
            if (!clipSegmentToRect(start, end, viewport)) {
                continue;
            }
            std::array<VisibleRange, 16U> ranges{};
            ranges[0U] = {};
            size_t range_count = 1U;
            float const dx = end[0U] - start[0U];
            float const dy = end[1U] - start[1U];
            float const length_squared = dx * dx + dy * dy;
            if (length_squared <= 1.0e-8F) {
                continue;
            }
            for (ScreenRect const& exclusion : excluded_rectangles) {
                std::array<VisibleRange, 16U> remaining{};
                size_t remaining_count = 0U;
                ScreenRect const expanded{
                    exclusion.left - margin, exclusion.top - margin,
                    exclusion.right + margin, exclusion.bottom + margin,
                };
                for (size_t range_index = 0U; range_index < range_count; ++range_index) {
                    VisibleRange const range = ranges[range_index];
                    ScreenPoint cut_start{
                        std::lerp(start[0U], end[0U], range.first),
                        std::lerp(start[1U], end[1U], range.first),
                    };
                    ScreenPoint cut_end{
                        std::lerp(start[0U], end[0U], range.last),
                        std::lerp(start[1U], end[1U], range.last),
                    };
                    if (!clipSegmentToRect(cut_start, cut_end, expanded)) {
                        remaining[remaining_count++] = range;
                        continue;
                    }
                    float const cut_first = std::clamp(
                        ((cut_start[0U] - start[0U]) * dx + (cut_start[1U] - start[1U]) * dy)
                            / length_squared,
                        range.first,
                        range.last
                    );
                    float const cut_last = std::clamp(
                        ((cut_end[0U] - start[0U]) * dx + (cut_end[1U] - start[1U]) * dy)
                            / length_squared,
                        range.first,
                        range.last
                    );
                    if (cut_first > range.first + 1.0e-5F) {
                        remaining[remaining_count++] = {range.first, cut_first};
                    }
                    if (cut_last < range.last - 1.0e-5F) {
                        remaining[remaining_count++] = {cut_last, range.last};
                    }
                }
                ranges = remaining;
                range_count = remaining_count;
                if (range_count == 0U) {
                    break;
                }
            }
            for (size_t range_index = 0U; range_index < range_count; ++range_index) {
                if (output.size() + (strokes.size() + 1U) * VERTICES_PER_SEGMENT > MAX_GUI_TRIANGLE_VERTICES
                    || (strokes.size() + 1U) * VERTICES_PER_SEGMENT > available_vertices) {
                    continue;
                }
                VisibleRange const range = ranges[range_index];
                ScreenPoint const visible_start{
                    std::lerp(start[0U], end[0U], range.first),
                    std::lerp(start[1U], end[1U], range.first),
                };
                ScreenPoint const visible_end{
                    std::lerp(start[0U], end[0U], range.last),
                    std::lerp(start[1U], end[1U], range.last),
                };
                float const visible_dx = visible_end[0U] - visible_start[0U];
                float const visible_dy = visible_end[1U] - visible_start[1U];
                float const visible_length = std::hypot(visible_dx, visible_dy);
                if (visible_length < 1.0e-4F) {
                    continue;
                }
                ScreenPoint const normal{-visible_dy / visible_length, visible_dx / visible_length};
                strokes.push_back({visible_start, visible_end, normal,
                    source_start == segment.start && start == unclipped_start && range.first == 0.0F,
                    source_end == segment.end && end == unclipped_end && range.last == 1.0F});
            }
        }
    }
    // Draw every outline before any foreground so adjoining outlines cannot
    // erase an already drawn foreground at a corner or junction.
    for (auto const [width, color] : {std::pair{outline_width, pixelColor(VECTOR_OUTLINE_COLOR)},
             std::pair{foreground_width, pixelColor(PLATE_COLOR)}}) {
        float const half = width * 0.5F;
        for (VisibleStroke const& stroke : strokes) {
            ScreenPoint start = stroke.start;
            ScreenPoint end = stroke.end;
            // Square caps fill the outside quadrant of an ownership-grid corner.
            // Axis-aligned caps stay within the margin used for viewport and HUD clipping.
            if (stroke.normal[0U] == 0.0F || stroke.normal[1U] == 0.0F) {
                ScreenPoint const tangent{stroke.normal[1U], -stroke.normal[0U]};
                if (stroke.cap_start) {
                    start = offsetPoint(start, tangent, -half);
                }
                if (stroke.cap_end) {
                    end = offsetPoint(end, tangent, half);
                }
            }
            emitSolidQuad(output, offsetPoint(start, stroke.normal, half),
                offsetPoint(end, stroke.normal, half), offsetPoint(end, stroke.normal, -half),
                offsetPoint(start, stroke.normal, -half), color);
        }
    }
}

[[nodiscard]]
std::array<float, 2U> sampleFlowField(
    std::span<float const> const u_field,
    std::span<float const> const v_field,
    uint32_t const resolution,
    float const grid_x,
    float const grid_y
) noexcept
{
    size_t const count = static_cast<size_t>(resolution) * resolution;
    if (resolution == 0U || u_field.size() != count || v_field.size() != count) {
        return {};
    }
    float const sample_x = grid_x - 0.5F;
    float const sample_y = grid_y - 0.5F;
    int64_t const x0_unwrapped = static_cast<int64_t>(std::floor(sample_x));
    int64_t const y0_unwrapped = static_cast<int64_t>(std::floor(sample_y));
    float const tx = sample_x - static_cast<float>(x0_unwrapped);
    float const ty = sample_y - static_cast<float>(y0_unwrapped);
    int64_t const signed_resolution = static_cast<int64_t>(resolution);
    auto const wrap = [signed_resolution](int64_t value) {
        value %= signed_resolution;
        return value < 0 ? value + signed_resolution : value;
    };
    uint32_t const x0 = static_cast<uint32_t>(wrap(x0_unwrapped));
    uint32_t const y0 = static_cast<uint32_t>(wrap(y0_unwrapped));
    uint32_t const x1 = (x0 + 1U) % resolution;
    uint32_t const y1 = (y0 + 1U) % resolution;
    auto const interpolate = [&](std::span<float const> const field) {
        float const top = std::lerp(field[static_cast<size_t>(y0) * resolution + x0],
            field[static_cast<size_t>(y0) * resolution + x1], tx);
        float const bottom = std::lerp(field[static_cast<size_t>(y1) * resolution + x0],
            field[static_cast<size_t>(y1) * resolution + x1], tx);
        return std::lerp(top, bottom, ty);
    };
    float const u = interpolate(u_field);
    return {u, u_field.data() == v_field.data() ? u : interpolate(v_field)};
}

[[nodiscard]]
bool oceanMaskCell(
    std::span<uint8_t const> const ocean_mask,
    uint32_t const mask_resolution,
    int64_t const x,
    int64_t const y
) noexcept
{
    size_t const mask_size = static_cast<size_t>(mask_resolution) * mask_resolution;
    if (mask_resolution == 0U || ocean_mask.size() != mask_size) {
        return false;
    }
    int64_t const signed_resolution = static_cast<int64_t>(mask_resolution);
    auto const wrap = [signed_resolution](int64_t value) {
        value %= signed_resolution;
        return value < 0 ? value + signed_resolution : value;
    };
    uint32_t const cell_x = static_cast<uint32_t>(wrap(x));
    uint32_t const cell_y = static_cast<uint32_t>(wrap(y));
    return ocean_mask[static_cast<size_t>(cell_y) * mask_resolution + cell_x] != 0U;
}

[[nodiscard]]
bool oceanPathSegment(
    std::span<uint8_t const> const ocean_mask,
    uint32_t const mask_resolution,
    uint32_t const flow_resolution,
    float const start_x,
    float const start_y,
    float const end_x,
    float const end_y
) noexcept
{
    if (ocean_mask.empty()) {
        return true;
    }
    if (flow_resolution == 0U || !std::isfinite(start_x) || !std::isfinite(start_y)
        || !std::isfinite(end_x) || !std::isfinite(end_y)
        || ocean_mask.size() != static_cast<size_t>(mask_resolution) * mask_resolution
        || mask_resolution == 0U) {
        return false;
    }
    float const scale = static_cast<float>(mask_resolution) / static_cast<float>(flow_resolution);
    float const mask_dx = (end_x - start_x) * scale;
    float const mask_dy = (end_y - start_y) * scale;
    uint32_t const sample_count = std::max(1U, static_cast<uint32_t>(std::ceil(
        std::max(std::abs(mask_dx), std::abs(mask_dy)) * 2.0F)));
    for (uint32_t sample = 0U; sample <= sample_count; ++sample) {
        float const amount = static_cast<float>(sample) / static_cast<float>(sample_count);
        int64_t const cell_x = static_cast<int64_t>(std::floor(
            (start_x + (end_x - start_x) * amount) * scale));
        int64_t const cell_y = static_cast<int64_t>(std::floor(
            (start_y + (end_y - start_y) * amount) * scale));
        // Half-cell steps and adjacent mask cells conservatively cover cells touched by the path.
        for (int64_t offset_y = -1; offset_y <= 1; ++offset_y) {
            for (int64_t offset_x = -1; offset_x <= 1; ++offset_x) {
                if (!oceanMaskCell(ocean_mask, mask_resolution, cell_x + offset_x, cell_y + offset_y)) {
                    return false;
                }
            }
        }
    }
    return true;
}

void appendWorldMapVectors(
    S2WorldMapState const& state,
    S2MapImage const& image,
    generation::GlobalAtlas const& atlas,
    S2MapLayout const& map_layout,
    S2MapOverlayLayout const& overlay_layout,
    uint32_t const width,
    uint32_t const height,
    float const dpi_scale,
    TextRenderer& text,
    std::vector<GuiVectorVertex>& output,
    float const flow_phase
)
{
    bool const v9 = atlas.configuration.model_tag == generation::TerrainModelTag::V9;
    bool const v10 = atlas.configuration.model_tag == generation::TerrainModelTag::V10;
    bool const v21 = atlas.configuration.model_tag == generation::TerrainModelTag::V21;
    bool const native = v9 || v10 || v21;
    uint32_t const resolution = native ? image.vector_field_resolution : atlas.configuration.resolution;
    size_t const atlas_size = static_cast<size_t>(atlas.configuration.resolution)
        * atlas.configuration.resolution;
    size_t const vector_size = static_cast<size_t>(resolution) * resolution;
    std::span<uint8_t const> const ocean_cells = v10 ? std::span<uint8_t const>{image.v10_ocean_cells}
        : v21 ? std::span<uint8_t const>{image.v21_ocean_cells}
        : (v9 ? std::span<uint8_t const>{image.v9_ocean_cells}
            : std::span<uint8_t const>{atlas.ocean_mask});
    uint32_t const ocean_mask_resolution = native ? image.ocean_mask_resolution : atlas.configuration.resolution;
    size_t const ocean_mask_size = static_cast<size_t>(ocean_mask_resolution) * ocean_mask_resolution;
    bool const current_ocean_mask_valid = ocean_mask_resolution != 0U && ocean_cells.size() == ocean_mask_size
        && (!native || ocean_mask_resolution == image.source_resolution);
    if ((atlas.configuration.resolution != 256U && atlas.configuration.resolution != 512U)
        || resolution == 0U || vector_size == 0U) {
        return;
    }
    bool const show_wind = state.layers[static_cast<size_t>(S2WorldMapLayer::Wind)]
        && image.available_layers[static_cast<size_t>(S2WorldMapLayer::Wind)]
        && (native
            ? (v10 ? image.v10_wind_u_m_s.size() == vector_size && image.v10_wind_v_m_s.size() == vector_size
                : v21 ? image.v21_wind_u_m_s.size() == vector_size && image.v21_wind_v_m_s.size() == vector_size
                : image.v9_wind_u_m_s.size() == vector_size && image.v9_wind_v_m_s.size() == vector_size)
            : atlas.wind_u.size() == atlas_size && atlas.wind_v.size() == atlas_size);
    bool const show_currents = state.layers[static_cast<size_t>(S2WorldMapLayer::Currents)]
        && image.available_layers[static_cast<size_t>(S2WorldMapLayer::Currents)]
        && current_ocean_mask_valid
        && (native ? (v10 ? image.v10_ocean_u_m_s.size() == vector_size && image.v10_ocean_v_m_s.size() == vector_size
            : v21 ? image.v21_ocean_u_m_s.size() == vector_size
                && image.v21_ocean_v_m_s.size() == vector_size
                : image.v9_ocean_u_m_s.size() == vector_size && image.v9_ocean_v_m_s.size() == vector_size
            )
            : atlas.current_u.size() == atlas_size && atlas.current_v.size() == atlas_size
                && atlas.ocean_mask.size() == atlas_size);
    bool const show_plates = state.layers[static_cast<size_t>(S2WorldMapLayer::Plates)]
        && image.available_layers[static_cast<size_t>(S2WorldMapLayer::Plates)];
    uint32_t const precipitation_resolution = native ? image.source_resolution : atlas.configuration.resolution;
    size_t const precipitation_size = static_cast<size_t>(precipitation_resolution) * precipitation_resolution;
    std::span<float const> const precipitation_field = v10 ? std::span<float const>{image.v10_precipitation_mean_mm_year}
        : v21 ? std::span<float const>{image.v21_precipitation_mean_mm_year}
        : (v9 ? std::span<float const>{image.v9_precipitation_mean_mm_year}
            : std::span<float const>{atlas.precipitation});
    float const precipitation_maximum = image.layer_ranges[static_cast<size_t>(S2WorldMapLayer::Precipitation)][1U];
    bool const show_rain = state.layers[static_cast<size_t>(S2WorldMapLayer::Precipitation)]
        && image.available_layers[static_cast<size_t>(S2WorldMapLayer::Precipitation)]
        && precipitation_field.size() == precipitation_size && precipitation_maximum > 0.0F;
    if (!show_wind && !show_currents && !show_plates && !show_rain) {
        return;
    }

    std::span<float const> const wind_u = v10 ? std::span<float const>{image.v10_wind_u_m_s}
        : v21 ? std::span<float const>{image.v21_wind_u_m_s}
        : (v9 ? std::span<float const>{image.v9_wind_u_m_s} : std::span<float const>{atlas.wind_u});
    std::span<float const> const wind_v = v10 ? std::span<float const>{image.v10_wind_v_m_s}
        : v21 ? std::span<float const>{image.v21_wind_v_m_s}
        : (v9 ? std::span<float const>{image.v9_wind_v_m_s} : std::span<float const>{atlas.wind_v});
    std::span<float const> const current_u = v10 ? std::span<float const>{image.v10_ocean_u_m_s}
        : v21 ? std::span<float const>{image.v21_ocean_u_m_s}
        : (v9 ? std::span<float const>{image.v9_ocean_u_m_s} : std::span<float const>{atlas.current_u});
    std::span<float const> const current_v = v10 ? std::span<float const>{image.v10_ocean_v_m_s}
        : v21 ? std::span<float const>{image.v21_ocean_v_m_s}
        : (v9 ? std::span<float const>{image.v9_ocean_v_m_s} : std::span<float const>{atlas.current_v});

    std::array<ScreenRect, 15U> exclusions{};
    size_t exclusion_count = 0U;
    auto append_exclusions = [&](auto const& rectangles, uint32_t const count) {
        uint32_t const bounded_count = std::min<uint32_t>(count, static_cast<uint32_t>(rectangles.size()));
        for (uint32_t index = 0U; index < bounded_count && exclusion_count < exclusions.size(); ++index) {
            auto const& bounds = rectangles[index].bounds;
            exclusions[exclusion_count++] = {
                bounds[0U], bounds[1U], bounds[0U] + bounds[2U], bounds[1U] + bounds[3U],
            };
        }
    };
    append_exclusions(overlay_layout.panels, overlay_layout.panel_count);
    append_exclusions(overlay_layout.heat_strips, overlay_layout.heat_strip_count);
    append_exclusions(overlay_layout.color_chips, overlay_layout.color_chip_count);
    std::span<ScreenRect const> const excluded_rectangles(exclusions.data(), exclusion_count);

    bool const compact = static_cast<float>(height) / dpi_scale < 500.0F
        || static_cast<float>(width) / dpi_scale < 1'080.0F;
    if (show_plates) {
        appendPlateBoundaryOverlay(output, image.plate_boundaries, map_layout, dpi_scale, excluded_rectangles);
    }
    size_t const weather_start = output.size();
    appendS2MapWeatherTriangles(output, map_layout, {
        .resolution = resolution,
        .ocean_resolution = ocean_mask_resolution,
        .precipitation_resolution = precipitation_resolution,
        .wind_u = wind_u, .wind_v = wind_v, .current_u = current_u, .current_v = current_v,
        .precipitation = precipitation_field, .ocean = ocean_cells,
        .precipitation_maximum = native ? 1'000.0F : precipitation_maximum,
    }, flow_phase, dpi_scale, show_wind, show_currents, show_rain);
    // Excluded geometry is absent from the batch, not merely transparent. Bounds
    // include the fringe, so no part of a streak crosses a legend or control.
    size_t visible_end = weather_start;
    for (size_t first = weather_start; first + 2U < output.size(); first += 3U) {
        ScreenRect bounds{output[first].position[0U], output[first].position[1U],
            output[first].position[0U], output[first].position[1U]};
        for (size_t vertex = first + 1U; vertex < first + 3U; ++vertex) {
            auto const point = output[vertex].position;
            bounds.left = std::min(bounds.left, point[0U]);
            bounds.right = std::max(bounds.right, point[0U]);
            bounds.top = std::min(bounds.top, point[1U]);
            bounds.bottom = std::max(bounds.bottom, point[1U]);
        }
        bool const hidden = std::any_of(excluded_rectangles.begin(), excluded_rectangles.end(),
            [&](ScreenRect const& rect) { return intersects(bounds, rect); });
        if (hidden) {
            continue;
        }
        for (size_t vertex = first; vertex < first + 3U; ++vertex) {
            output[visible_end++] = output[vertex];
        }
    }
    output.resize(visible_end);

    if (!show_plates) {
        return;
    }
    for (S2PlateMotion const& plate : image.plate_motions) {
        ScreenPoint screen{};
        ScreenRect viewport{};
        if (!mapPointForSource(map_layout, plate.x, plate.y, screen, viewport)) {
            continue;
        }
        ScreenPoint const center{screen[0U], screen[1U] - 10.0F * dpi_scale};
        float const length = (compact ? 64.0F : 72.0F) * dpi_scale;
        static_cast<void>(appendScreenArrow(output, center, plate.vector_x, plate.vector_y, length,
            (compact ? 3.2F : 3.8F) * dpi_scale, PLATE_MOTION_COLOR, viewport, excluded_rectangles));

        std::string const label = "P" + std::to_string(plate.id);
        ScreenRect const label_bounds{
            screen[0U] - 8.0F * dpi_scale,
            screen[1U] + 4.0F * dpi_scale,
            screen[0U] - 8.0F * dpi_scale + static_cast<float>(label.size()) * 8.0F * dpi_scale,
            screen[1U] + 20.0F * dpi_scale,
        };
        bool const panel_overlap = std::any_of(excluded_rectangles.begin(), excluded_rectangles.end(),
            [&](ScreenRect const& rect) { return intersects(label_bounds, rect); });
        if (contains(viewport, label_bounds) && !panel_overlap) {
            submitMapText(text, label, PLATE_MOTION_COLOR, screen[0U] / dpi_scale - 8.0F,
                screen[1U] / dpi_scale + 4.0F, dpi_scale, true);
        }
    }
}

[[nodiscard]]
std::optional<S2MapImage> rasterizeS2NativeWorldMap(
    shared::TerrainWorld const& world,
    shared::TerrainLodArtifact const& world_lod,
    S2WorldMapState const& state, bool use_river_geometry, std::stop_token const cancellation
)
{
    throwIfRasterCancelled(cancellation);
    auto const atlas = world.atlas();
    if (!atlas || !validL0Source(world, world_lod)) {
        return std::nullopt;
    }
    bool const v10 = atlas->configuration.model_tag == generation::TerrainModelTag::V10;
    bool const v21 = atlas->configuration.model_tag == generation::TerrainModelTag::V21;
    constexpr uint32_t SOURCE_RESOLUTION = 256U;
    constexpr uint32_t CLIMATE_RESOLUTION = 32U;
    constexpr size_t SOURCE_COUNT = static_cast<size_t>(SOURCE_RESOLUTION) * SOURCE_RESOLUTION;
    constexpr size_t CLIMATE_COUNT = static_cast<size_t>(CLIMATE_RESOLUTION) * CLIMATE_RESOLUTION;
    constexpr size_t NATIVE_BIOME_COUNT = 13U;
    constexpr std::array<uint32_t, 2U> SOURCE_SHAPE{SOURCE_RESOLUTION, SOURCE_RESOLUTION};
    constexpr std::array<uint32_t, 3U> SOURCE_PHASE_SHAPE{8U, SOURCE_RESOLUTION, SOURCE_RESOLUTION};
    constexpr std::array<uint32_t, 3U> CLIMATE_PHASE_SHAPE{8U, CLIMATE_RESOLUTION, CLIMATE_RESOLUTION};

    std::vector<generation::V9RegisteredFieldView> const views = v10
        ? generation::v10RegisteredFields(*atlas)
        : v21
        ? generation::v21RegisteredFields(*atlas)
        : generation::v9RegisteredFields(*atlas);
    auto const* elevation = v9Field(views, v10 ? "height_m" : "elevation");
    auto const* temperature = v9Field(views, "temperature_C");
    auto const* precipitation = v9Field(views, "precipitation_mm_year");
    auto const* biome = v9Field(views, "biome_id");
    auto const* ocean = v9Field(views, "ocean");
    auto const* lake = v9Field(views, "lake");
    auto const* river = v9Field(views, "river");
    auto const* water_level = v9Field(views, "water_level");
    auto const* wind_u = v9Field(views, "u");
    auto const* wind_v = v9Field(views, "v");
    auto const* ocean_u = v9Field(views, "ocean_u");
    auto const* ocean_v = v9Field(views, "ocean_v");
    auto const* moisture = v9Field(views, "bucket_kg_m2");
    auto const* land_snow = v9Field(views, "land_snow_water_equivalent_m");
    auto const* glacier = v9Field(views, "glacier_thickness_m");
    auto const* sea_ice = v9Field(views, "sea_ice_thickness_m");
    if (!hasV9Shape(elevation, SOURCE_SHAPE)
        || !hasV9Shape(temperature, SOURCE_PHASE_SHAPE)
        || !hasV9Shape(precipitation, SOURCE_PHASE_SHAPE)
        || !hasV9Shape(biome, SOURCE_SHAPE)
        || !hasV9Shape(ocean, SOURCE_SHAPE)
        || !hasV9Shape(lake, SOURCE_SHAPE)
        || !hasV9Shape(river, SOURCE_SHAPE)
        || !hasV9Shape(water_level, SOURCE_SHAPE)
        || !hasV9Shape(wind_u, CLIMATE_PHASE_SHAPE)
        || !hasV9Shape(wind_v, CLIMATE_PHASE_SHAPE)
        || !hasV9Shape(ocean_u, CLIMATE_PHASE_SHAPE)
        || !hasV9Shape(ocean_v, CLIMATE_PHASE_SHAPE)
        || !hasV9Shape(moisture, CLIMATE_PHASE_SHAPE)
        || !hasV9Shape(land_snow, SOURCE_PHASE_SHAPE)
        || !hasV9Shape(glacier, SOURCE_PHASE_SHAPE)
        || !hasV9Shape(sea_ice, SOURCE_PHASE_SHAPE)
        || !v9FiniteField(*elevation) || !v9FiniteField(*temperature)
        || !v9FiniteField(*precipitation) || !v9FiniteField(*water_level)
        || !v9FiniteField(*biome)
        || !v9FiniteField(*wind_u) || !v9FiniteField(*wind_v)
        || !v9FiniteField(*ocean_u) || !v9FiniteField(*ocean_v)
        || !v9FiniteField(*moisture) || !v9FiniteField(*land_snow)
        || !v9FiniteField(*glacier) || !v9FiniteField(*sea_ice)
        || !v9BooleanField(*ocean) || !v9BooleanField(*lake) || !v9BooleanField(*river)) {
        return std::nullopt;
    }
    std::span<uint32_t const> const plate_ids = v10
        ? generation::v10CurrentPlateIds(*atlas)
        : v21
        ? generation::v21CurrentPlateIds(*atlas)
        : generation::v9CurrentPlateIds(*atlas);
    std::vector<generation::V9PlateDirection> plate_directions;
    if (v10) {
        for (auto const& direction : generation::v10PlateDirections(*atlas)) {
            plate_directions.push_back({direction.plate_id, direction.x, direction.y});
        }
    } else {
        auto const source_directions = v21 ? generation::v21PlateDirections(*atlas)
            : generation::v9PlateDirections(*atlas);
        plate_directions.assign(source_directions.begin(), source_directions.end());
    }
    if (plate_ids.size() != SOURCE_COUNT) {
        return std::nullopt;
    }

    S2MapImage image;
    image.width = S2_WORLD_MAP_IMAGE_SIDE;
    image.height = S2_WORLD_MAP_IMAGE_SIDE;
    image.source_identity = world.identity();
    image.source_model_identity = world_lod.model_identity;
    image.source_resolution = SOURCE_RESOLUTION;
    image.vector_field_resolution = CLIMATE_RESOLUTION;
    image.ocean_mask_resolution = SOURCE_RESOLUTION;
    auto& output_plate_ids = v10 ? image.v10_plate_ids : v21 ? image.v21_plate_ids : image.v9_plate_ids;
    auto& output_elevation_m = v10 ? image.v10_elevation_m : v21 ? image.v21_elevation_m : image.v9_elevation_m;
    auto& output_temperature = v10 ? image.v10_temperature_mean_c
        : v21 ? image.v21_temperature_mean_c : image.v9_temperature_mean_c;
    auto& output_precipitation = v10 ? image.v10_precipitation_mean_mm_year
        : v21 ? image.v21_precipitation_mean_mm_year : image.v9_precipitation_mean_mm_year;
    auto& output_biome_ids = v10 ? image.v10_biome_ids : v21 ? image.v21_biome_ids : image.v9_biome_ids;
    auto& output_moisture = v10 ? image.v10_moisture_mean_kg_m2
        : v21 ? image.v21_moisture_mean_kg_m2 : image.v9_moisture_mean_kg_m2;
    auto& output_wind_u = v10 ? image.v10_wind_u_m_s : v21 ? image.v21_wind_u_m_s : image.v9_wind_u_m_s;
    auto& output_wind_v = v10 ? image.v10_wind_v_m_s : v21 ? image.v21_wind_v_m_s : image.v9_wind_v_m_s;
    auto& output_ocean_u = v10 ? image.v10_ocean_u_m_s : v21 ? image.v21_ocean_u_m_s : image.v9_ocean_u_m_s;
    auto& output_ocean_v = v10 ? image.v10_ocean_v_m_s : v21 ? image.v21_ocean_v_m_s : image.v9_ocean_v_m_s;
    auto& output_ocean_cells = v10 ? image.v10_ocean_cells : v21 ? image.v21_ocean_cells : image.v9_ocean_cells;
    auto& output_ice_kind = v10 ? image.v10_ice_surface_kind : v21 ? image.v21_ice_surface_kind : image.v9_ice_surface_kind;
    output_plate_ids.assign(plate_ids.begin(), plate_ids.end());
    output_elevation_m.resize(SOURCE_COUNT);
    output_temperature = v9MeanPhaseField(*temperature, SOURCE_COUNT);
    output_precipitation = v9MeanPhaseField(*precipitation, SOURCE_COUNT);
    output_moisture = v9MeanPhaseField(*moisture, CLIMATE_COUNT);
    output_wind_u = v9MeanPhaseField(*wind_u, CLIMATE_COUNT);
    output_wind_v = v9MeanPhaseField(*wind_v, CLIMATE_COUNT);
    output_ocean_u = v9MeanPhaseField(*ocean_u, CLIMATE_COUNT);
    output_ocean_v = v9MeanPhaseField(*ocean_v, CLIMATE_COUNT);
    output_ocean_cells.resize(SOURCE_COUNT);
    output_biome_ids.resize(SOURCE_COUNT);
    output_ice_kind.resize(SOURCE_COUNT);
    std::vector<uint8_t> v10_water_cells;
    std::vector<float> v10_water_level_m;
    if (v10) {
        v10_water_cells.resize(SOURCE_COUNT);
        v10_water_level_m.resize(SOURCE_COUNT);
        image.v10_projected_inland_ice_kind.resize(SOURCE_COUNT);
        image.v10_land_snow_mean_m.resize(SOURCE_COUNT);
        image.v10_glacier_mean_m.resize(SOURCE_COUNT);
        image.v10_sea_ice_mean_m.resize(SOURCE_COUNT);
        image.v10_land_snow_phase0_m.resize(SOURCE_COUNT);
        image.v10_glacier_phase0_m.resize(SOURCE_COUNT);
        image.v10_sea_ice_phase0_m.resize(SOURCE_COUNT);
    }
    image.rgba.resize(WORLD_PIXEL_COUNT);
    for (size_t index = 0U; index < SOURCE_COUNT; ++index) {
        if ((index & 63U) == 0U)
        {
            throwIfRasterCancelled(cancellation);
        }
        output_elevation_m[index] = v9ValueAt(*elevation, index);
        output_ocean_cells[index] = static_cast<uint8_t>(v9ValueAt(*ocean, index) != 0.0);
        double const biome_id = v9ValueAt(*biome, index);
        if (biome_id < 0.0 || biome_id >= static_cast<double>(NATIVE_BIOME_COUNT)
            || std::floor(biome_id) != biome_id) {
            return std::nullopt;
        }
        output_biome_ids[index] = static_cast<uint8_t>(biome_id);
        generation::V10SurfaceCover v10_cover = generation::V10SurfaceCover::None;
        if (v10) {
            double const world_x_m = static_cast<double>(index % SOURCE_RESOLUTION) * 256.0;
            double const world_y_m = static_cast<double>(index / SOURCE_RESOLUTION) * 256.0;
            double temperature_sum = 0.0;
            double precipitation_sum = 0.0;
            double snow_sum = 0.0;
            double glacier_sum = 0.0;
            double sea_ice_sum = 0.0;
            auto const phases = generation::sampleV10SurfacePhases(*atlas, world_x_m, world_y_m, 0U);
            for (uint32_t phase = 0U; phase < 8U; ++phase) {
                auto const& sample = phases[phase];
                temperature_sum += sample.temperature_celsius;
                precipitation_sum += sample.precipitation_mm_year;
                snow_sum += sample.snow_water_equivalent_m;
                glacier_sum += sample.glacier_thickness_m;
                sea_ice_sum += sample.sea_ice_thickness_m;
                output_biome_ids[index] = sample.biome_id;
                image.v10_land_snow_mean_m[index] += static_cast<float>(sample.snow_water_equivalent_m / 8.0);
                image.v10_glacier_mean_m[index] += static_cast<float>(sample.glacier_thickness_m / 8.0);
                image.v10_sea_ice_mean_m[index] += static_cast<float>(sample.sea_ice_thickness_m / 8.0);
                if (phase == 0U) {
                    image.v10_land_snow_phase0_m[index] = static_cast<float>(sample.snow_water_equivalent_m);
                    image.v10_glacier_phase0_m[index] = static_cast<float>(sample.glacier_thickness_m);
                    image.v10_sea_ice_phase0_m[index] = static_cast<float>(sample.sea_ice_thickness_m);
                }
            }
            std::array<double, 8U> seasonal_temperature{};
            for (size_t phase = 0U; phase < phases.size(); ++phase) {
                seasonal_temperature[phase] = phases[phase].temperature_celsius;
            }
            auto const& footprint = phases.front();
            output_elevation_m[index] = footprint.elevation_m;
            output_ocean_cells[index] = static_cast<uint8_t>(footprint.ocean);
            v10_water_cells[index] = static_cast<uint8_t>(
                footprint.ocean || footprint.lake || (!use_river_geometry && footprint.river));
            v10_water_level_m[index] = static_cast<float>(footprint.water_level_m);
            image.v10_projected_inland_ice_kind[index] = s2ProjectedInlandIceKind(
                seasonal_temperature, footprint.ocean, footprint.lake, footprint.river);
            auto annual = phases.front();
            annual.temperature_celsius = temperature_sum / 8.0;
            annual.precipitation_mm_year = precipitation_sum / 8.0;
            annual.snow_water_equivalent_m = snow_sum / 8.0;
            annual.glacier_thickness_m = glacier_sum / 8.0;
            annual.sea_ice_thickness_m = sea_ice_sum / 8.0;
            v10_cover = generation::classifyV10AnnualMeanSurfaceCover(annual);
            output_temperature[index] = static_cast<float>(temperature_sum / 8.0);
            output_precipitation[index] = static_cast<float>(precipitation_sum / 8.0);
        }
        bool const is_water = v10 ? v10_water_cells[index] != 0U
            : v9ValueAt(*ocean, index) != 0.0 || v9ValueAt(*lake, index) != 0.0 || v9ValueAt(*river, index) != 0.0;
        bool snow = false;
        bool ice = false;
        bool sea = false;
        if (v10) {
            snow = v10_cover == generation::V10SurfaceCover::Snow;
            ice = v10_cover == generation::V10SurfaceCover::GlacierIce;
            sea = v10_cover == generation::V10SurfaceCover::SeaIce;
            output_ice_kind[index] = sea ? 3U : (ice ? 2U : (snow ? 1U : 0U));
        } else if (v21) {
            constexpr float DISPLAY_ICE_CUTOFF_M = 0.1F;
            snow = !is_water && v9ValueAt(*land_snow, index) >= DISPLAY_ICE_CUTOFF_M;
            ice = !is_water && v9ValueAt(*glacier, index) >= DISPLAY_ICE_CUTOFF_M;
            sea = v9ValueAt(*ocean, index) != 0.0
                && v9ValueAt(*sea_ice, index) >= DISPLAY_ICE_CUTOFF_M;
            output_ice_kind[index] = sea ? 3U : (snow ? 1U : (ice ? 2U : 0U));
        } else {
            snow = !is_water && v9PhaseMean(*land_snow, SOURCE_COUNT, index) >= (v10 ? 0.1 : 1.0e-6);
            ice = !is_water && v9PhaseMean(*glacier, SOURCE_COUNT, index) >= (v10 ? 0.1 : 1.0e-6);
            sea = v9ValueAt(*ocean, index) != 0.0
                && v9PhaseMean(*sea_ice, SOURCE_COUNT, index) >= (v10 ? 0.1 : 1.0e-6);
            output_ice_kind[index] = sea ? 3U : (ice ? 2U : (snow ? 1U : 0U));
        }
        image.ice_snow_count += snow;
        image.ice_glacier_count += ice;
        image.ice_water_count += sea;
    }

    bool const fields_finite = std::all_of(output_elevation_m.begin(), output_elevation_m.end(),
            [](double value) { return std::isfinite(value); })
        && std::all_of(output_temperature.begin(), output_temperature.end(),
            [](float value) { return std::isfinite(value); })
        && std::all_of(output_precipitation.begin(), output_precipitation.end(),
            [](float value) { return std::isfinite(value); })
        && std::all_of(output_moisture.begin(), output_moisture.end(),
            [](float value) { return std::isfinite(value); })
        && std::all_of(output_biome_ids.begin(), output_biome_ids.end(),
            [](uint8_t value) { return value < NATIVE_BIOME_COLORS.size(); });
    throwIfRasterCancelled(cancellation);
    if (!fields_finite) {
        return std::nullopt;
    }

    auto const [height_minimum, height_maximum] = std::minmax_element(
        output_elevation_m.begin(), output_elevation_m.end()
    );
    auto const moisture_range = fieldRange(output_moisture);
    auto const precipitation_range = fieldRange(output_precipitation);
    image.layer_ranges[static_cast<size_t>(S2WorldMapLayer::Plates)] = {0.0F,
        static_cast<float>(*std::max_element(plate_ids.begin(), plate_ids.end()))};
    image.layer_ranges[static_cast<size_t>(S2WorldMapLayer::Heights)] = {
        static_cast<float>(*height_minimum), static_cast<float>(*height_maximum),
    };
    image.layer_ranges[static_cast<size_t>(S2WorldMapLayer::Temperature)] = {-40.0F, 30.0F};
    image.layer_ranges[static_cast<size_t>(S2WorldMapLayer::Biomes)] = {0.0F, 12.0F};
    image.layer_ranges[static_cast<size_t>(S2WorldMapLayer::Moisture)] = {
        moisture_range.minimum, moisture_range.maximum,
    };
    image.layer_ranges[static_cast<size_t>(S2WorldMapLayer::Precipitation)] = {
        precipitation_range.minimum, precipitation_range.maximum,
    };
    for (S2WorldMapLayer const layer : {
        S2WorldMapLayer::Plates, S2WorldMapLayer::Heights, S2WorldMapLayer::Temperature,
        S2WorldMapLayer::Biomes, S2WorldMapLayer::Moisture, S2WorldMapLayer::Precipitation,
    }) {
        image.has_numeric_range[static_cast<size_t>(layer)] = true;
    }
    image.available_layers.fill(true);
    image.has_display_ice_mask = true;
    std::vector<uint32_t> observed_ids(plate_ids.begin(), plate_ids.end());
    std::sort(observed_ids.begin(), observed_ids.end());
    observed_ids.erase(std::unique(observed_ids.begin(), observed_ids.end()), observed_ids.end());
    image.plate_motions.reserve(observed_ids.size());
    bool directions_complete = true;
    for (uint32_t const id : observed_ids) {
        throwIfRasterCancelled(cancellation);
        auto const direction = std::find_if(plate_directions.begin(), plate_directions.end(), [id](auto const& item) {
            return item.plate_id == id;
        });
        if (direction == plate_directions.end() || !std::isfinite(direction->x) || !std::isfinite(direction->y)) {
            directions_complete = false;
            continue;
        }
        constexpr double FULL_TURN = 6.283'185'307'179'586'476'9;
        constexpr double CELL_ANGLE = FULL_TURN / SOURCE_RESOLUTION;
        uint64_t count = 0U;
        double sum_cos_x = 0.0;
        double sum_sin_x = 0.0;
        double sum_cos_y = 0.0;
        double sum_sin_y = 0.0;
        for (size_t index = 0U; index < SOURCE_COUNT; ++index) {
            if ((index & 63U) == 0U) {
                throwIfRasterCancelled(cancellation);
            }
            if (plate_ids[index] == id) {
                ++count;
                double const angle_x = (static_cast<double>(index % SOURCE_RESOLUTION) + 0.5) * CELL_ANGLE;
                double const angle_y = (static_cast<double>(index / SOURCE_RESOLUTION) + 0.5) * CELL_ANGLE;
                sum_cos_x += std::cos(angle_x);
                sum_sin_x += std::sin(angle_x);
                sum_cos_y += std::cos(angle_y);
                sum_sin_y += std::sin(angle_y);
            }
        }
        if (count != 0U) {
            // A periodic centroid can still lie outside a concave or fragmented plate.
            // Anchor the arrow to its nearest actual owner cell on the same torus.
            double const center_x = wrapUnit(std::atan2(sum_sin_x, sum_cos_x) / FULL_TURN) * SOURCE_RESOLUTION;
            double const center_y = wrapUnit(std::atan2(sum_sin_y, sum_cos_y) / FULL_TURN) * SOURCE_RESOLUTION;
            size_t center_index = 0U;
            double nearest_distance = std::numeric_limits<double>::infinity();
            for (size_t index = 0U; index < SOURCE_COUNT; ++index) {
                if ((index & 63U) == 0U) {
                    throwIfRasterCancelled(cancellation);
                }
                if (plate_ids[index] != id) {
                    continue;
                }
                double const dx = std::remainder(
                    static_cast<double>(index % SOURCE_RESOLUTION) + 0.5 - center_x, SOURCE_RESOLUTION);
                double const dy = std::remainder(
                    static_cast<double>(index / SOURCE_RESOLUTION) + 0.5 - center_y, SOURCE_RESOLUTION);
                double const distance = dx * dx + dy * dy;
                if (distance < nearest_distance) {
                    nearest_distance = distance;
                    center_index = index;
                }
            }
            image.plate_motions.push_back({
                .id = id,
                .x = (static_cast<float>(center_index % SOURCE_RESOLUTION) + 0.5F) / SOURCE_RESOLUTION,
                .y = (static_cast<float>(center_index / SOURCE_RESOLUTION) + 0.5F) / SOURCE_RESOLUTION,
                .vector_x = direction->x,
                .vector_y = direction->y,
            });
        }
    }
    image.available_layers[static_cast<size_t>(S2WorldMapLayer::Plates)]
        = directions_complete && image.plate_motions.size() == observed_ids.size();
    if (state.layers[static_cast<size_t>(S2WorldMapLayer::Plates)]
        && image.available_layers[static_cast<size_t>(S2WorldMapLayer::Plates)]) {
        buildPlateBoundarySegments(image, plate_ids, SOURCE_RESOLUTION);
    }

    Range const height_range{
        static_cast<float>(*height_minimum), static_cast<float>(*height_maximum),
    };
    auto const source_index = [](uint32_t x, uint32_t y) {
        return static_cast<size_t>(y % SOURCE_RESOLUTION) * SOURCE_RESOLUTION + x % SOURCE_RESOLUTION;
    };
    auto const root_sample = [&world_lod](uint32_t x, uint32_t y) -> shared::TerrainLodSample const& {
        return world_lod.nodes[static_cast<size_t>(y) * world_lod.side + x].sample;
    };
    bool const scalar_overlay = state.layers[static_cast<size_t>(S2WorldMapLayer::Temperature)]
        || state.layers[static_cast<size_t>(S2WorldMapLayer::Moisture)];
    for (uint32_t y = 0U; y < image.height; ++y) {
        throwIfRasterCancelled(cancellation);
        uint32_t const source_y = y * SOURCE_RESOLUTION / image.height;
        for (uint32_t x = 0U; x < image.width; ++x) {
            uint32_t const source_x = x * SOURCE_RESOLUTION / image.width;
            size_t const index = source_index(source_x, source_y);
            bool const is_water = v10 ? v10_water_cells[index] != 0U
                : v9ValueAt(*ocean, index) != 0.0 || v9ValueAt(*lake, index) != 0.0 || v9ValueAt(*river, index) != 0.0;
            uint8_t const physical_ice_kind = output_ice_kind[index];
            uint8_t const ice_kind = physical_ice_kind == 0U && v10
                && !(use_river_geometry && output_biome_ids[index] == 12U)
                ? image.v10_projected_inland_ice_kind[index] : physical_ice_kind;
            Color color = is_water ? Color{35.0F, 69.0F, 87.0F} : materialColor(root_sample(source_x, source_y).top_material);
            bool const show_biomes = state.layers[static_cast<size_t>(S2WorldMapLayer::Biomes)];
            bool const show_heights = state.layers[static_cast<size_t>(S2WorldMapLayer::Heights)];
            if (show_biomes && !show_heights && !is_water) {
                color = NATIVE_BIOME_COLORS[output_biome_ids[index]];
            }
            if (show_heights) {
                color = s2NativeTerrainColor(
                    static_cast<float>(output_elevation_m[index]), {height_range.minimum, height_range.maximum},
                    {static_cast<float>(output_elevation_m[source_index(source_x + 1U, source_y)]),
                     static_cast<float>(output_elevation_m[source_index(source_x, source_y + 1U)])},
                    is_water,
                    static_cast<float>((v10 ? v10_water_level_m[index] : v21
                        ? v9ValueAt(*water_level, index) / 0.34
                        : v9ValueAt(*water_level, index)) - output_elevation_m[index]),
                    scalar_overlay ? 0U : ice_kind);
                if (show_biomes && ice_kind == 0U && !is_water
                    && !(use_river_geometry && v10 && output_biome_ids[index] == 12U)) {
                    color = s2NativeBiomeColor(color, output_biome_ids[index]);
                }
            }
            if (state.layers[static_cast<size_t>(S2WorldMapLayer::Temperature)]) {
                color = heatColor(S2WorldMapLayer::Temperature, output_temperature[index], {-40.0F, 30.0F});
            }
            uint32_t const climate_x = source_x / 8U;
            uint32_t const climate_y = source_y / 8U;
            size_t const climate_index = static_cast<size_t>(climate_y) * CLIMATE_RESOLUTION + climate_x;
            if (state.layers[static_cast<size_t>(S2WorldMapLayer::Moisture)]) {
                color = heatColor(S2WorldMapLayer::Moisture,
                    output_moisture[climate_index], moisture_range);
            }
            bool const canonical_terrain = state.layers[static_cast<size_t>(S2WorldMapLayer::Heights)] && !scalar_overlay;
            if (is_water && ice_kind != 3U && ice_kind != S2_MAP_PROJECTED_INLAND_ICE && !canonical_terrain) {
                double const water_level_m = v10 ? v10_water_level_m[index] : v21
                    ? v9ValueAt(*water_level, index) / 0.34
                    : v9ValueAt(*water_level, index);
                double const depth_m = std::clamp(water_level_m - output_elevation_m[index], 0.0, 600.0);
                blend(color, {20.0F, 45.0F, 60.0F}, static_cast<float>(std::min(0.40, depth_m * 0.0007)));
            }
            // Scalar legends stay exact; physical ice overlays the selected terrain layers.
            if (!scalar_overlay && !canonical_terrain) {
                applyOpaqueIceMaterial(color, ice_kind);
            }
            setPixel(image, static_cast<int32_t>(x), static_cast<int32_t>(y), color);
        }
    }
    return image;
}

} // namespace

uint8_t s2ProjectedInlandIceKind(
    std::array<double, 8U> const& seasonal_temperature_c, bool const ocean, bool const lake, bool const river
) noexcept
{
    if (ocean || !(lake || river)) return 0U;
    double annual_balance = 0.0;
    for (double const temperature : seasonal_temperature_c) {
        if (!std::isfinite(temperature)) return 0U;
        annual_balance += temperature / static_cast<double>(seasonal_temperature_c.size());
    }
    // Equal-duration phases: negative exposure outweighs positive exposure.
    // This annual map convention is not an ice-thickness or heat-budget model.
    return annual_balance < 0.0 ? S2_MAP_PROJECTED_INLAND_ICE : 0U;
}

void appendS2MapRainTriangles(
    std::vector<GuiVectorVertex>& output, S2MapLayout const& layout,
    S2MapWeatherFields const& fields, float const weather_seconds, float const opacity
)
{
    size_t const count = static_cast<size_t>(fields.precipitation_resolution) * fields.precipitation_resolution;
    if (!std::isfinite(weather_seconds) || !std::isfinite(opacity) || opacity <= 0.0F
        || fields.precipitation_resolution == 0U || fields.precipitation.size() != count
        || !std::isfinite(fields.precipitation_maximum) || fields.precipitation_maximum <= 0.0F) {
        return;
    }
    float total_area = 0.0F;
    for (uint32_t index = 0U; index < std::min<uint32_t>(layout.world_draw_count,
            static_cast<uint32_t>(layout.world_draws.size())); ++index) {
        auto const& draw = layout.world_draws[index];
        if (!validMapImageDraw(draw) || draw.image != 0U) {
            continue;
        }
        float left = draw.destination[0U], top = draw.destination[1U];
        float right = left + draw.destination[2U], bottom = top + draw.destination[3U];
        if (draw.scissor[2U] != 0U && draw.scissor[3U] != 0U) {
            left = std::max(left, static_cast<float>(draw.scissor[0U]));
            top = std::max(top, static_cast<float>(draw.scissor[1U]));
            right = std::min(right, static_cast<float>(draw.scissor[0U] + draw.scissor[2U]));
            bottom = std::min(bottom, static_cast<float>(draw.scissor[1U] + draw.scissor[3U]));
        }
        total_area += std::max(0.0F, right - left) * std::max(0.0F, bottom - top);
    }
    // Wrapped draws partition one viewport; their edges must not reseed the rain.
    float const area_scale = std::max(1.0F, std::sqrt(total_area / 850'000.0F));
    for (uint32_t draw_index = 0U; draw_index < std::min<uint32_t>(layout.world_draw_count,
            static_cast<uint32_t>(layout.world_draws.size())); ++draw_index) {
        auto const& draw = layout.world_draws[draw_index];
        if (!validMapImageDraw(draw) || draw.image != 0U) continue;
        float const pixels_per_world = draw.destination[2U] / draw.source[2U];
        float const scale = std::clamp(std::sqrt(pixels_per_world / 512.0F), .8F, 1.8F);
        ScreenRect viewport{draw.destination[0U], draw.destination[1U],
            draw.destination[0U] + draw.destination[2U], draw.destination[1U] + draw.destination[3U]};
        if (draw.scissor[2U] != 0U && draw.scissor[3U] != 0U) {
            viewport.left = std::max(viewport.left, static_cast<float>(draw.scissor[0U]));
            viewport.top = std::max(viewport.top, static_cast<float>(draw.scissor[1U]));
            viewport.right = std::min(viewport.right, static_cast<float>(draw.scissor[0U] + draw.scissor[2U]));
            viewport.bottom = std::min(viewport.bottom, static_cast<float>(draw.scissor[1U] + draw.scissor[3U]));
        }
        uint32_t const lattice = std::clamp(static_cast<uint32_t>(pixels_per_world / (32.0F * area_scale)), 4U, 256U);
        auto visible_cell = [&](float pixel, size_t axis, bool upper) {
            float const uv = draw.source[axis]
                + (pixel - draw.destination[axis]) / draw.destination[axis + 2U] * draw.source[axis + 2U];
            int const cell = static_cast<int>(std::floor(uv * lattice)) + (upper ? 2 : -1);
            return static_cast<uint32_t>(std::clamp(cell, 0, static_cast<int>(lattice)));
        };
        uint32_t const x_begin = visible_cell(viewport.left, 0U, false);
        uint32_t const x_end = visible_cell(viewport.right, 0U, true);
        uint32_t const y_begin = visible_cell(viewport.top, 1U, false);
        uint32_t const y_end = visible_cell(viewport.bottom, 1U, true);
        auto stroke = [&](ScreenPoint from, ScreenPoint to, PixelColor color) {
            float const margin = 1.25F * scale;
            ScreenRect const safe{viewport.left + margin, viewport.top + margin,
                viewport.right - margin, viewport.bottom - margin};
            if (safe.right <= safe.left || safe.bottom <= safe.top || !clipSegmentToRect(from, to, safe)) return;
            color[3U] = static_cast<uint8_t>(std::lround(color[3U] * std::clamp(opacity, 0.0F, 1.0F)));
            float const dx = to[0U] - from[0U];
            float const dy = to[1U] - from[1U];
            float const length = std::hypot(dx, dy);
            if (length < .0001F || output.size() + 24U > MAX_GUI_TRIANGLE_VERTICES) return;
            ScreenPoint const normal{-dy / length, dx / length};
            PixelColor const edge{15U, 37U, 45U, static_cast<uint8_t>(color[3U] * .55F)};
            float const half_width = 1.2F * scale;
            emitSolidQuad(output, offsetPoint(from, normal, half_width), offsetPoint(to, normal, half_width),
                offsetPoint(to, normal, -half_width), offsetPoint(from, normal, -half_width), edge);
            static_cast<void>(emitStroke(output, from, to, 1.2F * scale, color, .55F * scale));
        };
        for (uint32_t y = y_begin; y < y_end; ++y) {
            for (uint32_t x = x_begin; x < x_end; ++x) {
                uint32_t hash = x * 1'664'525U + y * 1'013'904'223U + 211U;
                hash ^= hash >> 16U;
                hash *= 0x7FEB'352DU;
                hash ^= hash >> 15U;
                ScreenPoint const uv{
                    (x + .2F + static_cast<float>(hash & 0xFFFFU) / 65'535.0F * .6F) / lattice,
                    (y + .2F + static_cast<float>(hash >> 16U) / 65'535.0F * .6F) / lattice,
                };
                float const event_time = weather_seconds / .65F + static_cast<float>(hash % 997U) / 997.0F;
                float const age = static_cast<float>(wrapUnit(event_time)) * .65F;
                if (age >= .38F) {
                    continue;
                }
                ScreenPoint const anchor{
                    draw.destination[0U] + (uv[0U] - draw.source[0U]) / draw.source[2U] * draw.destination[2U],
                    draw.destination[1U] + (uv[1U] - draw.source[1U]) / draw.source[3U] * draw.destination[3U],
                };
                // A splash extends at most 3.5 logical pixels from its anchor.
                float const extent = 5.75F * scale;
                if (anchor[0U] + extent < viewport.left || anchor[0U] - extent > viewport.right
                    || anchor[1U] + extent < viewport.top || anchor[1U] - extent > viewport.bottom) {
                    continue;
                }
                float const amount = sampleFlowField(fields.precipitation, fields.precipitation,
                    fields.precipitation_resolution, uv[0U] * fields.precipitation_resolution,
                    uv[1U] * fields.precipitation_resolution)[0U];
                if (!std::isfinite(amount)) {
                    continue;
                }
                float const intensity = std::sqrt(std::clamp(amount / fields.precipitation_maximum, 0.0F, 1.0F));
                int64_t const cycle = static_cast<int64_t>(std::floor(event_time));
                uint32_t const event_hash = hash ^ (static_cast<uint32_t>(cycle) * 0x9E37'79B9U);
                if (static_cast<float>(event_hash % 1021U) / 1020.0F >= intensity) continue;
                PixelColor color{210U, 230U, 235U, 255U};
                if (age < .12F) {
                    float const height = (1.0F - age / .12F) * 3.0F * scale;
                    stroke({anchor[0U], anchor[1U] - height - 1.5F * scale},
                        {anchor[0U], anchor[1U] - height}, color);
                    continue;
                }
                float const splash = std::clamp((age - .16F) / .22F, 0.0F, 1.0F);
                float const radius = (.7F + splash * 2.8F) * scale;
                color[3U] = static_cast<uint8_t>(std::lround(255.0F * (1.0F - splash)));
                std::array<ScreenPoint, 4U> const ring{{
                    {anchor[0U] - radius, anchor[1U]}, {anchor[0U], anchor[1U] - radius * .45F},
                    {anchor[0U] + radius, anchor[1U]}, {anchor[0U], anchor[1U] + radius * .45F},
                }};
                for (size_t index = 0U; index < ring.size(); ++index) {
                    stroke(ring[index], ring[(index + 1U) % ring.size()], color);
                }
            }
        }
    }
}

void appendS2MapWeatherTriangles(
    std::vector<GuiVectorVertex>& output, S2MapLayout const& layout,
    S2MapWeatherFields const& fields, float const phase, float const requested_dpi,
    bool const wind, bool const currents, bool const rain, float const opacity
)
{
    if (!std::isfinite(phase) || !std::isfinite(opacity) || opacity <= 0.0F) return;
    float const dpi = sanitizedDpi(requested_dpi);
    float total_area = 0.0F;
    for (uint32_t index = 0U; index < std::min<uint32_t>(layout.world_draw_count,
            static_cast<uint32_t>(layout.world_draws.size())); ++index) {
        auto const& draw = layout.world_draws[index];
        if (validMapImageDraw(draw) && draw.image == 0U) {
            total_area += draw.destination[2U] * draw.destination[3U];
        }
    }
    float const area_scale = std::max(1.0F, std::sqrt(total_area / (850'000.0F * dpi * dpi)));
    float const layer_scale = std::sqrt(static_cast<float>(std::max(1,
        static_cast<int>(wind) + static_cast<int>(currents) + static_cast<int>(rain))));

    for (uint32_t draw_index = 0U; draw_index < std::min<uint32_t>(layout.world_draw_count,
            static_cast<uint32_t>(layout.world_draws.size())); ++draw_index) {
        auto const& draw = layout.world_draws[draw_index];
        if (!validMapImageDraw(draw) || draw.image != 0U) continue;
        float const pixels_per_world = draw.destination[2U] / draw.source[2U];
        float const spacing = 32.0F * layer_scale * dpi * area_scale;
        uint32_t const lattice = std::clamp(static_cast<uint32_t>(pixels_per_world / spacing), 4U, 256U);
        ScreenRect viewport{draw.destination[0U], draw.destination[1U],
            draw.destination[0U] + draw.destination[2U], draw.destination[1U] + draw.destination[3U]};
        if (draw.scissor[2U] != 0U && draw.scissor[3U] != 0U) {
            viewport.left = std::max(viewport.left, static_cast<float>(draw.scissor[0U]));
            viewport.top = std::max(viewport.top, static_cast<float>(draw.scissor[1U]));
            viewport.right = std::min(viewport.right, static_cast<float>(draw.scissor[0U] + draw.scissor[2U]));
            viewport.bottom = std::min(viewport.bottom, static_cast<float>(draw.scissor[1U] + draw.scissor[3U]));
        }
        auto screen = [&](ScreenPoint point) -> ScreenPoint {
            return {draw.destination[0U] + (point[0U] - draw.source[0U]) * pixels_per_world,
                draw.destination[1U] + (point[1U] - draw.source[1U])
                    * draw.destination[3U] / draw.source[3U]};
        };
        auto stroke = [&](ScreenPoint from, ScreenPoint to, PixelColor color, float width, bool const soft = false) {
            float const margin = width * .5F + .001F;
            ScreenRect const stroke_viewport{viewport.left + margin, viewport.top + margin,
                viewport.right - margin, viewport.bottom - margin};
            if (stroke_viewport.right <= stroke_viewport.left || stroke_viewport.bottom <= stroke_viewport.top
                || !clipSegmentToRect(from, to, stroke_viewport)) {
                return;
            }
            color[3U] = static_cast<uint8_t>(std::lround(color[3U] * std::clamp(opacity, 0.0F, 1.0F)));
            static_cast<void>(emitStroke(output, from, to, width, color, soft ? width * .5F : .55F));
        };
        int const first_x = static_cast<int>(std::floor(draw.source[0U] * lattice)) - 1;
        int const first_y = static_cast<int>(std::floor(draw.source[1U] * lattice)) - 1;
        int const last_x = static_cast<int>(std::ceil((draw.source[0U] + draw.source[2U]) * lattice)) + 1;
        int const last_y = static_cast<int>(std::ceil((draw.source[1U] + draw.source[3U]) * lattice)) + 1;
        auto field = [&](bool water) {
            auto u = water ? fields.current_u : fields.wind_u;
            auto v = water ? fields.current_v : fields.wind_v;
            size_t const count = static_cast<size_t>(fields.resolution) * fields.resolution;
            if (fields.resolution == 0U || u.size() != count || v.size() != count) return;
            if (water && (fields.ocean_resolution == 0U
                    || fields.ocean.size() != static_cast<size_t>(fields.ocean_resolution) * fields.ocean_resolution)) return;
            auto can_travel = [&](ScreenPoint const from, ScreenPoint const to) {
                return !water || oceanPathSegment(fields.ocean, fields.ocean_resolution, fields.resolution,
                    from[0U] * fields.resolution, from[1U] * fields.resolution,
                    to[0U] * fields.resolution, to[1U] * fields.resolution);
            };
            for (int y = first_y; y <= last_y; ++y) {
                for (int x = first_x; x <= last_x; ++x) {
                    int const signed_lattice = static_cast<int>(lattice);
                    uint32_t const seed_x = static_cast<uint32_t>((x % signed_lattice + signed_lattice) % signed_lattice);
                    uint32_t const seed_y = static_cast<uint32_t>((y % signed_lattice + signed_lattice) % signed_lattice);
                    uint32_t hash = seed_x * 1'664'525U + seed_y * 1'013'904'223U + (water ? 97U : 13U);
                    hash ^= hash >> 16U;
                    hash *= 0x7FEB'352DU;
                    hash ^= hash >> 15U;
                    hash *= 0x846C'A68BU;
                    hash ^= hash >> 16U;
                    float const age = static_cast<float>(wrapUnit(phase * .8F
                        + static_cast<float>(hash % 997U) / 997.0F));
                    // A seed stays in its own stratum; mixed hashes avoid aligned rows
                    // without the clusters and holes produced by unrestricted random seeds.
                    float const jitter_x = (static_cast<float>(hash & 0xFFFFU) / 65'535.0F - .5F) * .7F;
                    float const jitter_y = (static_cast<float>(hash >> 16U) / 65'535.0F - .5F) * .7F;
                    ScreenPoint point{
                        (static_cast<float>(x) + .5F + jitter_x) / static_cast<float>(lattice),
                        (static_cast<float>(y) + .5F + jitter_y) / static_cast<float>(lattice),
                    };
                    auto advance = [&](ScreenPoint from, float distance) {
                        auto const vector = sampleFlowField(u, v, fields.resolution,
                            from[0U] * fields.resolution, from[1U] * fields.resolution);
                        float const magnitude = std::hypot(vector[0U], vector[1U]);
                        if (!std::isfinite(magnitude) || magnitude < 1.0e-5F) return from;
                        return ScreenPoint{from[0U] + vector[0U] / magnitude * distance,
                            from[1U] + vector[1U] / magnitude * distance};
                    };
                    // One lifetime covers 25 logical pixels in 1.25 seconds. Four
                    // bilinearly sampled steps follow turns and stop at the first coast.
                    float const travel_step = age * 25.0F * dpi / (pixels_per_world * 4.0F);
                    bool blocked = false;
                    for (uint32_t step = 0U; step < 4U; ++step) {
                        ScreenPoint const next = advance(point, travel_step);
                        if (!can_travel(point, next)) {
                            blocked = true;
                            break;
                        }
                        point = next;
                    }
                    if (blocked) {
                        continue;
                    }
                    float const fade = std::clamp(std::min(age, 1.0F - age) / .12F, 0.0F, 1.0F);
                    if (!water && hash % 7U == 0U) {
                        ScreenPoint ribbon = point;
                        PixelColor const cloud{222U, 231U, 227U, static_cast<uint8_t>(std::lround(25.0F * fade))};
                        for (uint32_t segment = 0U; segment < 4U; ++segment) {
                            ScreenPoint const next = advance(ribbon, 12.0F * dpi / pixels_per_world);
                            stroke(screen(ribbon), screen(next), cloud, (5.0F + segment * .8F) * dpi, true);
                            ribbon = next;
                        }
                    }
                    for (uint32_t segment = 0U; segment < 3U; ++segment) {
                        ScreenPoint const next = advance(point, 6.0F * dpi / pixels_per_world);
                        if (next == point) break;
                        if (!can_travel(point, next)) break;
                        PixelColor color = water ? PixelColor{162U, 224U, 249U, 255U}
                            : PixelColor{62U, 232U, 216U, 255U};
                        color[3U] = static_cast<uint8_t>(std::lround((segment == 2U ? 255.0F : 70.0F + segment * 48.0F) * fade));
                        stroke(screen(point), screen(next), color, (1.2F + segment * .2F) * dpi);
                        point = next;
                    }
                }
            }
        };
        if (wind) field(false);
        if (currents) field(true);
    }
    if (rain) {
        appendS2MapRainTriangles(output, layout, fields, phase, opacity);
    }
}


std::array<float, 3U> s2NativeTerrainColor(
    float const elevation_m, std::array<float, 2U> const height_range_m, std::array<float, 2U> const neighbors_m,
    bool const is_water, float const water_depth_m, uint8_t const ice_kind
) noexcept
{
    Color color = elevationColor(elevation_m, {height_range_m[0], height_range_m[1]});
    // Above-sea-level lakes and rivers are water, not exposed green terrain.
    if (is_water && elevation_m >= 0.0F) {
        color = {35.0F, 69.0F, 87.0F};
    }
    float const step = niceStep(std::max(0.001F, height_range_m[1] - height_range_m[0]) / 8.0F);
    if (std::floor(elevation_m / step) != std::floor(neighbors_m[0] / step)
        || std::floor(elevation_m / step) != std::floor(neighbors_m[1] / step)) {
        blend(color, is_water ? Color{105.0F, 151.0F, 169.0F} : Color{235.0F, 222.0F, 186.0F},
            is_water ? 0.19F : 0.29F);
    }
    if (is_water && ice_kind != 3U && ice_kind != S2_MAP_PROJECTED_INLAND_ICE) {
        blend(color, {20.0F, 45.0F, 60.0F}, std::min(0.40F, std::clamp(water_depth_m, 0.0F, 600.0F) * 0.0007F));
    }
    applyOpaqueIceMaterial(color, ice_kind);
    return color;
}

std::array<std::array<float, 2U>, 6U> s2FlowArrowSegments(
    std::array<float, 2U> const center, std::array<float, 2U> direction, float const length
) noexcept
{
    float const magnitude = std::hypot(direction[0U], direction[1U]);
    if (!std::isfinite(magnitude) || magnitude <= 0.0F || !std::isfinite(length) || length <= 0.0F) {
        return {center, center, center, center, center, center};
    }
    direction[0U] /= magnitude;
    direction[1U] /= magnitude;
    std::array<float, 2U> const normal{-direction[1U], direction[0U]};
    auto const tail = offsetPoint(center, direction, -length * 0.5F);
    auto const tip = offsetPoint(center, direction, length * 0.5F);
    auto const wing_base = offsetPoint(tip, direction, -length * 0.34F);
    return {tail, tip, tip, offsetPoint(wing_base, normal, -length * 0.20F),
        tip, offsetPoint(wing_base, normal, length * 0.20F)};
}

std::array<float, 3U> s2NativeBiomeColor(
    std::array<float, 3U> color, uint8_t const biome_id, float const coverage
) noexcept
{
    if (biome_id < NATIVE_BIOME_COLORS.size())
    {
        blend(color, NATIVE_BIOME_COLORS[biome_id], 0.40F * std::clamp(coverage, 0.0F, 1.0F));
    }
    return color;
}

std::shared_ptr<S2MapImage const> rasterizeS2RiverMap(
    std::shared_ptr<S2RiverMapGeometry const> const& geometry, std::span<MapImageDraw const> draws,
    uint32_t screen_width, uint32_t screen_height, uint32_t output_width, uint32_t output_height,
    std::stop_token const cancellation)
{
    throwIfRasterCancelled(cancellation);
    if (!geometry || screen_width == 0U || screen_height == 0U || output_width == 0U || output_height == 0U
        || output_width > S2_WORLD_MAP_IMAGE_SIDE || output_height > S2_WORLD_MAP_IMAGE_SIDE)
    {
        return {};
    }
    std::vector<uint32_t> key{screen_width, screen_height, output_width, output_height};
    for (auto const& draw : draws)
    {
        auto const draw_key = riverMapDrawKey(draw);
        key.insert(key.end(), draw_key.begin(), draw_key.end());
    }
    std::unique_lock lock(geometry->mutex, std::defer_lock);
    while (!lock.try_lock_for(std::chrono::milliseconds{10}))
    {
        throwIfRasterCancelled(cancellation);
    }
    throwIfRasterCancelled(cancellation);
    if (geometry->cache && geometry->cache_key == key)
    {
        return geometry->cache;
    }
    struct Pieces
    {
        std::vector<RiverPolygon> contact, standing, river, cold_standing, cold_river;
        std::array<double, 3U> land{};
        double land_area = 0.0;
        std::array<double, 3U> standing_color{};
        double standing_color_area = 0.0;
        double contact_area = 0.0;
        double standing_area = 0.0;
        double cold_standing_area = 0.0;
    };
    std::unordered_map<uint32_t, Pieces> pixels;
    double const sx = static_cast<double>(output_width) / screen_width;
    double const sy = static_cast<double>(output_height) / screen_height;
    auto const raster_copy = [&](MapImageDraw const& draw, S2RiverMapGeometry::Polygon const& primitive,
        int32_t px, int32_t py) {
        double const world_left = (static_cast<double>(draw.source[0]) - px) * 65'536.0;
        double const world_top = (static_cast<double>(draw.source[1]) - py) * 65'536.0;
        double const world_right = world_left + draw.source[2] * 65'536.0;
        double const world_bottom = world_top + draw.source[3] * 65'536.0;
        if (primitive.bounds[2] <= world_left || primitive.bounds[0] >= world_right
            || primitive.bounds[3] <= world_top || primitive.bounds[1] >= world_bottom)
        {
            return;
        }
        RiverPolygon projected;
        for (auto const& p : primitive.xy)
        {
            projected.push_back({(draw.destination[0] + ((p[0] / 65'536.0 + px)
                - draw.source[0]) / draw.source[2] * draw.destination[2]) * sx,
                (draw.destination[1] + ((p[1] / 65'536.0 + py)
                - draw.source[1]) / draw.source[3] * draw.destination[3]) * sy});
        }
        auto const bounds = [&](size_t axis, bool minimum) {
            double value = minimum ? std::numeric_limits<double>::infinity()
                : -std::numeric_limits<double>::infinity();
            for (auto const& p : projected)
            {
                value = minimum ? std::min(value, p[axis]) : std::max(value, p[axis]);
            }
            return value;
        };
        double const left = std::max({0.0, draw.destination[0] * sx,
            draw.scissor[2] ? draw.scissor[0] * sx : 0.0, bounds(0U, true)});
        double const right = std::min({static_cast<double>(output_width),
            (draw.destination[0] + draw.destination[2]) * sx,
            draw.scissor[2] ? (draw.scissor[0] + draw.scissor[2]) * sx : static_cast<double>(output_width),
            bounds(0U, false)});
        double const top = std::max({0.0, draw.destination[1] * sy,
            draw.scissor[3] ? draw.scissor[1] * sy : 0.0, bounds(1U, true)});
        double const bottom = std::min({static_cast<double>(output_height),
            (draw.destination[1] + draw.destination[3]) * sy,
            draw.scissor[3] ? (draw.scissor[1] + draw.scissor[3]) * sy : static_cast<double>(output_height),
            bounds(1U, false)});
        if (left >= right || top >= bottom)
        {
            return;
        }
        for (uint32_t y = static_cast<uint32_t>(std::floor(top)); y < std::ceil(bottom); ++y)
        {
            for (uint32_t x = static_cast<uint32_t>(std::floor(left)); x < std::ceil(right); ++x)
            {
                auto polygon = clipRiverPolygon(clipRiverPolygon(clipRiverPolygon(
                    clipRiverPolygon(projected, 0U, std::max<double>(x, left), true), 0U,
                    std::min<double>(x + 1U, right), false), 1U, std::max<double>(y, top), true),
                    1U, std::min<double>(y + 1U, bottom), false);
                if (polygon.size() < 3U)
                {
                    continue;
                }
                for (auto& p : polygon)
                {
                    p[0] -= x;
                    p[1] -= y;
                }
                double const area = riverPolygonArea(polygon);
                if (!(area > 0.0))
                {
                    continue;
                }
                auto& piece = pixels[y * output_width + x];
                if (!primitive.contact)
                {
                    piece.river.push_back(polygon);
                    if (primitive.cold)
                    {
                        piece.cold_river.push_back(polygon);
                    }
                    continue;
                }
                piece.contact_area += area;
                piece.contact.push_back(polygon);
                if (!primitive.standing)
                {
                    piece.land_area += area;
                    for (size_t c = 0U; c < 3U; ++c)
                    {
                        piece.land[c] += area * primitive.color[c];
                    }
                    continue;
                }
                piece.standing_area += area;
                piece.standing.push_back(polygon);
                if (primitive.cold)
                {
                    piece.cold_standing_area += area;
                    piece.cold_standing.push_back(polygon);
                }
                else
                {
                    piece.standing_color_area += area;
                    for (size_t c = 0U; c < 3U; ++c)
                    {
                        piece.standing_color[c] += area * primitive.color[c];
                    }
                }
            }
        }
    };
    for (auto const& draw : draws)
    {
        throwIfRasterCancelled(cancellation);
        if (!validMapImageDraw(draw) || draw.image != 0U)
        {
            continue;
        }
        for (auto const& primitive : geometry->polygons)
        {
            throwIfRasterCancelled(cancellation);
            int32_t const first_x = static_cast<int32_t>(std::ceil(draw.source[0]
                - primitive.bounds[2] / 65'536.0));
            int32_t const last_x = static_cast<int32_t>(std::floor(draw.source[0] + draw.source[2]
                - primitive.bounds[0] / 65'536.0));
            int32_t const first_y = static_cast<int32_t>(std::ceil(draw.source[1]
                - primitive.bounds[3] / 65'536.0));
            int32_t const last_y = static_cast<int32_t>(std::floor(draw.source[1] + draw.source[3]
                - primitive.bounds[1] / 65'536.0));
            for (int32_t py = first_y; py <= last_y; ++py)
            {
                throwIfRasterCancelled(cancellation);
                for (int32_t px = first_x; px <= last_x; ++px)
                {
                    raster_copy(draw, primitive, px, py);
                }
            }
        }
    }
    auto image = std::make_shared<S2MapImage>();
    image->width = output_width;
    image->height = output_height;
    image->rgba.resize(static_cast<size_t>(output_width) * output_height, {0U, 0U, 0U, 0U});
    image->v10_river_coverage.resize(image->rgba.size());
    // The unique periodic contact cells form a disjoint quadtree partition.
    // A single draw preserves that partition; overlapping draws need the union.
    bool const contact_partition = draws.size() == 1U;
    for (auto const& [index, p] : pixels)
    {
        if ((index & 63U) == 0U)
        {
            throwIfRasterCancelled(cancellation);
        }
        double const standing = contact_partition ? std::min(1.0, p.standing_area)
            : riverUnionArea(p.standing, cancellation);
        double const cold_standing = contact_partition
            ? std::min(standing, p.cold_standing_area) : riverUnionArea(p.cold_standing, cancellation);
        double wet = standing;
        double coverage = contact_partition ? std::min(1.0, p.contact_area)
            : riverUnionArea(p.contact, cancellation);
        if (!p.river.empty())
        {
            auto water = p.standing;
            water.insert(water.end(), p.river.begin(), p.river.end());
            wet = riverUnionArea(water, cancellation);
            if (coverage < 1.0)
            {
                auto combined = p.contact;
                combined.insert(combined.end(), p.river.begin(), p.river.end());
                coverage = riverUnionArea(combined, cancellation);
            }
        }
        double const land = std::max(0.0, coverage - wet);
        double const river = std::max(0.0, wet - standing);
        double cold_river = 0.0;
        if (!p.cold_river.empty())
        {
            auto ice = p.standing;
            ice.insert(ice.end(), p.cold_river.begin(), p.cold_river.end());
            cold_river = std::clamp(riverUnionArea(ice, cancellation) - standing, 0.0, river);
        }
        image->v10_river_coverage[index] = static_cast<float>(river);
        if (!(coverage > 0.0))
        {
            continue;
        }
        for (size_t c = 0U; c < 3U; ++c)
        {
            double const land_color = p.land_area > 0.0 ? p.land[c] / p.land_area : 0.0;
            double const standing_color = p.standing_color_area > 0.0
                ? p.standing_color[c] / p.standing_color_area : 0.0;
            constexpr std::array<double, 3U> WATER{35.0, 69.0, 87.0};
            constexpr std::array<double, 3U> ICE{230.0, 239.0, 242.0};
            image->rgba[index][c] = static_cast<uint8_t>(std::clamp(std::lround((land_color * land
                + standing_color * std::max(0.0, standing - cold_standing)
                + WATER[c] * (river - cold_river) + ICE[c] * (cold_river + cold_standing)) / coverage), 0L, 255L));
        }
        image->rgba[index][3] = static_cast<uint8_t>(std::clamp(std::lround(coverage * 255.0), 0L, 255L));
    }
    throwIfRasterCancelled(cancellation);
    geometry->cache_key = std::move(key);
    geometry->cache = image;
    return image;
}

S2WorldMapRiverWorker::~S2WorldMapRiverWorker()
{
    invalidate();
    if (m_job.valid()) {
        m_job.wait();
    }
}

std::shared_ptr<S2MapImage const> S2WorldMapRiverWorker::update(
    std::shared_ptr<S2RiverMapGeometry const> geometry,
    S2MapLayout const& layout,
    uint32_t const width,
    uint32_t const height
)
{
    auto const draws = worldMapDraws(layout);
    if (!geometry || width == 0U || height == 0U || draws.empty()) {
        invalidate();
        return {};
    }

    bool const same_request = m_request && m_request->geometry == geometry
        && m_request->width == width && m_request->height == height
        && std::ranges::equal(worldMapDraws(m_request->layout), draws, {}, riverMapDrawKey, riverMapDrawKey);
    if (!same_request) {
        invalidate();
    }
    if (m_job.valid()) {
        if (m_job.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
            return {};
        }
        try {
            auto result = m_job.get();
            if (same_request) {
                m_result = std::move(result);
            }
        } catch (...) {
            if (!m_cancellation.stop_requested()) {
                CORE_ERROR("World map river raster failed");
            }
        }
    }
    if (!same_request) {
        m_request = Request{std::move(geometry), layout, width, height};
        try {
            m_cancellation = std::stop_source{};
            m_job = std::async(std::launch::async, [request = *m_request, token = m_cancellation.get_token()] {
                auto const [output_width, output_height] = worldMapOutputSize(request.width, request.height);
                return rasterizeS2RiverMap(request.geometry, worldMapDraws(request.layout),
                    request.width, request.height, output_width, output_height, token);
            });
        } catch (...) {
            CORE_ERROR("Could not start world map river raster");
        }
    }
    return m_result;
}

void S2WorldMapRiverWorker::invalidate() noexcept
{
    m_cancellation.request_stop();
    m_request.reset();
    m_result.reset();
}

std::optional<S2MapImage> rasterizeS2WorldMap(
    shared::TerrainWorld const& world,
    shared::TerrainLodArtifact const& world_lod,
    S2WorldMapState const& state,
    std::stop_token const cancellation
)
{
    throwIfRasterCancelled(cancellation);
    if (!validL0Source(world, world_lod)) {
        return std::nullopt;
    }
    auto const atlas = world.atlas();
    if (atlas->configuration.model_tag == generation::TerrainModelTag::V9
        || atlas->configuration.model_tag == generation::TerrainModelTag::V21
        || atlas->configuration.model_tag == generation::TerrainModelTag::V10) {
        bool const geometry_enabled = atlas->configuration.model_tag == generation::TerrainModelTag::V10
            && state.layers[static_cast<size_t>(S2WorldMapLayer::Heights)]
            && state.layers[static_cast<size_t>(S2WorldMapLayer::Biomes)]
            && !state.layers[static_cast<size_t>(S2WorldMapLayer::Temperature)]
            && !state.layers[static_cast<size_t>(S2WorldMapLayer::Moisture)]
            && !state.layers[static_cast<size_t>(S2WorldMapLayer::Plates)];
        auto image = rasterizeS2NativeWorldMap(world, world_lod, state, geometry_enabled, cancellation);
        if (image && geometry_enabled)
        {
            image->v10_river_geometry = prepareRiverMapGeometry(world, *image,
                image->layer_ranges[static_cast<size_t>(S2WorldMapLayer::Heights)], cancellation);
            image->v10_river_base_rgba = image->rgba;
            MapImageDraw draw;
            draw.destination = {0.0F, 0.0F, static_cast<float>(image->width), static_cast<float>(image->height)};
            auto layer = rasterizeS2RiverMap(image->v10_river_geometry, std::span{&draw, 1U},
                image->width, image->height, image->width, image->height, cancellation);
            for (size_t i = 0U; layer && i < image->rgba.size(); ++i)
            {
                if ((i & 63U) == 0U)
                {
                    throwIfRasterCancelled(cancellation);
                }
                float const alpha = layer->rgba[i][3] / 255.0F;
                for (size_t c = 0U; c < 3U; ++c)
                {
                    image->rgba[i][c] = static_cast<uint8_t>(std::lround(std::lerp(
                        static_cast<float>(image->rgba[i][c]), static_cast<float>(layer->rgba[i][c]), alpha)));
                }
            }
        }
        return image;
    }
    uint32_t const resolution = atlas->configuration.resolution;
    size_t const count = static_cast<size_t>(resolution) * resolution;
    S2MapImage image{
        .width = S2_WORLD_MAP_IMAGE_SIDE,
        .height = S2_WORLD_MAP_IMAGE_SIDE,
        .source_identity = world.identity(),
        .source_model_identity = world_lod.model_identity,
        .source_resolution = resolution,
    };
    image.rgba.resize(WORLD_PIXEL_COUNT);
    throwIfRasterCancelled(cancellation);

    bool const elevation_valid = hasFiniteField(atlas->elevation, count);
    bool const temperature_valid = hasFiniteField(atlas->temperature, count);
    bool const precipitation_valid = hasFiniteField(atlas->precipitation, count);
    bool const moisture_valid = hasFiniteField(atlas->moisture, count);
    bool const climate_zone_valid = atlas->climate_zone.size() == count
        && std::all_of(atlas->climate_zone.begin(), atlas->climate_zone.end(), [](uint8_t zone) { return zone < 6U; });
    bool const plate_ids_valid = atlas->plate_id.size() == count
        && std::all_of(atlas->plate_id.begin(), atlas->plate_id.end(), [](int16_t id) { return id >= 0; });
    bool const plate_vectors_valid = hasFiniteField(atlas->plate_vx, count) && hasFiniteField(atlas->plate_vy, count);
    bool const wind_valid = hasFiniteField(atlas->wind_u, count) && hasFiniteField(atlas->wind_v, count);
    bool const current_valid = hasFiniteField(atlas->current_u, count) && hasFiniteField(atlas->current_v, count);
    bool const water_valid = hasBinaryMask(atlas->ocean_mask, count) && hasBinaryMask(atlas->lake_mask, count)
        && hasBinaryMask(atlas->river_mask, count) && hasFiniteField(atlas->water_level, count);
    auto const display_ice_mask = makeGenerationMapDisplayIceMask(
        std::span<uint8_t const>(atlas->climate_zone),
        std::span<uint8_t const>(atlas->ocean_mask),
        std::span<uint8_t const>(atlas->lake_mask),
        std::span<float const>(atlas->temperature)
    );
    std::span<uint8_t const> display_ice_surface_kind;
    if (display_ice_mask) {
        display_ice_surface_kind = std::span<uint8_t const>(display_ice_mask->surface_kind);
    }
    auto const generation_base = rasterizeGenerationMap(*atlas, display_ice_surface_kind, false);
    throwIfRasterCancelled(cancellation);
    image.has_display_ice_mask = display_ice_mask.has_value();
    if (display_ice_mask) {
        image.ice_snow_count = static_cast<uint32_t>(std::count(
            display_ice_mask->surface_kind.begin(), display_ice_mask->surface_kind.end(), GENERATION_MAP_ICE_SNOW));
        image.ice_water_count = static_cast<uint32_t>(std::count(
            display_ice_mask->surface_kind.begin(), display_ice_mask->surface_kind.end(), GENERATION_MAP_ICE_WATER));
    }
    image.available_layers = {
        plate_ids_valid && plate_vectors_valid,
        elevation_valid,
        temperature_valid,
        climate_zone_valid,
        wind_valid,
        current_valid && water_valid,
        moisture_valid,
        precipitation_valid,
    };
    if (state.layers[static_cast<size_t>(S2WorldMapLayer::Plates)]
        && image.available_layers[static_cast<size_t>(S2WorldMapLayer::Plates)]) {
        buildPlateMotions(image, atlas->plate_id, atlas->plate_vx, atlas->plate_vy, resolution);
        buildPlateBoundarySegments(image, std::span<int16_t const>{atlas->plate_id}, resolution);
    }

    auto const layer = [&image](S2WorldMapLayer value) {
        return image.available_layers[static_cast<size_t>(value)];
    };
    Range const elevation_range = elevation_valid ? fieldRange(atlas->elevation) : Range{};
    Range const temperature_range{-40.0F, 30.0F};
    Range const moisture_range = moisture_valid ? fieldRange(atlas->moisture) : Range{};
    Range const precipitation_range = precipitation_valid ? fieldRange(atlas->precipitation) : Range{};
    if (image.available_layers[static_cast<size_t>(S2WorldMapLayer::Plates)]) {
        Range const plate_range = fieldRange(atlas->plate_id);
        image.layer_ranges[static_cast<size_t>(S2WorldMapLayer::Plates)] = {plate_range.minimum, plate_range.maximum};
        image.has_numeric_range[static_cast<size_t>(S2WorldMapLayer::Plates)] = true;
    }
    if (elevation_valid) {
        image.layer_ranges[static_cast<size_t>(S2WorldMapLayer::Heights)] = {
            elevation_range.minimum, elevation_range.maximum,
        };
        image.has_numeric_range[static_cast<size_t>(S2WorldMapLayer::Heights)] = true;
    }
    if (temperature_valid) {
        image.layer_ranges[static_cast<size_t>(S2WorldMapLayer::Temperature)] = {-40.0F, 30.0F};
        image.has_numeric_range[static_cast<size_t>(S2WorldMapLayer::Temperature)] = true;
    }
    if (climate_zone_valid) {
        image.layer_ranges[static_cast<size_t>(S2WorldMapLayer::Biomes)] = {0.0F, 5.0F};
        image.has_numeric_range[static_cast<size_t>(S2WorldMapLayer::Biomes)] = true;
    }
    if (moisture_valid) {
        image.layer_ranges[static_cast<size_t>(S2WorldMapLayer::Moisture)] = {
            moisture_range.minimum, moisture_range.maximum,
        };
        image.has_numeric_range[static_cast<size_t>(S2WorldMapLayer::Moisture)] = true;
    }
    if (precipitation_valid) {
        image.layer_ranges[static_cast<size_t>(S2WorldMapLayer::Precipitation)] = {
            precipitation_range.minimum, precipitation_range.maximum,
        };
        image.has_numeric_range[static_cast<size_t>(S2WorldMapLayer::Precipitation)] = true;
    }
    float const elevation_extent = std::max(0.001F, elevation_range.maximum - elevation_range.minimum);
    uint32_t heat_count = 0U;
    for (S2WorldMapLayer const value : {
        S2WorldMapLayer::Temperature, S2WorldMapLayer::Moisture,
    }) {
        heat_count += state.layers[static_cast<size_t>(value)] && layer(value);
    }

    auto const atlasIndex = [resolution](uint32_t const x, uint32_t const y) {
        return static_cast<size_t>(y % resolution) * resolution + (x % resolution);
    };
    auto const rootSample = [&world_lod](uint32_t const x, uint32_t const y) -> shared::TerrainLodSample const& {
        return world_lod.nodes[static_cast<size_t>(y) * world_lod.side + x].sample;
    };
    auto const waterAt = [&](uint32_t const x, uint32_t const y) {
        size_t const index = atlasIndex(x, y);
        return water_valid && (atlas->ocean_mask[index] != 0U || atlas->lake_mask[index] != 0U
            || atlas->river_mask[index] != 0U);
    };

    for (uint32_t y = 0U; y < image.height; ++y) {
        throwIfRasterCancelled(cancellation);
        uint32_t const source_y = y * resolution / image.height;
        for (uint32_t x = 0U; x < image.width; ++x) {
            uint32_t const source_x = x * resolution / image.width;
            size_t const index = atlasIndex(source_x, source_y);
            shared::TerrainLodSample const& sample = rootSample(source_x, source_y);
            bool const water = sample.water_present || waterAt(source_x, source_y);
            uint8_t const ice_kind = display_ice_mask && index < display_ice_mask->surface_kind.size()
                ? display_ice_mask->surface_kind[index]
                : GENERATION_MAP_ICE_NONE;
            bool const ice_water = water && ice_kind == GENERATION_MAP_ICE_WATER;
            bool const snow_land = !water && ice_kind == GENERATION_MAP_ICE_SNOW;
            Color color;
            if (generation_base && index < generation_base->rgba.size()) {
                auto const& pixel = generation_base->rgba[index];
                color = {static_cast<float>(pixel[0]), static_cast<float>(pixel[1]), static_cast<float>(pixel[2])};
            } else {
                color = water
                    ? (ice_water ? Color{203.0F, 227.0F, 231.0F} : Color{35.0F, 69.0F, 87.0F})
                    : materialColor(sample.top_material);
                if (snow_land) {
                    blend(color, {236.0F, 242.0F, 235.0F}, 0.30F);
                }
            }
            if (state.layers[static_cast<size_t>(S2WorldMapLayer::Biomes)] && climate_zone_valid
                && !water && ice_kind == GENERATION_MAP_ICE_NONE) {
                color = CLIMATE_COLORS[atlas->climate_zone[index]];
            }
            if (state.layers[static_cast<size_t>(S2WorldMapLayer::Heights)] && elevation_valid) {
                uint32_t const west = (source_x + resolution - 1U) % resolution;
                uint32_t const north = (source_y + resolution - 1U) % resolution;
                float const slope = (atlas->elevation[atlasIndex(source_x + 1U, source_y)]
                    - atlas->elevation[atlasIndex(west, source_y)])
                    + 0.65F * (atlas->elevation[atlasIndex(source_x, source_y + 1U)]
                        - atlas->elevation[atlasIndex(source_x, north)]);
                float const relief = std::clamp(0.84F
                    + (atlas->elevation[index] - elevation_range.minimum) / elevation_extent * 0.30F
                    - slope * 0.08F, 0.52F, 1.28F);
                for (float& channel : color) {
                    channel *= relief;
                }
                float const contour_step = elevation_extent / 12.0F;
                if (contour_step > 0.0001F) {
                    float const band = std::floor(atlas->elevation[index] / contour_step);
                    if (band != std::floor(atlas->elevation[atlasIndex(source_x + 1U, source_y)] / contour_step)
                        || band != std::floor(atlas->elevation[atlasIndex(source_x, source_y + 1U)] / contour_step)) {
                        blend(color, water ? Color{105.0F, 151.0F, 169.0F} : Color{235.0F, 222.0F, 186.0F},
                            water ? 0.19F : 0.29F);
                    }
                }
            }
            if (water && !ice_water && water_valid) {
                float const depth = std::clamp(atlas->water_level[index] - atlas->elevation[index], 0.0F, 4.0F);
                blend(color, {20.0F, 45.0F, 60.0F}, std::min(0.40F, depth * 0.07F));
            }

            if (heat_count > 0U) {
                Color tint{};
                auto const addTint = [&tint](Color const value) {
                    for (size_t channel = 0U; channel < tint.size(); ++channel) {
                        tint[channel] += value[channel];
                    }
                };
                if (state.layers[static_cast<size_t>(S2WorldMapLayer::Temperature)] && temperature_valid) {
                    addTint(heatColor(S2WorldMapLayer::Temperature, atlas->temperature[index], temperature_range));
                }
                if (state.layers[static_cast<size_t>(S2WorldMapLayer::Moisture)] && moisture_valid) {
                    addTint(heatColor(S2WorldMapLayer::Moisture, atlas->moisture[index], moisture_range));
                }
                for (float& channel : tint) {
                    channel /= static_cast<float>(heat_count);
                }
                color = tint;
            }
            setPixel(image, static_cast<int32_t>(x), static_cast<int32_t>(y), color);
        }
    }

    return image;
}

std::shared_ptr<shared::TerrainLodArtifact const> buildS2MinimapPreview(
    shared::TerrainWorld const& world, shared::TerrainLodArtifact const& root,
    double const player_x, double const player_y)
{
    if (!validL0Source(world, root) || !std::isfinite(player_x) || !std::isfinite(player_y)) {
        return {};
    }
    auto result = std::make_shared<shared::TerrainLodArtifact>();
    result->terrain_identity = root.terrain_identity;
    result->model_identity = root.model_identity;
    result->level = shared::TerrainLodLevel::L0;
    result->spacing = root.spacing;
    result->origin_x = static_cast<uint32_t>(wrapWorld(player_x - S2_MINIMAP_WORLD_EXTENT / 2.0))
        / root.spacing * root.spacing;
    result->origin_y = static_cast<uint32_t>(wrapWorld(player_y - S2_MINIMAP_WORLD_EXTENT / 2.0))
        / root.spacing * root.spacing;
    result->extent = S2_MINIMAP_WORLD_EXTENT;
    result->side = result->extent / result->spacing + 1U;
    result->nodes.reserve(static_cast<size_t>(result->side) * result->side);
    for (uint32_t y = 0U; y < result->side; ++y) {
        for (uint32_t x = 0U; x < result->side; ++x) {
            uint32_t const column = (result->origin_x / root.spacing + x) % (root.side - 1U);
            uint32_t const row = (result->origin_y / root.spacing + y) % (root.side - 1U);
            result->nodes.push_back(root.nodes[static_cast<size_t>(row) * root.side + column]);
        }
    }
    return result;
}

std::shared_ptr<shared::TerrainLodArtifact const> buildS2MinimapLod(
    shared::TerrainWorld const& world,
    double const player_x,
    double const player_y,
    std::stop_token const& cancellation
) try {
    auto const atlas = world.atlas();
    if (!atlas || !shared::isValidTerrainIdentity(world.identity())
        || !std::isfinite(player_x) || !std::isfinite(player_y)
        || (atlas->configuration.resolution != 256U && atlas->configuration.resolution != 512U)) {
        return {};
    }
    uint32_t const spacing = shared::terrainLodSpacing(
        atlas->configuration.resolution, shared::TerrainLodLevel::L1, atlas->configuration.model_tag);
    uint32_t const origin_x = static_cast<uint32_t>(wrapWorld(player_x - S2_MINIMAP_WORLD_EXTENT / 2.0))
        / spacing * spacing;
    uint32_t const origin_y = static_cast<uint32_t>(wrapWorld(player_y - S2_MINIMAP_WORLD_EXTENT / 2.0))
        / spacing * spacing;
    uint32_t x_segment_count = 0U;
    uint32_t y_segment_count = 0U;
    auto const x_segments = splitWrappedAxis(origin_x, S2_MINIMAP_WORLD_EXTENT, x_segment_count);
    auto const y_segments = splitWrappedAxis(origin_y, S2_MINIMAP_WORLD_EXTENT, y_segment_count);
    auto result = std::make_shared<shared::TerrainLodArtifact>();
    result->terrain_identity = world.identity();
    result->model_identity = shared::terrainLodModel(atlas->configuration.model_tag);
    result->level = shared::TerrainLodLevel::L1;
    result->spacing = spacing;
    result->origin_x = origin_x;
    result->origin_y = origin_y;
    result->extent = S2_MINIMAP_WORLD_EXTENT;
    result->side = S2_MINIMAP_WORLD_EXTENT / spacing + 1U;
    result->nodes.resize(static_cast<size_t>(result->side) * result->side);

    for (uint32_t y_segment_index = 0U; y_segment_index < y_segment_count; ++y_segment_index) {
        WrappedAxisSegment const& y_segment = y_segments[y_segment_index];
        for (uint32_t x_segment_index = 0U; x_segment_index < x_segment_count; ++x_segment_index) {
            WrappedAxisSegment const& x_segment = x_segments[x_segment_index];
            uint32_t const patch_extent = std::max(x_segment.extent, y_segment.extent);
            uint32_t patch_origin_x = x_segment.world_start;
            uint32_t patch_origin_y = y_segment.world_start;
            if (patch_origin_x + patch_extent > WORLD_BLOCK_PERIOD) {
                patch_origin_x = WORLD_BLOCK_PERIOD - patch_extent;
            }
            if (patch_origin_y + patch_extent > WORLD_BLOCK_PERIOD) {
                patch_origin_y = WORLD_BLOCK_PERIOD - patch_extent;
            }
            auto const patch = shared::buildTerrainLod(
                world,
                shared::TerrainLodLevel::L1,
                patch_origin_x,
                patch_origin_y,
                patch_extent,
                cancellation
            );
            if (!patch) {
                return {};
            }
            uint32_t const source_x = (x_segment.world_start - patch_origin_x) / spacing;
            uint32_t const source_y = (y_segment.world_start - patch_origin_y) / spacing;
            uint32_t const destination_x = x_segment.local_start / spacing;
            uint32_t const destination_y = y_segment.local_start / spacing;
            uint32_t const columns = x_segment.extent / spacing + 1U;
            uint32_t const rows = y_segment.extent / spacing + 1U;
            for (uint32_t row = 0U; row < rows; ++row) {
                for (uint32_t column = 0U; column < columns; ++column) {
                    uint32_t const local_x = destination_x + column;
                    uint32_t const local_y = destination_y + row;
                    shared::TerrainLodNode node = patch->nodes[
                        static_cast<size_t>(source_y + row) * patch->side + source_x + column];
                    node.key.x = (origin_x + local_x * spacing) % WORLD_BLOCK_PERIOD;
                    node.key.y = (origin_y + local_y * spacing) % WORLD_BLOCK_PERIOD;
                    result->nodes[static_cast<size_t>(local_y) * result->side + local_x] = std::move(node);
                }
            }
        }
    }
    return result;
} catch (generation::GenerationCancelled const&) {
    return {};
}

std::optional<S2MapImage> rasterizeS2Minimap(shared::TerrainLodArtifact const& local_lod)
{
    if (!validLocalLod(local_lod)) {
        return std::nullopt;
    }
    S2MapImage image{
        .width = S2_MINIMAP_IMAGE_SIDE,
        .height = S2_MINIMAP_IMAGE_SIDE,
        .source_identity = local_lod.terrain_identity,
        .source_model_identity = local_lod.model_identity,
    };
    image.rgba.resize(MINIMAP_PIXEL_COUNT);
    for (uint32_t y = 0U; y < image.height; ++y) {
        uint32_t const row_from_south = std::min(
            y * local_lod.extent / image.height / local_lod.spacing, local_lod.side - 2U);
        uint32_t const row = local_lod.side - 2U - row_from_south;
        for (uint32_t x = 0U; x < image.width; ++x) {
            uint32_t const column = std::min(
                x * local_lod.extent / image.width / local_lod.spacing, local_lod.side - 2U);
            size_t const index = static_cast<size_t>(row) * local_lod.side + column;
            Color color = minimapMaterialColor(local_lod.nodes[index].sample);
            if (!local_lod.nodes[index].sample.water_present) {
                uint32_t const west = column == 0U ? column : column - 1U;
                uint32_t const east = std::min(column + 1U, local_lod.side - 1U);
                uint32_t const north = std::min(row + 1U, local_lod.side - 1U);
                uint32_t const south = row == 0U ? row : row - 1U;
                double const west_height = local_lod.nodes[static_cast<size_t>(row) * local_lod.side + west].sample.height;
                double const east_height = local_lod.nodes[static_cast<size_t>(row) * local_lod.side + east].sample.height;
                double const north_height = local_lod.nodes[static_cast<size_t>(north) * local_lod.side + column].sample.height;
                double const south_height = local_lod.nodes[static_cast<size_t>(south) * local_lod.side + column].sample.height;
                double const relief = 0.92 + (west_height - east_height + 0.65 * (north_height - south_height)) * 0.0015;
                float const shade = static_cast<float>(std::clamp(relief, 0.75, 1.10));
                for (float& channel : color) {
                    channel *= shade;
                }
            }
            setPixel(image, static_cast<int32_t>(x), static_cast<int32_t>(y), color);
        }
    }
    for (uint32_t coordinate = 0U; coordinate < image.width; ++coordinate) {
        setPixel(image, static_cast<int32_t>(coordinate), 0, {20.0F, 36.0F, 44.0F});
        setPixel(image, static_cast<int32_t>(coordinate), static_cast<int32_t>(image.height - 1U), {20.0F, 36.0F, 44.0F});
        setPixel(image, 0, static_cast<int32_t>(coordinate), {20.0F, 36.0F, 44.0F});
        setPixel(image, static_cast<int32_t>(image.width - 1U), static_cast<int32_t>(coordinate), {20.0F, 36.0F, 44.0F});
    }
    return image;
}

double minimapHeadingDegreesFromCameraForward(double const forward_x, double const forward_y) noexcept
{
    double heading = std::atan2(forward_x, forward_y) * (180.0 / 3.141'592'653'589'793'238'46);
    if (heading < 0.0) {
        heading += 360.0;
    }
    return heading;
}

S2MinimapMarker layoutS2MinimapMarker(
    shared::TerrainLodArtifact const& local_lod,
    double const player_x,
    double const player_y,
    double const heading_degrees,
    std::array<float, 4U> const& minimap_bounds
) noexcept
{
    if (!validLocalLod(local_lod) || !std::isfinite(player_x) || !std::isfinite(player_y)
        || !std::isfinite(heading_degrees) || minimap_bounds[2] <= 0.0F || minimap_bounds[3] <= 0.0F) {
        return {};
    }
    double heading = std::fmod(heading_degrees, 360.0);
    if (heading < 0.0) {
        heading += 360.0;
    }
    return {
        .x = minimap_bounds[0] + minimap_bounds[2] * 0.5F,
        .y = minimap_bounds[1] + minimap_bounds[3] * 0.5F,
        .heading_degrees = heading,
        .visible = true,
    };
}

void appendS2MinimapPointerTriangles(
    S2MinimapMarker const& marker,
    std::array<float, 4U> const& minimap_bounds,
    float const requested_dpi_scale,
    std::vector<GuiVectorVertex>& vector_triangles
)
{
    if (!marker.visible || minimap_bounds[2U] <= 0.0F || minimap_bounds[3U] <= 0.0F) {
        return;
    }
    float const dpi_scale = sanitizedDpi(requested_dpi_scale);
    float const logical_size = minimap_bounds[2U] / dpi_scale;
    float const reach = (logical_size < 160.0F ? 9.0F : 11.0F) * dpi_scale;
    ScreenRect const bounds{
        minimap_bounds[0U],
        minimap_bounds[1U],
        minimap_bounds[0U] + minimap_bounds[2U],
        minimap_bounds[1U] + minimap_bounds[3U],
    };
    static_cast<void>(appendPointerTriangles(
        vector_triangles, {marker.x, marker.y}, marker.heading_degrees, reach, bounds));
}

float minimumS2WorldMapZoom(uint32_t const width, uint32_t const height) noexcept
{
    if (width == 0U || height == 0U) {
        return 1.0F;
    }
    return static_cast<float>(std::min(width, height)) / static_cast<float>(std::max(width, height));
}

S2MapLayout layoutS2Map(
    S2WorldMapState const& state,
    uint32_t const width,
    uint32_t const height,
    bool const map_open,
    bool const show_minimap,
    float const requested_dpi_scale
) noexcept
{
    S2MapLayout result;
    if (width == 0U || height == 0U) {
        return result;
    }
    if (map_open) {
        float const minimum_zoom = minimumS2WorldMapZoom(width, height);
        float const zoom = state.overview
            ? minimum_zoom
            : std::clamp(static_cast<float>(std::isfinite(state.zoom) ? state.zoom : 1.0), minimum_zoom, 6.0F);
        float const side = static_cast<float>(std::max(width, height)) * zoom;
        float const source_width = static_cast<float>(width) / side;
        float const source_height = static_cast<float>(height) / side;
        auto const x_segments = splitAxis(
            static_cast<float>(state.center_x / WORLD_SIZE - source_width * 0.5), source_width);
        auto const y_segments = splitAxis(
            static_cast<float>(state.center_y / WORLD_SIZE - source_height * 0.5), source_height);
        for (AxisSegment const& y_segment : y_segments) {
            for (AxisSegment const& x_segment : x_segments) {
                if (result.world_draw_count >= result.world_draws.size()) {
                    break;
                }
                result.world_draws[result.world_draw_count++] = {
                    .image = 0U,
                    .source = {x_segment.source_start, y_segment.source_start,
                        x_segment.source_extent, y_segment.source_extent},
                    .destination = {
                        x_segment.destination_fraction * static_cast<float>(width),
                        y_segment.destination_fraction * static_cast<float>(height),
                        x_segment.destination_extent * static_cast<float>(width),
                        y_segment.destination_extent * static_cast<float>(height),
                    },
                    .scissor = {0U, 0U, width, height},
                };
            }
        }
        return result;
    }
    if (show_minimap) {
        float const dpi_scale = sanitizedDpi(requested_dpi_scale);
        float const constrained_size = std::min({192.0F * dpi_scale, static_cast<float>(height) * 0.35F,
            static_cast<float>(width) * 0.19F});
        float const size = std::max(128.0F * dpi_scale,
            std::floor(constrained_size / (16.0F * dpi_scale)) * 16.0F * dpi_scale);
        float const x = std::floor(static_cast<float>(width) - size - 34.0F * dpi_scale);
        float const y = 44.0F * dpi_scale;
        result.minimap_draw = {
            .image = 1U,
            .source = {0.0F, 0.0F, 1.0F, 1.0F},
            .destination = {x, y, size, size},
        };
        result.minimap_bounds = {x, y, size, size};
        result.show_minimap = true;
    }
    return result;
}

S2MapOverlayLayout buildS2WorldMapOverlay(
    S2WorldMapState const& state,
    S2MapImage const* const source_image,
    shared::TerrainWorld const* const world,
    double const player_x,
    double const player_y,
    uint32_t const width,
    uint32_t const height,
    float const requested_dpi_scale,
    TextRenderer& output,
    std::vector<GuiVectorVertex>& vector_triangles,
    float const flow_phase,
    bool const touch_controls
)
{
    output.clear();
    vector_triangles.clear();
    S2MapOverlayLayout layout;
    if (width == 0U || height == 0U) {
        return layout;
    }
    float const dpi_scale = sanitizedDpi(requested_dpi_scale);
    bool const source_matches = source_image
        && (!world || source_image->source_identity == world->identity());
    S2MapImage const* const image = source_matches ? source_image : nullptr;
    auto const atlas = image && world ? world->atlas() : nullptr;
    bool const sample_source_matches = atlas && image
        && image->source_identity == world->identity()
        && (atlas->configuration.resolution == 256U || atlas->configuration.resolution == 512U);
    std::optional<size_t> const sample_index = sample_source_matches
        ? mapSampleIndex(world, image, player_x, player_y)
        : std::nullopt;
    uint32_t heat_count = 0U;
    for (S2WorldMapLayer const layer : {
        S2WorldMapLayer::Temperature, S2WorldMapLayer::Moisture,
    }) {
        size_t const index = static_cast<size_t>(layer);
        heat_count += image && image->available_layers[index] && image->has_numeric_range[index]
            && state.layers[index];
    }
    auto add_panel = [&](float const x, float const y, float const panel_width, float const panel_height,
                         uint8_t const palette_index) {
        if (layout.panel_count >= layout.panels.size() || panel_width <= 0.0F || panel_height <= 0.0F) {
            return;
        }
        layout.panels[layout.panel_count++] = {
            .bounds = {x * dpi_scale, y * dpi_scale, panel_width * dpi_scale, panel_height * dpi_scale},
            .palette_index = palette_index,
        };
    };
    auto add_heat_strip = [&](std::array<float, 4U> const bounds, S2WorldMapLayer const layer,
                              std::array<float, 2U> const range) {
        if (layout.heat_strip_count < layout.heat_strips.size()) {
            layout.heat_strips[layout.heat_strip_count++] = {.bounds = bounds, .layer = layer, .range = range};
        }
    };
    auto add_color_chip = [&](float const x, float const y, Color const color) {
        if (layout.color_chip_count >= layout.color_chips.size()) {
            return;
        }
        layout.color_chips[layout.color_chip_count++] = {
            .bounds = {x * dpi_scale, y * dpi_scale, 8.0F * dpi_scale, 8.0F * dpi_scale},
            .rgba = {
                static_cast<uint8_t>(std::clamp(std::lround(color[0]), 0L, 255L)),
                static_cast<uint8_t>(std::clamp(std::lround(color[1]), 0L, 255L)),
                static_cast<uint8_t>(std::clamp(std::lround(color[2]), 0L, 255L)),
                255U,
            },
        };
    };
    auto outlined = [&](std::string_view const value, Color const color, float const x, float const y,
                        float const text_scale = 1.0F) {
        submitMapText(output, value, color, x, y, dpi_scale, true, 0.5F * text_scale);
    };
    auto plain = [&](std::string_view const value, Color const color, float const x, float const y,
                     float const text_scale = 1.0F) {
        submitMapText(output, value, color, x, y, dpi_scale, false, 0.5F * text_scale);
    };

    float header_height = heat_count > 0U ? 70.0F : 46.0F;
    float const controls_width = static_cast<float>(width) / dpi_scale;
    auto add_action = [&](std::array<float, 4U> bounds, S2WorldMapAction const action) {
        if (layout.action_region_count >= layout.action_regions.size()) return;
        for (float& coordinate : bounds) coordinate *= dpi_scale;
        layout.action_regions[layout.action_region_count++] = {bounds, action};
    };

    if (!image) {
        outlined("Map source unavailable", {213.0F, 201.0F, 172.0F}, 12.0F,
            static_cast<float>(height) / dpi_scale * 0.5F);
    }

    float const minimum_zoom = minimumS2WorldMapZoom(width, height);
    float const zoom = std::clamp(
        static_cast<float>(std::isfinite(state.zoom) ? state.zoom : 1.0), minimum_zoom, 6.0F);
    std::string view = state.overview
        ? "Whole world"
        : fixedText(zoom, 2) + "x / Player view";
    std::string header = "MAP N^ / " + view;
    if (heat_count > 0U) {
        header += " / Opaque field";
        if (heat_count > 1U) {
            header += " average";
        }
    }
    outlined(header, {240.0F, 231.0F, 204.0F}, 12.0F, 8.0F);
    std::array<std::string, 3U> const navigation_labels{
        touch_controls ? "Whole world" : "0 Whole world",
        touch_controls ? "Reset" : "R Reset",
        std::string(touch_controls ? "" : "L ") + (state.legend_expanded ? "Hide details" : "Details"),
    };
    constexpr std::array<S2WorldMapAction, 3U> NAVIGATION_ACTIONS{
        S2WorldMapAction::Overview, S2WorldMapAction::Reset, S2WorldMapAction::ToggleLegend,
    };
    size_t const navigation_characters = navigation_labels[0U].size() + navigation_labels[1U].size()
        + navigation_labels[2U].size() + 4U;
    float navigation_x = touch_controls ? 12.0F : controls_width - static_cast<float>(navigation_characters) * 8.0F - 12.0F;
    float navigation_y = touch_controls ? 28.0F : 6.0F;
    for (size_t index = 0U; index < navigation_labels.size(); ++index) {
        float const label_width = static_cast<float>(navigation_labels[index].size()) * 8.0F;
        float const button_width = touch_controls ? std::max(48.0F, label_width + 16.0F) : label_width;
        if (touch_controls && navigation_x + button_width > controls_width - 12.0F) {
            navigation_x = 12.0F;
            navigation_y += 52.0F;
        }
        outlined(navigation_labels[index], {212.0F, 221.0F, 208.0F},
            navigation_x + (touch_controls ? 8.0F : 0.0F), navigation_y + (touch_controls ? 16.0F : 2.0F));
        add_action({navigation_x, navigation_y, button_width, touch_controls ? 48.0F : 20.0F},
            NAVIGATION_ACTIONS[index]);
        navigation_x += button_width + (touch_controls ? 8.0F : 16.0F);
    }

    static constexpr std::array<std::string_view, S2_WORLD_MAP_LAYER_COUNT> KEYS{
        "P", "H", "T", "B", "Ctrl+W", "C", "O", "Q",
    };
    static constexpr std::array<std::string_view, S2_WORLD_MAP_LAYER_COUNT> SHORT_NAMES{
        "Plate", "Height", "Temp", "Biomes", "Wind", "Curr", "Moist", "Rain",
    };
    static constexpr std::array<std::string_view, S2_WORLD_MAP_LAYER_COUNT> FULL_NAMES{
        "PLATES", "HEIGHTS", "TEMPERATURE", "BIOMES", "WIND", "CURRENTS", "MOISTURE", "PRECIPITATION",
    };
    static constexpr std::array<Color, S2_WORLD_MAP_LAYER_COUNT> LAYER_COLORS{{
        {213.0F, 164.0F, 237.0F}, {217.0F, 206.0F, 170.0F}, {239.0F, 183.0F, 123.0F},
        {168.0F, 200.0F, 147.0F}, {99.0F, 219.0F, 212.0F}, {220.0F, 238.0F, 247.0F},
        {121.0F, 194.0F, 168.0F}, {201.0F, 168.0F, 230.0F},
    }};
    constexpr std::array<S2WorldMapAction, S2_WORLD_MAP_LAYER_COUNT> LAYER_ACTIONS{
        S2WorldMapAction::TogglePlates, S2WorldMapAction::ToggleHeights,
        S2WorldMapAction::ToggleTemperature, S2WorldMapAction::ToggleBiomes,
        S2WorldMapAction::ToggleWind, S2WorldMapAction::ToggleCurrents,
        S2WorldMapAction::ToggleMoisture, S2WorldMapAction::TogglePrecipitation,
    };
    float cursor = 12.0F;
    float layer_y = touch_controls ? navigation_y + 52.0F : 30.0F;
    for (size_t index = 0U; index < S2_WORLD_MAP_LAYER_COUNT; ++index) {
        bool const available = image && image->available_layers[index];
        std::string const status = available ? (state.layers[index] ? "ON" : "OFF") : "N/A";
        std::string const label = (touch_controls ? "" : std::string(KEYS[index]) + " ")
            + std::string(SHORT_NAMES[index]) + " " + status;
        float const label_width = static_cast<float>(label.size()) * 8.0F;
        float const button_width = touch_controls ? std::max(48.0F, label_width + 16.0F) : label_width;
        if (touch_controls && cursor + button_width > controls_width - 12.0F) {
            cursor = 12.0F;
            layer_y += 52.0F;
        }
        outlined(label, available && state.layers[index] ? LAYER_COLORS[index] : Color{147.0F, 167.0F, 170.0F},
            cursor + (touch_controls ? 8.0F : 0.0F), layer_y + (touch_controls ? 16.0F : 2.0F));
        add_action({cursor, layer_y, button_width, touch_controls ? 48.0F : 20.0F}, LAYER_ACTIONS[index]);
        cursor += button_width + (touch_controls ? 8.0F : 12.0F);
    }
    float const heat_y = touch_controls ? layer_y + 56.0F : 56.0F;
    if (touch_controls) header_height = heat_count > 0U ? heat_y + 14.0F : layer_y + 44.0F;
    add_panel(6.0F, 6.0F, controls_width - 12.0F, header_height, 1U);
    add_panel(6.0F, static_cast<float>(height) / dpi_scale - 26.0F,
        controls_width - 12.0F, 20.0F, 1U);

    float const column_width = heat_count > 0U
        ? (static_cast<float>(width) / dpi_scale - 24.0F) / static_cast<float>(heat_count)
        : 0.0F;
    uint32_t heat_column = 0U;
    for (S2WorldMapLayer const layer : {
        S2WorldMapLayer::Temperature, S2WorldMapLayer::Moisture,
    }) {
        size_t const index = static_cast<size_t>(layer);
        if (!image || !image->available_layers[index] || !image->has_numeric_range[index]
            || !state.layers[index]) {
            continue;
        }
        std::string const label = std::string(KEYS[index]) + " " + rangeText(*image, layer)
            + (layer == S2WorldMapLayer::Temperature ? " C" : " model");
        float const x = 12.0F + static_cast<float>(heat_column) * column_width;
        outlined(label, {231.0F, 234.0F, 219.0F},
            x, heat_y);
        float const bar_x = x + static_cast<float>(label.size()) * 8.0F + 8.0F;
        add_heat_strip({bar_x * dpi_scale, (heat_y + 4.0F) * dpi_scale, 48.0F * dpi_scale, 8.0F * dpi_scale},
            layer, image->layer_ranges[index]);
        ++heat_column;
    }

    if (state.legend_expanded) {
        float const x = 10.0F;
        float const y = touch_controls ? header_height + 14.0F : (heat_count > 0U ? 84.0F : 60.0F);
        float const overlay_width = std::min(static_cast<float>(width) / dpi_scale - 20.0F, 1'040.0F);
        float const details_column_width = (overlay_width - 30.0F) * 0.5F;
        add_panel(x, y, overlay_width, 232.0F, 2U);
        std::string source = image && sample_source_matches
            ? " / SEED " + std::to_string(world->identity().seed) + " / "
                + std::to_string(image->source_resolution != 0U
                    ? image->source_resolution : atlas->configuration.resolution)
                + "x" + std::to_string(image->source_resolution != 0U
                    ? image->source_resolution : atlas->configuration.resolution)
            : " / SOURCE UNAVAILABLE";
        plain("LAYER DETAILS / L Close" + source, {231.0F, 226.0F, 204.0F}, x + 10.0F, y + 8.0F);
        bool const ice_available = image && image->has_display_ice_mask;
        bool const v9 = image && image->source_identity.terrain_version
            == generation::terrainVersion(generation::TerrainModelTag::V9);
        bool const v10 = image && image->source_identity.terrain_version
            == generation::terrainVersion(generation::TerrainModelTag::V10);
        bool const v21 = image && image->source_identity.terrain_version
            == generation::terrainVersion(generation::TerrainModelTag::V21);
        bool const native = v9 || v10 || v21;
        std::span<uint32_t const> const native_plate_ids = image
            ? (v10 ? std::span<uint32_t const>{image->v10_plate_ids} : v21 ? std::span<uint32_t const>{image->v21_plate_ids}
                : std::span<uint32_t const>{image->v9_plate_ids})
            : std::span<uint32_t const>{};
        std::span<double const> const native_elevation_m = image
            ? (v10 ? std::span<double const>{image->v10_elevation_m} : v21 ? std::span<double const>{image->v21_elevation_m}
                : std::span<double const>{image->v9_elevation_m})
            : std::span<double const>{};
        std::span<float const> const native_temperature = image
            ? (v10 ? std::span<float const>{image->v10_temperature_mean_c}
                : v21 ? std::span<float const>{image->v21_temperature_mean_c}
                : std::span<float const>{image->v9_temperature_mean_c})
            : std::span<float const>{};
        std::span<uint8_t const> const native_biome_ids = image
            ? (v10 ? std::span<uint8_t const>{image->v10_biome_ids} : v21 ? std::span<uint8_t const>{image->v21_biome_ids}
                : std::span<uint8_t const>{image->v9_biome_ids})
            : std::span<uint8_t const>{};
        std::span<float const> const native_moisture = image
            ? (v10 ? std::span<float const>{image->v10_moisture_mean_kg_m2}
                : v21 ? std::span<float const>{image->v21_moisture_mean_kg_m2}
                : std::span<float const>{image->v9_moisture_mean_kg_m2})
            : std::span<float const>{};
        std::span<float const> const native_precipitation = image
            ? (v10 ? std::span<float const>{image->v10_precipitation_mean_mm_year}
                : v21 ? std::span<float const>{image->v21_precipitation_mean_mm_year}
                : std::span<float const>{image->v9_precipitation_mean_mm_year})
            : std::span<float const>{};
        std::string const ice_rule = ice_available
            ? (v10 ? "V10: inland map ice annual cold balance"
                : native ? "MODEL STOCK: land snow / glacier / sea ice"
                  : "ICE PROXY: Snow land / water <= -10 C")
            : "ICE SOURCE UNAVAILABLE";
        plain(ice_rule, {181.0F, 216.0F, 220.0F},
            x + overlay_width - static_cast<float>(ice_rule.size()) * 8.0F - 10.0F, y + 8.0F);

        auto const sample_float = [&](std::vector<float> const* const values) -> std::optional<float> {
            if (!values || !sample_index || *sample_index >= values->size()
                || !std::isfinite((*values)[*sample_index])) {
                return std::nullopt;
            }
            return (*values)[*sample_index];
        };
        auto const layer_detail = [&](size_t const index) {
            std::array<std::string, 2U> info{"", "Source unavailable; no values supplied"};
            if (!image || !image->available_layers[index]) {
                return info;
            }
            info[1].clear();
            if (index == static_cast<size_t>(S2WorldMapLayer::Plates)) {
                if (native && sample_index && *sample_index < native_plate_ids.size()) {
                    info[0] = "ID " + std::to_string(native_plate_ids[*sample_index]);
                } else if (atlas && sample_index && *sample_index < atlas->plate_id.size()) {
                    info[0] = "ID " + std::to_string(atlas->plate_id[*sample_index]);
                }
                info[1] = native ? "Current uint32 ownership; arrows use actual per-plate unit directions"
                    : "Edges; model world/epoch drift arrows";
            } else if (index == static_cast<size_t>(S2WorldMapLayer::Heights)) {
                if (native && sample_index && *sample_index < native_elevation_m.size()) {
                    info[0] = numberText(static_cast<float>(native_elevation_m[*sample_index])) + " m";
                } else if (auto const value = sample_float(atlas ? &atlas->elevation : nullptr)) {
                    info[0] = numberText(*value) + " km";
                }
                float const step = niceStep((image->layer_ranges[index][1] - image->layer_ranges[index][0]) / 8.0F);
                info[1] = numberText(step) + (native ? " m" : " km") + " contours / "
                    + rangeText(*image, S2WorldMapLayer::Heights) + (native ? " m / sea 0 m" : " km / sea 0");
            } else if (index == static_cast<size_t>(S2WorldMapLayer::Temperature)) {
                if (native && sample_index && *sample_index < native_temperature.size()) {
                    info[0] = numberText(native_temperature[*sample_index]) + " C mean";
                } else if (auto const value = sample_float(atlas ? &atlas->temperature : nullptr)) {
                    info[0] = numberText(*value) + " C";
                }
                info[1] = "Opaque field / " + rangeText(*image, S2WorldMapLayer::Temperature) + " C";
            } else if (index == static_cast<size_t>(S2WorldMapLayer::Biomes)) {
                if (native && sample_index && *sample_index < native_biome_ids.size()) {
                    static constexpr std::array<std::string_view, 13U> NAMES{
                        "Permanent snow/glacier", "Tundra", "Boreal forest", "Temperate grassland",
                        "Temperate forest", "Hot desert", "Savanna", "Seasonal tropical forest",
                        "Tropical rainforest", "Cold desert/polar barren", "Ocean", "Lake", "River reach",
                    };
                    uint8_t const biome_id = native_biome_ids[*sample_index];
                    info[0] = std::to_string(biome_id) + " " + std::string(NAMES[biome_id]);
                } else if (atlas && sample_index && *sample_index < atlas->climate_zone.size()) {
                    static constexpr std::array<std::string_view, 6U> NAMES{
                        "Ocean", "Warm dry", "Warm wet", "Temperate", "Cold", "Snow",
                    };
                    uint8_t const zone = atlas->climate_zone[*sample_index];
                    info[0] = zone < NAMES.size() ? std::string(NAMES[zone]) : "Zone " + std::to_string(zone);
                }
                info[1] = v10 ? "V10 canonical biome_id" : v21 ? "V21 native catalogue biome_id"
                    : (v9 ? "V9 catalogue biome_id" : "Coarse climate-zone proxy");
            } else if (index == static_cast<size_t>(S2WorldMapLayer::Wind)) {
                info[1] = native ? "Mean of eight actual phase wind fields; m/s"
                    : "Cyan single arrows; cells/climate step";
            } else if (index == static_cast<size_t>(S2WorldMapLayer::Currents)) {
                info[1] = native ? "Mean of eight ocean-current fields at ocean cells; m/s"
                    : "White double arrows; cells/climate step";
            } else if (index == static_cast<size_t>(S2WorldMapLayer::Moisture)) {
                if (native && sample_index && image->source_resolution != 0U) {
                    uint32_t const x_index = static_cast<uint32_t>(*sample_index % image->source_resolution);
                    uint32_t const y_index = static_cast<uint32_t>(*sample_index / image->source_resolution);
                    size_t const climate_index = static_cast<size_t>(y_index * image->vector_field_resolution
                        / image->source_resolution) * image->vector_field_resolution
                        + x_index * image->vector_field_resolution / image->source_resolution;
                    if (climate_index < native_moisture.size()) {
                        info[0] = numberText(native_moisture[climate_index]) + " kg/m2 mean";
                    }
                } else if (auto const value = sample_float(atlas ? &atlas->moisture : nullptr)) {
                    info[0] = numberText(*value) + " model";
                }
                info[1] = native ? "Mean soil-water reservoir / kg/m2 / linear color"
                    : "Reservoir " + rangeText(*image, S2WorldMapLayer::Moisture) + " / linear color";
            } else {
                if (native && sample_index && *sample_index < native_precipitation.size()) {
                    info[0] = numberText(native_precipitation[*sample_index]) + " mm/year mean";
                } else if (auto const value = sample_float(atlas ? &atlas->precipitation : nullptr)) {
                    info[0] = numberText(*value) + " model";
                }
                info[1] = native ? "Mean annual precipitation / mm/year / particle density"
                    : "Accum rain " + rangeText(*image, S2WorldMapLayer::Precipitation) + " / particle density";
            }
            return info;
        };

        for (size_t index = 0U; index < S2_WORLD_MAP_LAYER_COUNT; ++index) {
            size_t const column = index / 4U;
            size_t const row = index % 4U;
            float const row_x = x + 10.0F + static_cast<float>(column) * (details_column_width + 10.0F);
            float const row_y = y + 32.0F + static_cast<float>(row) * 38.0F;
            bool const available = image && image->available_layers[index];
            std::string const status = available ? (state.layers[index] ? "ON" : "OFF") : "N/A";
            std::array<std::string, 2U> const info = layer_detail(index);
            std::string const title = std::string(KEYS[index]) + " " + std::string(FULL_NAMES[index]) + " " + status
                + (info[0].empty() ? "" : " / " + info[0]);
            plain(fitMapText(title, details_column_width, dpi_scale),
                available && state.layers[index] ? LAYER_COLORS[index] : Color{187.0F, 197.0F, 190.0F},
                row_x, row_y);
            plain(fitMapText(info[1], details_column_width, dpi_scale), {151.0F, 173.0F, 177.0F}, row_x, row_y + 18.0F);
        }

        std::string note = image && image->has_display_ice_mask
            ? (native ? "Model stocks: land snow " + std::to_string(image->ice_snow_count)
                    + " / glacier " + std::to_string(image->ice_glacier_count)
                    + " / sea ice " + std::to_string(image->ice_water_count) + ". Arrows show direction only."
                  : "Ice proxy: " + std::to_string(image->ice_snow_count) + " land / "
                    + std::to_string(image->ice_water_count) + " water. Climate view, not freezing physics. "
                    + "Arrows: direction only.")
            : "Ice source unavailable. Arrows show direction, not speed.";
        if (v10) {
            note = "Inland ice: annual cold-balance map projection, not thickness. " + note;
        }
        plain(
            fitMapText(std::move(note), overlay_width - 20.0F, dpi_scale),
            {170.0F, 189.0F, 183.0F},
            x + 10.0F,
            y + 186.0F
        );
        if (image && image->available_layers[static_cast<size_t>(S2WorldMapLayer::Biomes)]) {
            plain("B:", {158.0F, 177.0F, 179.0F}, x + 10.0F, y + 208.0F);
            float biome_cursor = x + 38.0F;
            if (native) {
                static constexpr std::array<std::string_view, 13U> NAMES{
                    "Gl", "Tu", "Bo", "Gr", "Fo", "De", "Sa", "ST", "TR", "Po", "Oc", "La", "Ri",
                };
                static constexpr std::array<Color, 13U> BIOME_COLORS{{
                    {222.0F, 231.0F, 224.0F}, {153.0F, 172.0F, 154.0F}, {62.0F, 103.0F, 78.0F},
                    {154.0F, 180.0F, 117.0F}, {67.0F, 119.0F, 75.0F}, {207.0F, 175.0F, 107.0F},
                    {174.0F, 174.0F, 94.0F}, {115.0F, 151.0F, 83.0F}, {43.0F, 116.0F, 63.0F},
                    {169.0F, 164.0F, 146.0F}, {35.0F, 69.0F, 87.0F}, {49.0F, 102.0F, 131.0F},
                    {63.0F, 138.0F, 162.0F},
                }};
                for (uint8_t biome_id = 0U; biome_id < NAMES.size(); ++biome_id) {
                    add_color_chip(biome_cursor, y + 212.0F, BIOME_COLORS[biome_id]);
                    plain(NAMES[biome_id], {185.0F, 198.0F, 187.0F}, biome_cursor + 12.0F, y + 208.0F);
                    biome_cursor += 32.0F;
                }
            } else {
                static constexpr std::array<std::string_view, 6U> NAMES{
                    "Ocean", "Warm dry", "Warm wet", "Temperate", "Cold", "Snow",
                };
                static constexpr std::array<Color, 6U> BIOME_COLORS{{
                    {35.0F, 69.0F, 87.0F}, {181.0F, 153.0F, 99.0F}, {76.0F, 125.0F, 91.0F},
                    {119.0F, 148.0F, 107.0F}, {153.0F, 172.0F, 154.0F}, {222.0F, 231.0F, 224.0F},
                }};
                for (uint8_t zone = 0U; zone < NAMES.size(); ++zone) {
                    add_color_chip(biome_cursor, y + 212.0F, BIOME_COLORS[zone]);
                    plain(NAMES[zone], {185.0F, 198.0F, 187.0F}, biome_cursor + 12.0F, y + 208.0F);
                    biome_cursor += 24.0F + static_cast<float>(NAMES[zone].size()) * 8.0F;
                }
            }
        }
        if (image && image->available_layers[static_cast<size_t>(S2WorldMapLayer::Heights)]) {
            float height_cursor = x + 498.0F;
            plain(native ? "H m:" : "H km:", {158.0F, 177.0F, 179.0F}, height_cursor, y + 208.0F);
            height_cursor += native ? 40.0F : 48.0F;
            Range const height_range{
                image->layer_ranges[static_cast<size_t>(S2WorldMapLayer::Heights)][0],
                image->layer_ranges[static_cast<size_t>(S2WorldMapLayer::Heights)][1],
            };
            for (float const value : {height_range.minimum, 0.0F, height_range.maximum}) {
                std::string const label = numberText(value, 1);
                add_color_chip(height_cursor, y + 212.0F, elevationColor(value, height_range));
                plain(label, elevationColor(value, height_range), height_cursor + 12.0F, y + 208.0F);
                height_cursor += 24.0F + static_cast<float>(label.size()) * 8.0F;
            }
        }
    }

    float const logical_width = static_cast<float>(width) / dpi_scale;
    float const logical_height = static_cast<float>(height) / dpi_scale;
    if (logical_width < 720.0F) {
        outlined("WASD/arrows pan  Drag pan  Shift fast  Wheel zoom",
            {215.0F, 223.0F, 205.0F}, 12.0F, logical_height - 40.0F);
        outlined("+/- zoom  Ctrl+W Wind  M/Esc Close",
            {215.0F, 223.0F, 205.0F}, 12.0F, logical_height - 24.0F);
    } else {
        outlined("WASD/arrows pan  Drag pan  Shift faster  Wheel zoom  +/- zoom  Ctrl+W Wind  M/Esc Close",
            {215.0F, 223.0F, 205.0F}, 12.0F, logical_height - 24.0F);
    }

    S2MapLayout const map_layout = layoutS2Map(state, width, height, true, false, dpi_scale);
    if (image && std::isfinite(player_x) && std::isfinite(player_y)) {
        float const player_u = static_cast<float>(wrapWorld(player_x) / WORLD_SIZE);
        float const player_v = static_cast<float>(wrapWorld(player_y) / WORLD_SIZE);
        for (uint32_t draw_index = 0U; draw_index < map_layout.world_draw_count; ++draw_index) {
            MapImageDraw const& draw = map_layout.world_draws[draw_index];
            if (player_u < draw.source[0] || player_u > draw.source[0] + draw.source[2]
                || player_v < draw.source[1] || player_v > draw.source[1] + draw.source[3]) {
                continue;
            }
            float const player_screen_x = draw.destination[0]
                + (player_u - draw.source[0]) / draw.source[2] * draw.destination[2];
            float const player_screen_y = draw.destination[1]
                + (player_v - draw.source[1]) / draw.source[3] * draw.destination[3];
            float const player_logical_x = player_screen_x / dpi_scale;
            float const player_logical_y = player_screen_y / dpi_scale;
            outlined("+", {255.0F, 228.0F, 154.0F}, player_logical_x - 8.0F, player_logical_y - 16.0F, 2.0F);
            outlined("YOU", {255.0F, 228.0F, 154.0F}, player_logical_x + 12.0F, player_logical_y + 8.0F);
        }
    }
    if (image && atlas && sample_source_matches) {
        appendWorldMapVectors(state, *image, *atlas, map_layout, layout,
            width, height, dpi_scale, output, vector_triangles, flow_phase);
    }
    return layout;
}

namespace {

std::optional<S2MapImage> composeS2WorldMapViewImpl(
    S2MapImage const* const world_image,
    S2MapLayout const& map_layout,
    S2MapOverlayLayout const& overlay_layout,
    uint32_t const width,
    uint32_t const height,
    S2MapImage const* river_layer,
    bool const rasterize_river,
    std::stop_token const cancellation
)
{
    throwIfRasterCancelled(cancellation);
    if (width == 0U || height == 0U || overlay_layout.panel_count > overlay_layout.panels.size()
        || overlay_layout.heat_strip_count > overlay_layout.heat_strips.size()
        || overlay_layout.color_chip_count > overlay_layout.color_chips.size()) {
        return std::nullopt;
    }
    if (world_image && (world_image->width == 0U || world_image->height == 0U
        || world_image->rgba.size() != static_cast<uint64_t>(world_image->width) * world_image->height)) {
        return std::nullopt;
    }
    auto const [output_width, output_height] = worldMapOutputSize(width, height);
    size_t const pixel_count = static_cast<size_t>(output_width) * output_height;
    auto const draws = worldMapDraws(map_layout);
    auto const rasterized_river = rasterize_river && world_image
        ? rasterizeS2RiverMap(world_image->v10_river_geometry, draws, width, height,
            output_width, output_height, cancellation)
        : nullptr;
    if (rasterized_river) {
        river_layer = rasterized_river.get();
    }
    if (river_layer && (!world_image || river_layer->width != output_width || river_layer->height != output_height
        || river_layer->rgba.size() != pixel_count || (!world_image->v10_river_base_rgba.empty()
            && world_image->v10_river_base_rgba.size() != world_image->rgba.size()))) {
        river_layer = nullptr;
    }
    auto const* base_rgba = world_image
        ? &(river_layer && !world_image->v10_river_base_rgba.empty() ? world_image->v10_river_base_rgba
            : world_image->rgba)
        : nullptr;
    S2MapImage view{
        .width = output_width,
        .height = output_height,
        .source_identity = world_image ? world_image->source_identity : shared::TerrainIdentity{},
        .source_model_identity = world_image ? world_image->source_model_identity : shared::TERRAIN_LOD_MODEL,
    };
    view.rgba.resize(pixel_count);
    Color const canvas{
        static_cast<float>(S2_WORLD_MAP_OVERLAY_PALETTE[0U][0U]),
        static_cast<float>(S2_WORLD_MAP_OVERLAY_PALETTE[0U][1U]),
        static_cast<float>(S2_WORLD_MAP_OVERLAY_PALETTE[0U][2U]),
    };
    auto const drawAt = [&](float const screen_x, float const screen_y, Color& sampled_color) {
        if (!world_image) {
            return false;
        }
        for (MapImageDraw const& draw : draws) {
            if (!validMapImageDraw(draw) || draw.image != 0U) {
                continue;
            }
            float const x = draw.destination[0];
            float const y = draw.destination[1];
            float const draw_width = draw.destination[2];
            float const draw_height = draw.destination[3];
            if (screen_x < x || screen_y < y || screen_x >= x + draw_width || screen_y >= y + draw_height) {
                continue;
            }
            if (draw.scissor[2] != 0U
                && (screen_x < draw.scissor[0] || screen_y < draw.scissor[1]
                    || screen_x >= draw.scissor[0] + draw.scissor[2]
                    || screen_y >= draw.scissor[1] + draw.scissor[3])) {
                continue;
            }
            float const u = draw.source[0] + (screen_x - x) / draw_width * draw.source[2];
            float const v = draw.source[1] + (screen_y - y) / draw_height * draw.source[3];
            // Resample at pixel centers rather than snapping each drag position to
            // another source cell. Nearest sampling of this low-resolution composite
            // caused whole contour/biome rows to blink during slow subpixel pans.
            float const px = u * world_image->width - 0.5F;
            float const py = v * world_image->height - 0.5F;
            int64_t const x0 = static_cast<int64_t>(std::floor(px));
            int64_t const y0 = static_cast<int64_t>(std::floor(py));
            auto const pixel = [&](int64_t sx, int64_t sy) -> auto const& {
                sx = (sx % world_image->width + world_image->width) % world_image->width;
                sy = (sy % world_image->height + world_image->height) % world_image->height;
                return (*base_rgba)[static_cast<size_t>(sy) * world_image->width + static_cast<size_t>(sx)];
            };
            for (size_t channel = 0U; channel < sampled_color.size(); ++channel) {
                sampled_color[channel] = std::lerp(
                    std::lerp(static_cast<float>(pixel(x0, y0)[channel]),
                        static_cast<float>(pixel(x0 + 1, y0)[channel]), px - static_cast<float>(x0)),
                    std::lerp(static_cast<float>(pixel(x0, y0 + 1)[channel]),
                        static_cast<float>(pixel(x0 + 1, y0 + 1)[channel]), px - static_cast<float>(x0)),
                    py - static_cast<float>(y0));
            }
            return true;
        }
        return false;
    };
    auto blendPanel = [](Color& base, S2MapOverlayLayout::Panel const& panel) {
        if (panel.palette_index >= S2_WORLD_MAP_OVERLAY_PALETTE.size()) {
            return;
        }
        auto const& rgba = S2_WORLD_MAP_OVERLAY_PALETTE[panel.palette_index];
        float const alpha = static_cast<float>(rgba[3U]) / 255.0F;
        for (size_t channel = 0U; channel < base.size(); ++channel) {
            base[channel] = base[channel] * (1.0F - alpha) + static_cast<float>(rgba[channel]) * alpha;
        }
    };
    for (uint32_t y = 0U; y < output_height; ++y) {
        throwIfRasterCancelled(cancellation);
        float const screen_y = (static_cast<float>(y) + 0.5F) * static_cast<float>(height) / output_height;
        for (uint32_t x = 0U; x < output_width; ++x) {
            float const screen_x = (static_cast<float>(x) + 0.5F) * static_cast<float>(width) / output_width;
            Color color = canvas;
            static_cast<void>(drawAt(screen_x, screen_y, color));
            if (river_layer)
            {
                auto const& pixel = river_layer->rgba[static_cast<size_t>(y) * output_width + x];
                float const alpha = pixel[3] / 255.0F;
                for (size_t c = 0U; c < 3U; ++c)
                {
                    color[c] = std::lerp(color[c], static_cast<float>(pixel[c]), alpha);
                }
            }
            for (uint32_t panel_index = 0U; panel_index < overlay_layout.panel_count; ++panel_index) {
                S2MapOverlayLayout::Panel const& panel = overlay_layout.panels[panel_index];
                auto const& bounds = panel.bounds;
                if (screen_x >= bounds[0U] && screen_y >= bounds[1U]
                    && screen_x < bounds[0U] + bounds[2U] && screen_y < bounds[1U] + bounds[3U]) {
                    blendPanel(color, panel);
                }
            }
            for (uint32_t strip_index = 0U; strip_index < overlay_layout.heat_strip_count; ++strip_index) {
                S2MapOverlayLayout::HeatStrip const& strip = overlay_layout.heat_strips[strip_index];
                auto const& bounds = strip.bounds;
                if (screen_x < bounds[0U] || screen_y < bounds[1U]
                    || screen_x >= bounds[0U] + bounds[2U] || screen_y >= bounds[1U] + bounds[3U]
                    || bounds[2U] <= 0.0F) {
                    continue;
                }
                float const position = std::clamp((screen_x - bounds[0U]) / bounds[2U], 0.0F, 1.0F);
                float const range_span = strip.range[1U] - strip.range[0U];
                float value = strip.range[0U] + range_span * position;
                if (strip.layer == S2WorldMapLayer::Precipitation) {
                    value = strip.range[0U] + std::expm1(std::log1p(range_span) * position);
                }
                color = heatColor(strip.layer, value, {strip.range[0U], strip.range[1U]});
            }
            for (uint32_t chip_index = 0U; chip_index < overlay_layout.color_chip_count; ++chip_index) {
                S2MapOverlayLayout::ColorChip const& chip = overlay_layout.color_chips[chip_index];
                auto const& bounds = chip.bounds;
                if (screen_x >= bounds[0U] && screen_y >= bounds[1U]
                    && screen_x < bounds[0U] + bounds[2U] && screen_y < bounds[1U] + bounds[3U]) {
                    color = {static_cast<float>(chip.rgba[0U]), static_cast<float>(chip.rgba[1U]),
                        static_cast<float>(chip.rgba[2U])};
                }
            }
            view.rgba[static_cast<size_t>(y) * output_width + x] = {
                static_cast<uint8_t>(std::clamp(std::lround(color[0U]), 0L, 255L)),
                static_cast<uint8_t>(std::clamp(std::lround(color[1U]), 0L, 255L)),
                static_cast<uint8_t>(std::clamp(std::lround(color[2U]), 0L, 255L)),
                255U,
            };
        }
    }
    return view;
}

} // namespace

std::optional<S2MapImage> composeS2WorldMapView(
    S2MapImage const* const world_image,
    S2MapLayout const& map_layout,
    S2MapOverlayLayout const& overlay_layout,
    uint32_t const width,
    uint32_t const height,
    std::stop_token const cancellation
)
{
    return composeS2WorldMapViewImpl(world_image, map_layout, overlay_layout, width, height,
        nullptr, true, cancellation);
}

std::optional<S2MapImage> composeS2WorldMapViewWithRiverLayer(
    S2MapImage const* const world_image,
    S2MapLayout const& map_layout,
    S2MapOverlayLayout const& overlay_layout,
    uint32_t const width,
    uint32_t const height,
    S2MapImage const* const river_layer,
    std::stop_token const cancellation
)
{
    return composeS2WorldMapViewImpl(world_image, map_layout, overlay_layout, width, height,
        river_layer, false, cancellation);
}

bool applyS2WorldMapAction(
    S2WorldMapState& state,
    S2WorldMapAction const action,
    double const player_x,
    double const player_y,
    bool const fast_pan,
    uint32_t const viewport_width,
    uint32_t const viewport_height
) noexcept
{
    float const minimum_zoom = minimumS2WorldMapZoom(viewport_width, viewport_height);
    switch (action) {
    case S2WorldMapAction::TogglePlates:
    case S2WorldMapAction::ToggleHeights:
    case S2WorldMapAction::ToggleTemperature:
    case S2WorldMapAction::ToggleBiomes:
    case S2WorldMapAction::ToggleWind:
    case S2WorldMapAction::ToggleCurrents:
    case S2WorldMapAction::ToggleMoisture:
    case S2WorldMapAction::TogglePrecipitation: {
        size_t const index = static_cast<size_t>(action);
        state.layers[index] = !state.layers[index];
        return true;
    }
    case S2WorldMapAction::PanLeft:
    case S2WorldMapAction::PanRight:
    case S2WorldMapAction::PanUp:
    case S2WorldMapAction::PanDown: {
        double const step = (fast_pan ? 0.018 : 0.006)
            / std::clamp(state.zoom, static_cast<double>(minimum_zoom), 6.0) * WORLD_SIZE;
        if (action == S2WorldMapAction::PanLeft) state.center_x -= step;
        if (action == S2WorldMapAction::PanRight) state.center_x += step;
        if (action == S2WorldMapAction::PanUp) state.center_y -= step;
        if (action == S2WorldMapAction::PanDown) state.center_y += step;
        state.center_x = wrapWorld(state.center_x);
        state.center_y = wrapWorld(state.center_y);
        state.overview = false;
        return true;
    }
    case S2WorldMapAction::ZoomIn:
        state.zoom = std::min(6.0, std::max(static_cast<double>(minimum_zoom), state.zoom) + 0.25);
        state.overview = false;
        return true;
    case S2WorldMapAction::ZoomOut:
        state.zoom = std::max(static_cast<double>(minimum_zoom), std::min(6.0, state.zoom) - 0.25);
        state.overview = false;
        return true;
    case S2WorldMapAction::Overview:
        state.overview = true;
        state.zoom = minimum_zoom;
        state.center_x = WORLD_SIZE * 0.5;
        state.center_y = WORLD_SIZE * 0.5;
        return true;
    case S2WorldMapAction::Reset:
        state.overview = false;
        state.zoom = 1.0;
        state.center_x = wrapWorld(player_x);
        state.center_y = wrapWorld(player_y);
        return true;
    case S2WorldMapAction::ToggleLegend:
        state.legend_expanded = !state.legend_expanded;
        return true;
    }
    return false;
}

} // namespace client
