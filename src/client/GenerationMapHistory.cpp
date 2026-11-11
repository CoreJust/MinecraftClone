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
constexpr double TAU = 6.283'185'307'179'586'476'9;

bool supportedValue(double value)
{
    return std::isfinite(value) && std::abs(value) <= INPUT_MAGNITUDE_LIMIT;
}
bool supportedFields(std::span<double const> values)
{
    return std::all_of(values.begin(), values.end(), supportedValue);
}
double smooth(double value)
{
    value = std::clamp(value, 0.0, 1.0);
    return value * value * value * (10.0 + value * (-15.0 + value * 6.0));
}
double wrap(double value, double side)
{
    value = std::fmod(value, side);
    if (value < 0.0) value += side;
    // Adding the period to a negative sub-ULP remainder can round up to side.
    return value < side ? value : 0.0;
}
float periodicCoordinate(double value, uint32_t side)
{
    float const coordinate = static_cast<float>(wrap(value, side));
    return coordinate < static_cast<float>(side) ? coordinate : 0.0F;
}
template<typename T>
double sample(std::vector<T> const& values, uint32_t side, double x, double y)
{
    x = wrap(x, side); y = wrap(y, side);
    auto const ix = static_cast<uint32_t>(x), iy = static_cast<uint32_t>(y);
    double const fx = x - ix, fy = y - iy;
    return std::lerp(std::lerp(static_cast<double>(values[iy * side + ix]),
        static_cast<double>(values[iy * side + (ix + 1U) % side]), fx),
        std::lerp(static_cast<double>(values[((iy + 1U) % side) * side + ix]),
        static_cast<double>(values[((iy + 1U) % side) * side + (ix + 1U) % side]), fx), fy);
}
std::array<uint32_t, 4U> neighbors(uint32_t index, uint32_t side)
{
    uint32_t const x = index % side, y = index / side;
    return {y * side + (x + 1U) % side, y * side + (x + side - 1U) % side,
        ((y + 1U) % side) * side + x, ((y + side - 1U) % side) * side + x};
}
}

