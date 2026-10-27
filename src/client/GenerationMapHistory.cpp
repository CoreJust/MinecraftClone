#include <client/GenerationMapHistory.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace client
{
namespace
{
constexpr uint32_t MAX_HISTORY_EVENTS = 16U;
constexpr double INPUT_MAGNITUDE_LIMIT = static_cast<double>(std::numeric_limits<float>::max())
    / (MAX_HISTORY_EVENTS + 8.0);

bool supportedValue(double value)
{
    return std::isfinite(value) && std::abs(value) <= INPUT_MAGNITUDE_LIMIT;
}

double smooth(double value)
{
    value = std::clamp(value, 0.0, 1.0);
    return value * value * value * (10.0 + value * (-15.0 + value * 6.0));
}
double wrap(double value, double side)
{
    value = std::fmod(value, side);
    return value < 0.0 ? value + side : value;
}
float periodicCoordinate(double value, uint32_t side)
{
    float const coordinate = static_cast<float>(wrap(value, side));
    // Rounding a wrapped double to float can reach the periodic upper endpoint.
    return coordinate < static_cast<float>(side) ? coordinate : 0.0F;
}
double sample(std::vector<double> const& values, uint32_t side, double x, double y)
{
    x = wrap(x, side); y = wrap(y, side);
    auto const ix = static_cast<uint32_t>(x), iy = static_cast<uint32_t>(y);
    double const fx = x - ix, fy = y - iy;
    return std::lerp(std::lerp(values[iy * side + ix], values[iy * side + (ix + 1U) % side], fx),
        std::lerp(values[((iy + 1U) % side) * side + ix], values[((iy + 1U) % side) * side + (ix + 1U) % side], fx), fy);
}
bool supportedFields(std::span<double const> values)
{
    return std::all_of(values.begin(), values.end(), supportedValue);
}
}

struct GenerationMapHistory::Data final
{
    struct Core final { GenerationHistoryCore motion; double length; };
    struct Event final { std::vector<double> offset; double begin; double end; };
    uint32_t side = 0U;
    double sea = 0.0;
    double range = 1.0;
    std::vector<double> height, water, vacated_height, vacated_water;
    std::vector<float> boundary, departure;
    std::vector<Core> cores;
    std::vector<Event> events;
};

GenerationMapHistory::GenerationMapHistory(std::shared_ptr<Data const> data) : data_(std::move(data)) {}

std::optional<GenerationMapHistory> GenerationMapHistory::prepare(
    GenerationHistorySource const& source, std::span<GenerationHistoryCore const> cores,
    std::span<GenerationHistoryEvent const> events)
{
    uint32_t const side = source.resolution;
    size_t const count = static_cast<size_t>(side) * side;
    if (side < 4U || side > 512U || source.elevation_m.size() != count || source.water_level_m.size() != count
        || source.plate_ids.size() != count || !supportedValue(source.sea_level_m)
        || !supportedFields(source.elevation_m) || !supportedFields(source.water_level_m)
        || cores.size() > 64U || events.size() > MAX_HISTORY_EVENTS)
        return std::nullopt;
    auto data = std::make_shared<Data>(); data->side = side; data->sea = source.sea_level_m;
    data->height.assign(source.elevation_m.begin(), source.elevation_m.end());
    data->water.assign(source.water_level_m.begin(), source.water_level_m.end());
    data->vacated_height = data->height; data->vacated_water = data->water;
    data->departure.assign(count, 0.0F);
    auto const bounds = std::minmax_element(data->height.begin(), data->height.end());
    data->range = std::max(1.0, *bounds.second - *bounds.first);
    std::vector<uint32_t> queue; queue.reserve(count);
    std::vector<int32_t> distance(count, -1);
    auto neighbors = [side](uint32_t index) {
        uint32_t const x = index % side, y = index / side;
        return std::array<uint32_t, 4U>{y * side + (x + 1U) % side, y * side + (x + side - 1U) % side,
            ((y + 1U) % side) * side + x, ((y + side - 1U) % side) * side + x};
    };
    for (uint32_t index = 0U; index < count; ++index)
        for (uint32_t neighbor : neighbors(index))
            if (source.plate_ids[index] != source.plate_ids[neighbor]) { distance[index] = 0; queue.push_back(index); break; }
    for (size_t cursor = 0U; cursor < queue.size(); ++cursor)
        for (uint32_t neighbor : neighbors(queue[cursor]))
            if (distance[neighbor] < 0) { distance[neighbor] = distance[queue[cursor]] + 1; queue.push_back(neighbor); }
    data->boundary.resize(count);
    for (size_t index = 0U; index < count; ++index)
        data->boundary[index] = distance[index] < 0 ? 0.0F : static_cast<float>(std::exp(-distance[index] / 2.5));
    for (auto const& core : cores)
    {
        if (!supportedValue(core.source_center[0]) || !supportedValue(core.source_center[1])
            || !supportedValue(core.initial_displacement_cells[0]) || !supportedValue(core.initial_displacement_cells[1])
            || !supportedValue(core.initial_rotation_radians) || !supportedValue(core.solid_radius_cells)
            || !supportedValue(core.blend_radius_cells) || core.solid_radius_cells <= 0.0
            || core.blend_radius_cells <= core.solid_radius_cells || core.blend_radius_cells >= side * .5
            || std::abs(core.initial_displacement_cells[0]) > side * 2.0
            || std::abs(core.initial_displacement_cells[1]) > side * 2.0) return std::nullopt;
        Data::Core prepared{core, std::hypot(core.initial_displacement_cells[0], core.initial_displacement_cells[1])};
        prepared.motion.source_center = {wrap(core.source_center[0], side), wrap(core.source_center[1], side)};
        auto const& center = prepared.motion.source_center;
        // Vacate only the connected dry source footprint inside this solid core.
        // The prepared seabed inherits surrounding bathymetry; it is never restored
        // by a distance fade. The returning rigid body covers it geometrically.
        std::vector<uint8_t> hole(count, 0U);
        std::vector<uint32_t> footprint;
        uint32_t seed = 0U; double nearest = std::numeric_limits<double>::max();
        auto radiusSquared = [&](uint32_t index) {
            double const dx = wrap(index % side - center[0] + side * .5, side) - side * .5;
            double const dy = wrap(index / side - center[1] + side * .5, side) - side * .5;
            return dx * dx + dy * dy;
        };
        for (uint32_t index = 0U; index < count; ++index)
            if (data->height[index] > data->sea && radiusSquared(index) < nearest
                && radiusSquared(index) < core.solid_radius_cells * core.solid_radius_cells)
            { seed = index; nearest = radiusSquared(index); }
        if (nearest != std::numeric_limits<double>::max())
        {
            footprint.push_back(seed); hole[seed] = 1U;
            for (size_t cursor = 0U; cursor < footprint.size(); ++cursor)
                for (uint32_t neighbor : neighbors(footprint[cursor]))
                    if (!hole[neighbor] && data->height[neighbor] > data->sea
                        && radiusSquared(neighbor) < core.solid_radius_cells * core.solid_radius_cells)
                    { hole[neighbor] = 1U; footprint.push_back(neighbor); }
            double height_sum = 0.0, water_sum = 0.0; size_t boundary_count = 0U;
            for (uint32_t index : footprint)
                for (uint32_t neighbor : neighbors(index))
                    if (!hole[neighbor])
                    { height_sum += data->vacated_height[neighbor]; water_sum += data->vacated_water[neighbor]; ++boundary_count; }
            if (boundary_count != 0U)
            {
                for (uint32_t index : footprint)
                {
                    data->vacated_height[index] = height_sum / static_cast<double>(boundary_count);
                    data->vacated_water[index] = water_sum / static_cast<double>(boundary_count);
                    data->departure[index] = 1.0F;
                }
                std::vector<double> next_height = data->vacated_height, next_water = data->vacated_water;
                for (uint32_t iteration = 0U; iteration < 512U; ++iteration)
                {
                    double change = 0.0;
                    for (uint32_t index : footprint)
                    {
                        double height = 0.0, water = 0.0;
                        for (uint32_t neighbor : neighbors(index))
                        { height += data->vacated_height[neighbor]; water += data->vacated_water[neighbor]; }
                        next_height[index] = height * .25; next_water[index] = water * .25;
                        change = std::max(change, std::abs(next_height[index] - data->vacated_height[index]));
                    }
                    data->vacated_height.swap(next_height); data->vacated_water.swap(next_water);
                    if (change < 1.0e-9) break;
                }
            }
        }
        data->cores.push_back(std::move(prepared));
    }
    for (auto const& event : events)
    {
        if (event.height_offset_m.size() != count || !supportedFields(event.height_offset_m)
            || !supportedValue(event.begin) || !supportedValue(event.end)
            || event.begin < 0.0 || event.end > .78 || event.begin >= event.end) return std::nullopt;
        data->events.push_back({{event.height_offset_m.begin(), event.height_offset_m.end()}, event.begin, event.end});
    }
    return GenerationMapHistory{std::move(data)};
}

bool GenerationMapHistory::evaluate(double time, GenerationHistoryFrame& frame) const
{
    if (!std::isfinite(time) || time < 0.0 || time > 1.0 || !data_) return false;
    auto const& data = *data_; uint32_t const side = data.side; size_t const count = data.height.size();
    frame.resolution = side;
    frame.elevation_m = data.vacated_height;
    frame.water_level_m = data.vacated_water;
    frame.next_elevation_m.resize(count); frame.warm_weight.assign(count, 0.0F); frame.core_weight.assign(count, 0.0F);
    frame.final_coordinates.resize(count);
    frame.liquid_fraction.resize(count);
    frame.boundary_heat.resize(count);
    frame.water_depth_m.resize(count);
    for (uint32_t index = 0U; index < count; ++index)
        frame.final_coordinates[index] = {static_cast<float>(index % side), static_cast<float>(index / side)};
    // The last interval already has the final scalar world; weather can develop without a terrain switch.
    double const q = 1.0 - smooth((time - .18) / .60);
    for (auto const& prepared : data.cores)
    {
        auto const& core = prepared.motion;
        double const cx = core.source_center[0] + q * core.initial_displacement_cells[0];
        double const cy = core.source_center[1] + q * core.initial_displacement_cells[1];
        double const angle = q * core.initial_rotation_radians, cs = std::cos(angle), sn = std::sin(angle);
        double const strength = smooth(
            q * std::max(prepared.length, std::abs(core.initial_rotation_radians) * core.solid_radius_cells)
                / (core.blend_radius_cells - core.solid_radius_cells));
        int32_t const x0 = static_cast<int32_t>(std::floor(cx - core.blend_radius_cells));
        int32_t const x1 = static_cast<int32_t>(std::ceil(cx + core.blend_radius_cells));
        int32_t const y0 = static_cast<int32_t>(std::floor(cy - core.blend_radius_cells));
        int32_t const y1 = static_cast<int32_t>(std::ceil(cy + core.blend_radius_cells));
        for (int32_t y = y0; y <= y1; ++y) for (int32_t x = x0; x <= x1; ++x)
        {
            double const dx = x - cx, dy = y - cy, radius = std::hypot(dx, dy);
            if (radius >= core.blend_radius_cells) continue;
            uint32_t const index = static_cast<uint32_t>(wrap(y, side)) * side + static_cast<uint32_t>(wrap(x, side));
            float const weight = static_cast<float>(
                1.0 - smooth((radius - core.solid_radius_cells) / (core.blend_radius_cells - core.solid_radius_cells)));
            if (weight < frame.core_weight[index]) continue;
            double const sx = core.source_center[0] + cs * dx + sn * dy;
            double const sy = core.source_center[1] - sn * dx + cs * dy;
            frame.elevation_m[index] = std::lerp(
                data.vacated_height[index], sample(data.height, side, sx, sy), static_cast<double>(weight));
            frame.water_level_m[index] = std::lerp(
                data.vacated_water[index], sample(data.water, side, sx, sy), static_cast<double>(weight));
            frame.warm_weight[index] = std::max(frame.warm_weight[index],
                static_cast<float>(strength * 4.0 * weight * (1.0 - weight)));
            frame.final_coordinates[index] = {periodicCoordinate(sx, side), periodicCoordinate(sy, side)};
            frame.core_weight[index] = weight;
        }
    }
    for (auto const& event : data.events)
    {
        if (time <= event.begin || time >= event.end) continue;
        double const t = (time - event.begin) / (event.end - event.begin);
        double const strength = 64.0 * t * t * t * (1.0 - t) * (1.0 - t) * (1.0 - t);
        for (size_t index = 0U; index < count; ++index)
        {
            auto const uv = frame.final_coordinates[index];
            frame.elevation_m[index] += strength * sample(event.offset, side, uv[0], uv[1]);
        }
    }
    std::vector<uint32_t> active; active.reserve(count / 3U);
    for (uint32_t index = 0U; index < count; ++index)
    {
        if (frame.warm_weight[index] > 0.0F) active.push_back(index);
    }
    frame.next_elevation_m = frame.elevation_m;
    // Screened harmonic scalar joins retain adjacent bathymetry. Solid cores and
    // the far field are Dirichlet values; there is no empty-pixel underlay.
    std::vector<double> const initial = frame.elevation_m;
    for (uint32_t iteration = 0U; iteration < 96U && !active.empty(); ++iteration)
    {
        for (uint32_t index : active)
        {
            uint32_t const x = index % side, y = index / side;
            double const average = .25 * (frame.elevation_m[y * side + (x + 1U) % side]
                + frame.elevation_m[y * side + (x + side - 1U) % side]
                + frame.elevation_m[((y + 1U) % side) * side + x]
                + frame.elevation_m[((y + side - 1U) % side) * side + x]);
            frame.next_elevation_m[index] = std::lerp(initial[index], average, static_cast<double>(frame.warm_weight[index]));
        }
        frame.elevation_m.swap(frame.next_elevation_m);
    }
    double const formation = .12 + .88 * smooth(time / .18);
    double const rain = smooth((time - .40) / .32);
    double const residual = 1.0 - smooth((time - .40) / .30);
    for (size_t index = 0U; index < count; ++index)
    {
        frame.elevation_m[index] = formation == 1.0 ? frame.elevation_m[index]
            : data.sea + formation * (frame.elevation_m[index] - data.sea);
        auto const uv = frame.final_coordinates[index];
        uint32_t const source_index = static_cast<uint32_t>(uv[1]) * side + static_cast<uint32_t>(uv[0]);
        float const border = data.boundary[source_index];
        double const rise = std::clamp((frame.elevation_m[index] - data.sea) / data.range, 0.0, 1.0);
        double const onset = .10 + .17 * (1.0 - rise) + .16 * border;
        float const hot = static_cast<float>(residual * std::max({static_cast<double>(border),
            static_cast<double>(frame.warm_weight[index]),
            static_cast<double>(data.departure[index] * (1.0F - frame.core_weight[index]))}));
        frame.boundary_heat[index] = hot;
        frame.liquid_fraction[index] = std::max(static_cast<float>(1.0 - smooth((time - onset) / .20)), hot);
        double const water = frame.water_level_m[index];
        frame.water_depth_m[index] = rain == 0.0 ? 0.0F
            : static_cast<float>(std::max(0.0, water - (1.0 - rain) * data.range - frame.elevation_m[index]));
    }
    return true;
}
} // namespace client