struct GenerationMapHistory::Data final
{
    struct Core final
    {
        std::array<double, 2U> center, displacement;
        double rotation, collar;
        uint64_t id;
        std::vector<uint8_t> footprint;
        std::vector<double> support, masked_height, masked_water;
    };
    struct Event final { std::vector<double> offset; double begin; double end; };
    uint32_t side = 0U;
    double sea = 0.0, range = 1.0;
    std::vector<double> height, water, vacated_height, vacated_water, boundary;
    std::vector<uint64_t> owner_id;
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
        || cores.size() > 64U || events.size() > MAX_HISTORY_EVENTS) return std::nullopt;
    auto data = std::make_shared<Data>(); data->side = side; data->sea = source.sea_level_m;
    data->height.assign(source.elevation_m.begin(), source.elevation_m.end());
    data->water.assign(source.water_level_m.begin(), source.water_level_m.end());
    data->vacated_height = data->height; data->vacated_water = data->water;
    auto const bounds = std::minmax_element(data->height.begin(), data->height.end());
    data->range = std::max(1.0, *bounds.second - *bounds.first);
    std::vector<uint8_t> owned(count, 0U);
    std::vector<uint32_t> queue; queue.reserve(count);
    std::vector<int32_t> distance(count, -1);
    for (uint32_t index = 0U; index < count; ++index)
        for (uint32_t neighbor : neighbors(index, side))
            if (source.plate_ids[index] != source.plate_ids[neighbor])
            { distance[index] = 0; queue.push_back(index); break; }
    for (size_t cursor = 0U; cursor < queue.size(); ++cursor)
        for (uint32_t neighbor : neighbors(queue[cursor], side))
            if (distance[neighbor] < 0)
            { distance[neighbor] = distance[queue[cursor]] + 1; queue.push_back(neighbor); }
    data->boundary.resize(count);
    for (size_t index = 0U; index < count; ++index)
        data->boundary[index] = distance[index] < 0 ? 0.0 : std::exp(-distance[index] / 2.5);
    for (auto const& core : cores)
    {
        if (!supportedValue(core.source_center[0]) || !supportedValue(core.source_center[1])
            || !supportedValue(core.initial_displacement_cells[0]) || !supportedValue(core.initial_displacement_cells[1])
            || !supportedValue(core.initial_rotation_radians) || !supportedValue(core.support_collar_cells)
            || core.support_collar_cells <= 0.0 || core.support_collar_cells > side * .25
            || std::abs(core.initial_displacement_cells[0]) > side * 2.0
            || std::abs(core.initial_displacement_cells[1]) > side * 2.0
            || core.source_footprint.size() != count) return std::nullopt;
        Data::Core body{{wrap(core.source_center[0], side), wrap(core.source_center[1], side)},
            core.initial_displacement_cells, core.initial_rotation_radians, core.support_collar_cells,
            14'695'981'039'346'656'037ULL, {core.source_footprint.begin(), core.source_footprint.end()}, {}, {}, {}};
        bool nonempty = false;
        queue.clear(); std::fill(distance.begin(), distance.end(), -1);
        body.masked_height.assign(count, 0.0); body.masked_water.assign(count, 0.0);
        for (uint32_t index = 0U; index < count; ++index)
        {
            uint8_t const cell = body.footprint[index];
            if (cell > 1U || (cell && owned[index])) return std::nullopt;
            body.id = (body.id ^ cell) * 1'099'511'628'211ULL;
            if (!cell) continue;
            nonempty = true; owned[index] = 1U; distance[index] = 0; queue.push_back(index);
            body.masked_height[index] = data->height[index];
            body.masked_water[index] = data->water[index];
        }
        if (!nonempty) return std::nullopt;
        if (body.id == 0U) body.id = 1U;
        for (size_t cursor = 0U; cursor < queue.size(); ++cursor)
            for (uint32_t neighbor : neighbors(queue[cursor], side))
                if (distance[neighbor] < 0)
                { distance[neighbor] = distance[queue[cursor]] + 1; queue.push_back(neighbor); }
        body.support.resize(count);
        for (size_t index = 0U; index < count; ++index)
            body.support[index] = 1.0 - smooth(distance[index] / body.collar);
        data->cores.push_back(std::move(body));
    }
    // Canonical order controls only floating reductions and provenance, never ownership priority.
    std::sort(data->cores.begin(), data->cores.end(), [](auto const& a, auto const& b) {
        return a.id != b.id ? a.id < b.id : a.footprint < b.footprint;
    });
    data->owner_id.assign(count, 0U);
    for (auto const& body : data->cores)
        for (size_t index = 0U; index < count; ++index)
            if (body.footprint[index]) data->owner_id[index] = body.id;
    queue.clear(); double height_sum = 0.0, water_sum = 0.0; size_t boundary_count = 0U;
    for (uint32_t index = 0U; index < count; ++index)
    {
        if (!owned[index]) continue;
        queue.push_back(index);
        for (uint32_t neighbor : neighbors(index, side))
            if (!owned[neighbor])
            { height_sum += data->height[neighbor]; water_sum += data->water[neighbor]; ++boundary_count; }
    }
    if (!queue.empty())
    {
        if (boundary_count == 0U) return std::nullopt;
        for (uint32_t index : queue)
        {
            data->vacated_height[index] = height_sum / static_cast<double>(boundary_count);
            data->vacated_water[index] = water_sum / static_cast<double>(boundary_count);
        }
        auto next_height = data->vacated_height, next_water = data->vacated_water;
        for (uint32_t iteration = 0U; iteration < 512U; ++iteration)
        {
            double change = 0.0;
            for (uint32_t index : queue)
            {
                double height = 0.0, water = 0.0;
                for (uint32_t neighbor : neighbors(index, side))
                { height += data->vacated_height[neighbor]; water += data->vacated_water[neighbor]; }
                next_height[index] = height * .25; next_water[index] = water * .25;
                change = std::max({change, std::abs(next_height[index] - data->vacated_height[index]),
                    std::abs(next_water[index] - data->vacated_water[index])});
            }
            data->vacated_height.swap(next_height); data->vacated_water.swap(next_water);
            if (change < 1.0e-9) break;
        }
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
    frame.elevation_m.resize(count); frame.water_level_m.resize(count);
    frame.next_elevation_m.resize(count); frame.warm_weight.assign(count, 0.0F); frame.core_weight.assign(count, 0.0F);
    frame.final_coordinates.resize(count); frame.liquid_fraction.resize(count); frame.boundary_heat.resize(count);
    frame.water_depth_m.resize(count); frame.overlap_fraction.assign(count, 0.0F); frame.source_body_id.assign(count, 0U);
    frame.source_mapping_weight.assign(count, 0.0F);
    double const q = 1.0 - smooth((time - .18) / .60);
    double const formation = .12 + .88 * smooth(time / .18);
    double const rain = smooth((time - .40) / .32), residual = 1.0 - smooth((time - .40) / .30);
    std::vector<double> event_strength;
    for (auto const& event : data.events)
    {
        double const t = (time - event.begin) / (event.end - event.begin);
        event_strength.push_back(t <= 0.0 || t >= 1.0 ? 0.0
            : 64.0 * t * t * t * (1.0 - t) * (1.0 - t) * (1.0 - t));
    }
    auto offsetAt = [&](double x, double y) {
        double offset = 0.0;
        for (size_t event = 0U; event < data.events.size(); ++event)
            if (event_strength[event] != 0.0)
                offset += event_strength[event] * sample(data.events[event].offset, side, x, y);
        return offset;
    };
    struct Motion final { Data::Core const* body; double cx, cy, cs, sn; };
    std::vector<Motion> motions;
    for (auto const& body : data.cores)
    {
        double const angle = q * body.rotation;
        motions.push_back({&body, body.center[0] + q * body.displacement[0],
            body.center[1] + q * body.displacement[1], std::cos(angle), std::sin(angle)});
    }
    for (uint32_t index = 0U; index < count; ++index)
    {
        double const x = index % side, y = index / side;
        double const background = data.vacated_height[index] + offsetAt(x, y);
        double height_excess = 0.0, water_excess = 0.0;
        double sum = 0.0, overlap_sum = 0.0, maximum = 0.0, heat_sum = 0.0;
        std::array<double, 4U> circular{}; uint64_t unique = 0U; size_t contributors = 0U;
        frame.final_coordinates[index] = {static_cast<float>(x), static_cast<float>(y)};
        if (q != 0.0)
        for (auto const& motion : motions)
        {
            auto const& body = *motion.body;
            double const dx = wrap(x - motion.cx + side * .5, side) - side * .5;
            double const dy = wrap(y - motion.cy + side * .5, side) - side * .5;
            for (int image_y = -1; image_y <= 1; ++image_y)
            for (int image_x = -1; image_x <= 1; ++image_x)
            {
                double const px = dx + image_x * static_cast<double>(side);
                double const py = dy + image_y * static_cast<double>(side);
                double const rx = motion.cs * px + motion.sn * py, ry = -motion.sn * px + motion.cs * py;
                if (rx < -static_cast<double>(side) * .5 || rx >= side * .5
                    || ry < -static_cast<double>(side) * .5 || ry >= side * .5) continue;
                double const sx = body.center[0] + rx, sy = body.center[1] + ry;
                double const coverage = sample(body.footprint, side, sx, sy);
                if (coverage <= 0.0) continue;
                double const support = sample(body.support, side, sx, sy);
                double const h = sample(body.masked_height, side, sx, sy) + coverage * offsetAt(sx, sy);
                double const w = sample(body.masked_water, side, sx, sy);
                height_excess += support * (h - coverage * background);
                water_excess += support * (w - coverage * data.vacated_water[index]);
                overlap_sum += 2.0 * sum * coverage; sum += coverage; maximum = std::max(maximum, coverage);
                heat_sum += coverage * sample(data.boundary, side, sx, sy);
                double const ax = wrap(sx, side) * TAU / side, ay = wrap(sy, side) * TAU / side;
                circular[0] += coverage * std::cos(ax); circular[1] += coverage * std::sin(ax);
                circular[2] += coverage * std::cos(ay); circular[3] += coverage * std::sin(ay);
                unique = body.id; ++contributors;
                frame.warm_weight[index] = std::max(frame.warm_weight[index],
                    static_cast<float>(4.0 * coverage * (1.0 - coverage)));
            }
        }
        // Normalized coverage keeps every contributor and forms convex weights with the background.
        double const normalization = std::max(1.0, sum);
        double height = q == 0.0 ? data.height[index] : background + height_excess / normalization;
        double water = q == 0.0 ? data.water[index] : data.vacated_water[index] + water_excess / normalization;
        double const overlap = std::clamp(overlap_sum, 0.0, 1.0);
        double border = data.boundary[index], mapping_confidence = 1.0;
        if (sum > 0.0)
        {
            border = std::lerp(border, heat_sum / sum, maximum);
            if (contributors > 1U)
                mapping_confidence = std::clamp(std::min(std::hypot(circular[0], circular[1]),
                    std::hypot(circular[2], circular[3])) / sum, 0.0, 1.0);
            frame.final_coordinates[index] = {
                periodicCoordinate(std::atan2(circular[1], circular[0]) * side / TAU, side),
                periodicCoordinate(std::atan2(circular[3], circular[2]) * side / TAU, side)};
            frame.source_body_id[index] = contributors == 1U ? unique : 0U;
        }
        frame.core_weight[index] = q == 0.0 ? (data.owner_id[index] != 0U ? 1.0F : 0.0F)
            : static_cast<float>(maximum);
        if (q == 0.0) frame.source_body_id[index] = data.owner_id[index];
        frame.overlap_fraction[index] = static_cast<float>(overlap);
        frame.source_mapping_weight[index] = q == 0.0 ? frame.core_weight[index]
            : static_cast<float>(maximum * (1.0 - overlap) * mapping_confidence);
        height = formation == 1.0 ? height : data.sea + formation * (height - data.sea);
        frame.elevation_m[index] = height; frame.water_level_m[index] = water;
        frame.next_elevation_m[index] = height;
        double const rise = std::clamp((height - data.sea) / data.range, 0.0, 1.0);
        double const onset = .10 + .17 * (1.0 - rise) + .16 * border;
        float const hot = static_cast<float>(std::max(residual * border, overlap));
        frame.boundary_heat[index] = hot;
        frame.liquid_fraction[index] = std::max(static_cast<float>(1.0 - smooth((time - onset) / .20)), hot);
        frame.water_depth_m[index] = rain == 0.0 ? 0.0F
            : static_cast<float>(std::max(0.0, water - (1.0 - rain) * data.range - height));
    }
    return true;
}
} // namespace client
