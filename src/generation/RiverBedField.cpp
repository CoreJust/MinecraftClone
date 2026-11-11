#include <generation/RiverBedChannel.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace river_field_candidate
{
namespace
{
using Key = std::array<int64_t, 2>;
using NodeKey = std::pair<int, int64_t>; // 0: vertex, 1: shore reach index

WorldXY add(WorldXY a, WorldXY b) { return {a.x_m + b.x_m, a.y_m + b.y_m}; }
WorldXY sub(WorldXY a, WorldXY b) { return {a.x_m - b.x_m, a.y_m - b.y_m}; }
WorldXY mul(WorldXY a, double s) { return {a.x_m * s, a.y_m * s}; }
double dot(WorldXY a, WorldXY b) { return a.x_m * b.x_m + a.y_m * b.y_m; }
double length(WorldXY a) { return std::hypot(a.x_m, a.y_m); }
bool finite(double value) { return std::isfinite(value); }
bool finite(WorldXY p) { return finite(p.x_m) && finite(p.y_m); }
double clip(double value, double lower, double upper) { return std::clamp(value, lower, upper); }
double smooth(double value)
{
    const double t = clip(value, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
[[noreturn]] void fail(FailureCode code, std::string message)
{
    throw Failure{code, std::move(message)};
}
void require(bool condition, FailureCode code, const char* message)
{
    if (!condition) {
        fail(code, message);
    }
}
double wrap(double value, double period)
{
    double result = std::fmod(value, period);
    if (result < 0) {
        result += period;
    }
    return result >= period ? 0.0 : result;
}
WorldXY wrap(WorldXY point, double period)
{
    return {wrap(point.x_m, period), wrap(point.y_m, period)};
}
Key key(WorldXY point, double period)
{
    const auto scale = static_cast<int64_t>(std::nearbyint(period * 1e8));
    const WorldXY p = wrap(point, period);
    auto component = [scale](double value) {
        auto number = static_cast<int64_t>(std::nearbyint(value * 1e8)) % scale;
        return number < 0 ? number + scale : number;
    };
    return {component(p.x_m), component(p.y_m)};
}
WorldXY point(Key k) { return {k[0] * 1e-8, k[1] * 1e-8}; }
double periodicDelta(double a, double b, double period)
{
    return wrap(a - b + period / 2.0, period) - period / 2.0;
}
std::pair<double, double> nearest(WorldXY p, std::vector<WorldXY> const& curve)
{
    double best = std::numeric_limits<double>::infinity();
    double fraction = 0.0;
    for (size_t i = 0; i + 1 < curve.size(); ++i) {
        const WorldXY delta = sub(curve[i + 1], curve[i]);
        const double size2 = dot(delta, delta);
        if (size2 <= 0.0) {
            continue;
        }
        const double u = clip(dot(sub(p, curve[i]), delta) / size2, 0.0, 1.0);
        const WorldXY difference = sub(p, add(curve[i], mul(delta, u)));
        const double candidate = dot(difference, difference);
        if (candidate < best) {
            best = candidate;
            fraction = (static_cast<double>(i) + u) / (curve.size() - 1);
        }
    }
    require(finite(best), FailureCode::InvalidInput, "course has no nondegenerate segment");
    return {std::sqrt(best), fraction};
}
struct Outlet
{
    WorldXY anchor;
    WorldXY extension;
    int32_t owner;
};
struct Observation
{
    WorldXY xy;
    double grade;
    double q;
    WorldXY direction;
    double slope;
};
struct Node
{
    WorldXY xy;
    WorldXY course_xy;
    WorldXY tangent;
    double grade;
    double halfwidth;
    double depth;
    double valley;
    double valley_rise;
};
struct Course
{
    size_t reach_index;
    std::vector<WorldXY> xy;
    NodeKey start;
    NodeKey target;
    bool shore;
    bool receiver_extension;
    int32_t receiver_owner;
};
struct Image
{
    size_t course_index;
    WorldXY offset;
    std::vector<WorldXY> xy;
};
struct Target
{
    double distance;
    double width;
    double depth;
    double grade;
    bool shore;
};
} // namespace

class PreparedRiverField
{
public:
    RiverFieldIdentity identity;
    VisualCalibration calibration;
    ProbeOptions options;
    std::vector<RiverReach> reaches;
    std::vector<size_t> source_reach_indices;
    std::vector<ProtectedContact> contacts;
    std::map<NodeKey, Node> nodes;
    std::vector<Course> courses;
    std::map<std::pair<int, int>, std::vector<Image>> buckets;
    double bucket_m = 64.0;
};

namespace
{
double applyBed(PreparedRiverField const& field, NativePointSample const& native,
    WorldXY p, double target_grade, double corridor_support, std::span<Target const> targets)
{
    const double period = field.identity.period_m;
    const double dry_permission = smooth(native.foundation_m - native.standing_stage_m);
    double contact_permission = 1.0;
    for (auto const& contact : field.contacts) {
        const double dx = periodicDelta(p.x_m, contact.xy.x_m, period);
        const double dy = periodicDelta(p.y_m, contact.xy.y_m, period);
        contact_permission *= smooth(std::hypot(dx, dy) / contact.radius_m);
    }
    const double permission = dry_permission * contact_permission;
    const double valley_cut = corridor_support * permission *
                              std::min(std::max(native.foundation_m - target_grade, 0.0), 12.0);
    const double valley_bed = native.foundation_m - valley_cut;
    double channel_cut = 0.0;
    double channel_fill = 0.0;
    for (auto const& target : targets) {
        const double section = target.grade - target.depth + target.depth *
                               std::pow(std::min(target.distance / target.width, 1.0), 2.0);
        const double channel_weight = smooth((1.25 * target.width - target.distance) /
            (0.35 * target.width));
        const double channel_permission = target.shore ? contact_permission : permission;
        const double delta = clip(section - valley_bed, -16.0, 1.2);
        channel_cut = std::max(channel_cut, channel_permission * channel_weight * std::max(-delta, 0.0));
        if (!target.shore) {
            channel_fill = std::max(channel_fill, channel_permission * channel_weight * std::max(delta, 0.0));
        }
    }
    const double actual_fill = channel_fill * (1.0 - smooth(channel_cut / 0.25));
    return valley_bed - channel_cut + actual_fill;
}

RiverChannelSample sampleOne(PreparedRiverField const& field, NativePointSample const& native,
    bool const include_containing_courses = false)
{
    const double period = field.identity.period_m;
    const WorldXY p = wrap(native.xy, period);
    const auto cell = std::pair{static_cast<int>(std::floor(p.x_m / field.bucket_m)),
        static_cast<int>(std::floor(p.y_m / field.bucket_m))};
    const auto found = field.buckets.find(cell);
    RiverChannelSample output{{native.xy, native.foundation_m, native.foundation_m, 0.0, 0.0},
                              native.standing_stage_m, native.standing_owner, std::nullopt, {}};
    if (found == field.buckets.end()) {
        return output;
    }
    RiverBedSample& result = output.bed;

    std::map<size_t, std::pair<double, double>> closest;
    for (auto const& image : found->second) {
        const auto [distance, fraction] = nearest(p, image.xy);
        auto [it, inserted] = closest.emplace(image.course_index, std::pair{distance, fraction});
        if (!inserted && distance < it->second.first) {
            it->second = {distance, fraction};
        }
    }
    double grade_sum = 0.0;
    double weight_sum = 0.0;
    std::vector<Target> targets;
    for (auto const& [id, value] : closest) {
        const Course& course = field.courses[id];
        const Node& start = field.nodes.at(course.start);
        const Node& end = field.nodes.at(course.target);
        const auto [distance, fraction] = value;
        auto interpolate = [fraction](double a, double b) { return a * (1.0 - fraction) + b * fraction; };
        const double width = interpolate(start.halfwidth, end.halfwidth);
        const double depth = interpolate(start.depth, end.depth);
        const double grade = interpolate(start.grade, end.grade);
        const double valley = interpolate(start.valley, end.valley);
        const double rise = interpolate(start.valley_rise, end.valley_rise);
        auto const& reach = field.reaches[course.reach_index];
        NearestCourseGeometry const geometry{id, course.reach_index, distance, width, grade,
            distance <= width, field.source_reach_indices[course.reach_index],
            reach.start_vertex, reach.target_vertex, fraction, course.receiver_extension, course.receiver_owner};
        if (!output.nearest_course || distance < output.nearest_course->centreline_distance_m) {
            output.nearest_course = geometry;
        }
        if (include_containing_courses && geometry.within_nominal_channel) {
            output.containing_courses.push_back(geometry);
        }
        const double weight = smooth(1.0 - distance / valley);
        result.corridor_support = std::max(result.corridor_support, weight);
        const double blend = weight * weight * weight;
        grade_sum += blend * (grade + rise * std::pow(distance / valley, 2.0));
        weight_sum += blend;
        targets.push_back({distance, width, depth, grade, course.shore});
    }
    const double target_grade = weight_sum > 0.0 ? grade_sum / weight_sum : native.foundation_m;
    result.bed_m = applyBed(field, native, p, target_grade, result.corridor_support, targets);
    result.signed_delta_m = result.bed_m - native.foundation_m;
    return output;
}

bool validSample(NativePointSample const& sample)
{
    return finite(sample.xy) && finite(sample.foundation_m) && finite(sample.standing_stage_m) &&
           sample.standing_owner >= 0;
}
// Trial alternatives preserve the graph pins and their endpoint tangents.
void routeInterior(PreparedRiverField const& field, Course& course, NativeBatchSampler const& sampler)
{
    const Node& start = field.nodes.at(course.start);
    const Node& end = field.nodes.at(course.target);
    const RiverReach& reach = field.reaches[course.reach_index];
    const double period = field.identity.period_m;
    const double reach_length = length(reach.delta_xy);
    if (course.shore || course.xy.size() < 9U || reach_length > 1024.0) return;

    const WorldXY normal{-reach.delta_xy.y_m / reach_length, reach.delta_xy.x_m / reach_length};
    const double amplitude = std::min(48.0, reach_length * 0.12);
    const auto mix = [](double a, double b, double t) { return a * (1.0 - t) + b * t; };
    const std::vector<WorldXY> original = course.xy;
    std::vector<WorldXY> best = original;
    double best_score = std::numeric_limits<double>::infinity();
    for (const auto [scale, wave] : std::array<std::pair<double, bool>, 7>{{
             {0.0, false}, {-0.5, false}, {0.5, false}, {-1.0, false}, {1.0, false}, {-1.0, true}, {1.0, true}}}) {
        auto curve = original;
        for (size_t j = 2; j + 2 < curve.size(); ++j) {
            const double t = static_cast<double>(j - 1) / (curve.size() - 3);
            const double taper = 16.0 * t * t * (1.0 - t) * (1.0 - t);
            curve[j] = add(curve[j], mul(normal, amplitude * scale * taper * (wave ? 2.0 * (2.0 * t - 1.0) : 1.0)));
        }
        bool blocked = false;
        for (size_t j = 2; scale != 0.0 && j + 2 < curve.size(); ++j) {
            const WorldXY forward = sub(curve[j + 1], curve[j]);
            const WorldXY back = sub(curve[j], curve[j - 1]);
            const double turn = std::abs(back.x_m * forward.y_m - back.y_m * forward.x_m);
            const double radius_width = 2.0 * std::max(start.halfwidth, end.halfwidth);
            if (dot(back, forward) <= 0.0
                || turn * radius_width > length(back) * length(forward) * length(add(back, forward)) / 2.0) {
                blocked = true;
                break;
            }
            for (auto const& contact : field.contacts) {
                const double protect = contact.radius_m + std::max(start.valley, end.valley) + 4.0;
                for (WorldXY p : {curve[j], original[j]}) {
                    const double dx = periodicDelta(p.x_m, contact.xy.x_m, period);
                    const double dy = periodicDelta(p.y_m, contact.xy.y_m, period);
                    blocked |= std::hypot(dx, dy) < protect;
                }
            }
            if (blocked) break;
        }
        if (blocked) continue;
        std::vector<WorldXY> points;
        // The unchanged endpoints remain junction-owned. Test knots and mid-links inside the reach.
        for (size_t i = 1; i + 1 < curve.size(); ++i) {
            const WorldXY secant = sub(curve[i + 1], curve[i - 1]);
            const double size = length(secant);
            if (size <= 0.0) break;
            const WorldXY transverse{-secant.y_m / size, secant.x_m / size};
            for (double along : {0.0, 0.5}) {
                const double t = (i + along) / (curve.size() - 1);
                const double width = mix(start.halfwidth, end.halfwidth, t);
                const WorldXY center = add(curve[i], mul(sub(curve[i + 1], curve[i]), along));
                for (double offset : {0.0, -0.5, 0.5}) {
                    points.push_back(add(center, mul(transverse, width * offset)));
                }
            }
        }
        if (points.size() != (curve.size() - 2U) * 6U) continue;
        std::vector<WorldXY> wrapped;
        wrapped.reserve(points.size());
        for (WorldXY p : points) wrapped.push_back(wrap(p, period));
        auto rows = sampler(wrapped);
        if (!rows) throw rows.error();
        require(rows->size() == points.size(), FailureCode::NativeSampleMismatch, "course probe result count differs");

        double cost = 0.0;
        bool supported = true;
        for (size_t i = 0; i < points.size(); ++i) {
            const NativePointSample& native = (*rows)[i];
            require(validSample(native) && native.xy.x_m == wrapped[i].x_m && native.xy.y_m == wrapped[i].y_m,
                    FailureCode::NativeSampleMismatch, "course probe XY, order or value differs");
            const auto [distance, t] = nearest(points[i], curve);
            const double valley = mix(start.valley, end.valley, t);
            const double grade = mix(start.grade, end.grade, t);
            const double width = mix(start.halfwidth, end.halfwidth, t);
            const double rise = mix(start.valley_rise, end.valley_rise, t);
            const double stage = mix(reach.stage0_m, reach.stage1_m, t);
            const Target target{distance, width, mix(start.depth, end.depth, t), grade, false};
            const double support = smooth(1.0 - distance / valley);
            const double bed = applyBed(
                field, native, wrapped[i], grade + rise * std::pow(distance / valley, 2.0), support, std::span{&target, 1U});
            if (distance > width || native.foundation_m <= native.standing_stage_m
                || stage - bed < 0.25) {
                supported = false;
                break;
            }
            // Penalize cuts and fills against the natural reference section.
            const double section = grade - target.depth * (1.0 - std::pow(distance / width, 2.0));
            const double correction = native.foundation_m - section;
            cost += correction * correction;
        }
        const double score = cost / points.size() + 0.002 * std::pow(amplitude * scale, 2.0);
        if (supported && score < best_score) {
            best_score = score;
            best = std::move(curve);
        }
    }
    course.xy = std::move(best);
}

void indexCourses(PreparedRiverField& field)
{
    field.buckets.clear();
    const double period = field.identity.period_m;
    for (size_t id = 0; id < field.courses.size(); ++id) {
        auto const& course = field.courses[id];
        const double support = std::max(field.nodes.at(course.start).valley,
            field.nodes.at(course.target).valley);
        for (int sx : {-1, 0, 1}) {
            for (int sy : {-1, 0, 1}) {
                Image image{id, {sx * period, sy * period}, {}};
                image.xy.reserve(course.xy.size());
                WorldXY lower{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
                WorldXY upper{-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
                for (WorldXY p : course.xy) {
                    p = add(p, image.offset);
                    image.xy.push_back(p);
                    lower.x_m = std::min(lower.x_m, p.x_m);
                    lower.y_m = std::min(lower.y_m, p.y_m);
                    upper.x_m = std::max(upper.x_m, p.x_m);
                    upper.y_m = std::max(upper.y_m, p.y_m);
                }
                lower = sub(lower, {support, support});
                upper = add(upper, {support, support});
                if (upper.x_m < 0 || upper.y_m < 0 || lower.x_m >= period || lower.y_m >= period) {
                    continue;
                }
                const int ix0 = static_cast<int>(std::floor(std::max(0.0, lower.x_m) / field.bucket_m));
                const int iy0 = static_cast<int>(std::floor(std::max(0.0, lower.y_m) / field.bucket_m));
                const int ix1 = static_cast<int>(std::floor(std::min(period - 1e-8, upper.x_m) / field.bucket_m));
                const int iy1 = static_cast<int>(std::floor(std::min(period - 1e-8, upper.y_m) / field.bucket_m));
                for (int ix = ix0; ix <= ix1; ++ix) {
                    for (int iy = iy0; iy <= iy1; ++iy) {
                        field.buckets[{ix, iy}].push_back(image);
                    }
                }
            }
        }
    }
}

// Measure what the joined native field will carve and which stage the inland
// water query will select. Probes lie inside the inner channel, where the fine
// terrain residual is attenuated to zero.
struct JoinedProbe
{
    double bed;
    double clearance;
};

JoinedProbe joinedProbe(PreparedRiverField const& field, NativePointSample const& native)
{
    const auto result = sampleOne(field, native, true);
    const double bed = result.bed.bed_m;
    if (bed <= native.standing_stage_m) return {bed, 0.0};
    double stage = bed;
    double discharge = 0.0;
    size_t source_index = field.reaches.size();
    for (auto const& match : result.containing_courses) {
        if (match.receiver_extension) continue;
        const auto& reach = field.reaches[match.reach_index];
        const double candidate_stage = reach.stage0_m
            + match.course_fraction * (reach.stage1_m - reach.stage0_m);
        if (candidate_stage <= bed) continue;
        if (reach.discharge_m3_s > discharge
            || (reach.discharge_m3_s == discharge && match.source_reach_index < source_index)) {
            stage = candidate_stage;
            discharge = reach.discharge_m3_s;
            source_index = match.source_reach_index;
        }
    }
    return {bed, stage - bed};
}

std::vector<WorldXY> innerChannelProbes(PreparedRiverField const& field, Course const& course)
{
    const Node& start = field.nodes.at(course.start);
    const Node& end = field.nodes.at(course.target);
    std::vector<WorldXY> result;
    result.reserve((course.xy.size() - 1U) * 6U);
    for (size_t i = 0; i + 1 < course.xy.size(); ++i) {
        const WorldXY edge = sub(course.xy[i + 1], course.xy[i]);
        const double norm = length(edge);
        require(norm > 0.0, FailureCode::InvalidInput, "joined course has a degenerate segment");
        const WorldXY normal{-edge.y_m / norm, edge.x_m / norm};
        for (double along : {0.0, 0.5}) {
            const double t = (i + along) / (course.xy.size() - 1);
            const double width = start.halfwidth * (1.0 - t) + end.halfwidth * t;
            const WorldXY center = add(course.xy[i], mul(edge, along));
            for (double side : {-0.5, 0.0, 0.5}) {
                result.push_back(wrap(add(center, mul(normal, width * side)), field.identity.period_m));
            }
        }
    }
    return result;
}

std::vector<NativePointSample> queryRouteProbes(NativeBatchSampler const& sampler, std::span<WorldXY const> points)
{
    auto rows = sampler(points);
    if (!rows) throw rows.error();
    require(rows->size() == points.size(), FailureCode::NativeSampleMismatch, "joined route probe count differs");
    for (size_t i = 0; i < points.size(); ++i) {
        const auto& r = (*rows)[i];
        require(validSample(r) && r.xy.x_m == points[i].x_m && r.xy.y_m == points[i].y_m,
                FailureCode::NativeSampleMismatch, "joined route probe XY, order or value differs");
    }
    return std::move(*rows);
}

// Include proposals that can contribute to the joined bed in either field,
// even when the clearance witness's own centerline did not move.
void findContributingProposals(PreparedRiverField const& field, WorldXY p,
    std::span<uint8_t const> changed, std::set<size_t>& reject)
{
    const auto cell = std::pair{static_cast<int>(std::floor(p.x_m / field.bucket_m)),
        static_cast<int>(std::floor(p.y_m / field.bucket_m))};
    const auto bucket = field.buckets.find(cell);
    if (bucket == field.buckets.end()) return;
    for (auto const& image : bucket->second) {
        const size_t id = image.course_index;
        if (!changed[id]) continue;
        auto const& course = field.courses[id];
        const Node& a = field.nodes.at(course.start);
        const Node& b = field.nodes.at(course.target);
        const auto [distance, t] = nearest(p, image.xy);
        const double valley = a.valley * (1.0 - t) + b.valley * t;
        const double width = a.halfwidth * (1.0 - t) + b.halfwidth * t;
        if (distance < std::max(valley, width * 1.25)) reject.insert(id);
    }
}

void admitJoinedRoutes(PreparedRiverField& field, NativeBatchSampler const& sampler)
{
    PreparedRiverField baseline = field;
    indexCourses(baseline);
    std::vector<uint8_t> changed(field.courses.size());
    for (size_t id = 0; id < field.courses.size(); ++id) {
        auto& course = field.courses[id];
        routeInterior(field, course, sampler);
        changed[id] = !std::equal(course.xy.begin(), course.xy.end(), baseline.courses[id].xy.begin(),
            [](WorldXY a, WorldXY b) { return a.x_m == b.x_m && a.y_m == b.y_m; });
    }

    constexpr double MINIMUM_CLEARANCE_M = 0.25;
    struct Checks {
        std::vector<NativePointSample> baseline;
        std::vector<NativePointSample> proposed;
        std::vector<uint8_t> baseline_pass;
        double maximum_bed_rise = 0.0;
    };
    std::vector<Checks> checks(field.courses.size());
    for (size_t id = 0; id < field.courses.size(); ++id) {
        if (field.courses[id].receiver_extension) continue;
        auto& check = checks[id];
        check.baseline = queryRouteProbes(sampler, innerChannelProbes(baseline, baseline.courses[id]));
        if (changed[id]) check.proposed = queryRouteProbes(sampler, innerChannelProbes(field, field.courses[id]));
        check.baseline_pass.reserve(check.baseline.size());
        std::optional<double> previous;
        for (size_t i = 0; i < check.baseline.size(); ++i) {
            const auto probe = joinedProbe(baseline, check.baseline[i]);
            check.baseline_pass.push_back(probe.clearance >= MINIMUM_CLEARANCE_M);
            if (i % 3U != 1U) continue;
            if (previous) check.maximum_bed_rise = std::max(check.maximum_bed_rise, probe.bed - *previous);
            previous = probe.bed;
        }
    }

    for (int pass = 0; pass < 3; ++pass) {
        indexCourses(field);
        std::set<size_t> reject;
        size_t losses = 0;
        size_t uphill = 0;
        for (size_t id = 0; id < field.courses.size(); ++id) {
            auto const& check = checks[id];
            const auto& probes = changed[id] ? check.proposed : check.baseline;
            std::optional<double> previous;
            auto const rejectAt = [&](WorldXY p) {
                if (changed[id]) reject.insert(id);
                findContributingProposals(field, p, changed, reject);
                findContributingProposals(baseline, p, changed, reject);
            };
            for (size_t i = 0; i < probes.size(); ++i) {
                const auto probe = joinedProbe(field, probes[i]);
                if (check.baseline_pass[i] && probe.clearance < MINIMUM_CLEARANCE_M) {
                    ++losses;
                    rejectAt(probes[i].xy);
                }
                if (i % 3U != 1U) continue;
                if (previous && probe.bed - *previous > check.maximum_bed_rise + 1e-7) {
                    ++uphill;
                    rejectAt(probes[i - 3U].xy);
                    rejectAt(probes[i].xy);
                }
                previous = probe.bed;
            }
        }
        std::clog << "JOINED_ROUTE pass=" << pass << " losses=" << losses
                  << " uphill=" << uphill << " rejected=" << reject.size() << " retained="
                  << std::accumulate(changed.begin(), changed.end(), size_t{0}) << '\n';
        if (losses == 0U && uphill == 0U) return;
        if (reject.empty()) break;
        for (size_t id : reject) {
            field.courses[id].xy = baseline.courses[id].xy;
            changed[id] = 0U;
        }
    }
    // Leave a complete indexed control graph if remaining interactions could
    // not be attributed and removed within the bounded validation passes.
    field.courses = std::move(baseline.courses);
    field.buckets = std::move(baseline.buckets);
}

} // namespace

std::expected<std::shared_ptr<PreparedRiverField const>, Failure> prepareRiverField(
    RiverFieldIdentity const& identity, std::span<RiverReach const> source_reaches,
    std::span<ProtectedContact const> source_contacts, NativeBatchSampler const& native_sampler,
    VisualCalibration const& calibration, ProbeOptions const& options)
{
    try {
        const double period = identity.period_m;
        require(finite(period) && std::nearbyint(period * 1e8) >= 1.0 &&
                    period * 1e8 < 9e18, FailureCode::InvalidInput,
                "period must be representable at 1e-8 m");
        for (auto const* name : {&identity.field_model_id, &identity.field_source_sha256,
            &identity.native_model_identity, &identity.native_source_manifest_sha256,
            &identity.native_prepared_state_identity, &identity.native_sampler_identity,
            &identity.reach_catalog_sha256, &identity.contact_catalog_sha256}) {
            require(!name->empty(), FailureCode::InvalidInput, "river field identity is incomplete");
        }
        for (double coefficient : {calibration.width_base_m, calibration.width_sqrt_m_per_sqrt_m3_s,
            calibration.depth_base_m, calibration.depth_sqrt_m_per_sqrt_m3_s,
            options.gradient_step_m, options.outlet_search_m,
            options.outlet_step_m, options.receiver_extension_m}) {
            require(finite(coefficient) && coefficient > 0, FailureCode::InvalidInput,
                    "calibration and probe distances must be positive and finite");
        }
        auto field = std::make_shared<PreparedRiverField>();
        field->identity = identity;
        field->calibration = calibration;
        field->options = options;
        field->reaches.assign(source_reaches.begin(), source_reaches.end());
        field->contacts.assign(source_contacts.begin(), source_contacts.end());
        require(!field->reaches.empty(), FailureCode::InvalidInput, "reach catalog is empty");
        for (auto const& r : field->reaches) {
            require(finite(r.start_xy) && finite(r.delta_xy) && finite(r.grade0_m) &&
                        finite(r.grade1_m) && finite(r.discharge_m3_s) && r.discharge_m3_s > 0 &&
                        length(r.delta_xy) > 0, FailureCode::InvalidInput, "invalid reach");
        }
        field->source_reach_indices.resize(source_reaches.size());
        std::iota(field->source_reach_indices.begin(), field->source_reach_indices.end(), 0U);
        std::sort(field->source_reach_indices.begin(), field->source_reach_indices.end(), [&](size_t a_id, size_t b_id) {
            auto const& a = source_reaches[a_id];
            auto const& b = source_reaches[b_id];
            return std::tie(a.start_vertex, a.target_vertex, a.start_xy.x_m, a.start_xy.y_m,
                            a.delta_xy.x_m, a.delta_xy.y_m, a.grade0_m, a.grade1_m,
                            a.discharge_m3_s, a.shore_truncated) <
                   std::tie(b.start_vertex, b.target_vertex, b.start_xy.x_m, b.start_xy.y_m,
                            b.delta_xy.x_m, b.delta_xy.y_m, b.grade0_m, b.grade1_m,
                            b.discharge_m3_s, b.shore_truncated);
        });
        for (size_t i = 0U; i < field->reaches.size(); ++i) {
            field->reaches[i] = source_reaches[field->source_reach_indices[i]];
        }
        for (size_t i = 1; i < field->reaches.size(); ++i) {
            require(!(field->reaches[i].start_vertex == field->reaches[i-1].start_vertex &&
                      field->reaches[i].target_vertex == field->reaches[i-1].target_vertex &&
                      field->reaches[i].start_xy.x_m == field->reaches[i-1].start_xy.x_m &&
                      field->reaches[i].start_xy.y_m == field->reaches[i-1].start_xy.y_m &&
                      field->reaches[i].delta_xy.x_m == field->reaches[i-1].delta_xy.x_m &&
                      field->reaches[i].delta_xy.y_m == field->reaches[i-1].delta_xy.y_m &&
                      field->reaches[i].grade0_m == field->reaches[i-1].grade0_m &&
                      field->reaches[i].grade1_m == field->reaches[i-1].grade1_m &&
                      field->reaches[i].discharge_m3_s == field->reaches[i-1].discharge_m3_s &&
                      field->reaches[i].shore_truncated == field->reaches[i-1].shore_truncated),
                    FailureCode::InvalidInput, "duplicate reach");
        }
        for (auto const& c : field->contacts) {
            require(finite(c.xy) && finite(c.radius_m) && c.radius_m > 0,
                    FailureCode::InvalidInput, "invalid contact");
        }
        std::sort(field->contacts.begin(), field->contacts.end(), [](auto const& a, auto const& b) {
            return std::tie(a.xy.x_m, a.xy.y_m, a.radius_m) <
                   std::tie(b.xy.x_m, b.xy.y_m, b.radius_m);
        });

        std::map<NodeKey, WorldXY> node_xy;
        std::map<size_t, std::vector<WorldXY>> tracks;
        auto register_node = [&](NodeKey id, WorldXY xy) {
            auto [it, inserted] = node_xy.emplace(id, xy);
            require(inserted || length(sub(it->second, xy)) <= 1e-7,
                    FailureCode::InvalidInput, "shared graph node XY differs");
        };
        const double track_count = std::ceil(
            (options.outlet_search_m + options.receiver_extension_m) / options.outlet_step_m);
        require(finite(track_count) && track_count < 1'000'000, FailureCode::InvalidInput,
                "shore probe budget exceeded");
        const int anchor_steps = static_cast<int>(std::floor(options.outlet_search_m / options.outlet_step_m));
        const int extension_steps = static_cast<int>(std::ceil(options.receiver_extension_m / options.outlet_step_m));
        const int track_steps = static_cast<int>(track_count);
        for (size_t i = 0; i < field->reaches.size(); ++i) {
            auto const& r = field->reaches[i];
            const WorldXY start = point(key(r.start_xy, period));
            register_node({0, r.start_vertex}, start);
            const WorldXY end = add(start, r.delta_xy);
            if (r.shore_truncated) {
                const WorldXY direction = mul(r.delta_xy, 1.0 / length(r.delta_xy));
                auto& track = tracks[i];
                track.reserve(track_steps + 1);
                for (int step = 0; step <= track_steps; ++step) {
                    track.push_back(add(end, mul(direction, step * options.outlet_step_m)));
                }
            } else {
                register_node({0, r.target_vertex}, point(key(end, period)));
            }
        }
        std::set<Key> probe_keys;
        auto add_probe = [&](WorldXY xy) {
            probe_keys.insert(key(xy, period));
            const double step = options.gradient_step_m;
            for (WorldXY offset : {WorldXY{step, 0}, WorldXY{-step, 0},
                WorldXY{0, step}, WorldXY{0, -step}}) {
                probe_keys.insert(key(add(xy, offset), period));
            }
        };
        for (auto const& [id, xy] : node_xy) {
            add_probe(xy);
        }
        for (auto const& [i, track] : tracks) {
            for (WorldXY xy : track) {
                add_probe(xy);
            }
        }
        std::vector<WorldXY> probes;
        probes.reserve(probe_keys.size());
        for (Key k : probe_keys) {
            probes.push_back(point(k));
        }
        auto queried = native_sampler(probes);
        if (!queried) {
            return std::unexpected(queried.error());
        }
        require(queried->size() == probes.size(), FailureCode::NativeSampleMismatch,
                "native probe result count differs");
        std::map<Key, NativePointSample> samples;
        auto probe_it = probe_keys.begin();
        for (size_t i = 0; i < probes.size(); ++i, ++probe_it) {
            auto const& native = (*queried)[i];
            require(validSample(native) && native.xy.x_m == probes[i].x_m &&
                        native.xy.y_m == probes[i].y_m,
                    FailureCode::NativeSampleMismatch, "native probe XY, order or value differs");
            samples.emplace(*probe_it, native);
        }
        auto sample = [&](WorldXY xy) -> NativePointSample const& { return samples.at(key(xy, period)); };
        auto gradient = [&](WorldXY xy) {
            const double step = options.gradient_step_m;
            return WorldXY{(sample({xy.x_m + step, xy.y_m}).foundation_m -
                            sample({xy.x_m - step, xy.y_m}).foundation_m) / (2 * step),
                           (sample({xy.x_m, xy.y_m + step}).foundation_m -
                            sample({xy.x_m, xy.y_m - step}).foundation_m) / (2 * step)};
        };
        std::map<size_t, Outlet> outlets;
        for (auto const& [number, track] : tracks) {
            int anchor = -1;
            for (int step = 0; step <= anchor_steps; ++step) {
                auto const& native = sample(track[step]);
                if (native.foundation_m < native.standing_stage_m) { anchor = step; break; }
            }
            if (anchor < 0) {
                fail(FailureCode::OutletUnresolved, "shore reach " + std::to_string(number) +
                     " has no native wet owner within bounded search");
            }
            const int end = anchor + extension_steps;
            require(end < static_cast<int>(track.size()), FailureCode::OutletUnresolved,
                    "shore receiver extension exceeds probe track");
            const auto owner = sample(track[anchor]).standing_owner;
            for (int step = anchor; step <= end; ++step) {
                auto const& native = sample(track[step]);
                require(native.foundation_m < native.standing_stage_m && native.standing_owner == owner,
                        FailureCode::OutletUnresolved, "shore extension leaves one native water owner");
            }
            outlets[number] = {track[anchor], track[end], owner};
        }

        std::map<NodeKey, std::vector<Observation>> observations;
        std::vector<std::pair<NodeKey, NodeKey>> endpoints;
        for (size_t i = 0; i < field->reaches.size(); ++i) {
            auto const& r = field->reaches[i];
            const NodeKey start_key{0, r.start_vertex};
            const NodeKey target_key = r.shore_truncated ? NodeKey{1, static_cast<int64_t>(i)} :
                NodeKey{0, r.target_vertex};
            const WorldXY start = node_xy.at(start_key);
            const WorldXY target = r.shore_truncated ? outlets.at(i).anchor : node_xy.at(target_key);
            const double distance = length(r.delta_xy);
            const WorldXY direction = mul(r.delta_xy, 1.0 / distance);
            const double slope = (r.grade0_m - r.grade1_m) / distance;
            observations[start_key].push_back({start, r.grade0_m, r.discharge_m3_s, direction, slope});
            observations[target_key].push_back({target, r.grade1_m, r.discharge_m3_s, direction, slope});
            endpoints.emplace_back(start_key, target_key);
        }
        for (auto const& [id, rows] : observations) {
            for (auto const& row : rows) {
                require(length(sub(row.xy, rows.front().xy)) <= 1e-7 &&
                            std::abs(row.grade - rows.front().grade) <= 1e-7,
                        FailureCode::InvalidInput, "shared junction position or grade differs");
            }
            WorldXY tangent{0, 0};
            double turn = 0;
            double slope = 0;
            double max_q = 0;
            for (auto const& a : rows) {
                tangent = add(tangent, mul(a.direction, a.q));
                slope += a.slope;
                max_q = std::max(max_q, a.q);
                for (auto const& b : rows) {
                    turn = std::max(turn, 1.0 - dot(a.direction, b.direction));
                }
            }
            require(length(tangent) > 1e-12, FailureCode::InvalidInput, "junction tangent undefined");
            tangent = mul(tangent, 1.0 / length(tangent));
            slope /= rows.size();
            const WorldXY normal{-tangent.y_m, tangent.x_m};
            const double cross_slope = dot(gradient(rows.front().xy), normal);
            const double root_q = std::sqrt(max_q);
            const double halfwidth = (calibration.width_base_m +
                    calibration.width_sqrt_m_per_sqrt_m3_s * root_q) *
                    clip(std::pow(0.08 / std::max(slope, 0.005), 0.12), 0.78, 1.22) *
                    (1.0 + 0.12 * turn);
            const double valley = (12.0 + 3.5 * halfwidth) /
                (1.0 + 0.25 * std::min(std::abs(cross_slope) / 0.15, 1.0));
            const double shift = rows.size() == 2 ? clip(-12.0 * cross_slope / 0.08, -12.0, 12.0) : 0.0;
            field->nodes[id] = {rows.front().xy, add(rows.front().xy, mul(normal, shift)), tangent,
                rows.front().grade, halfwidth,
            calibration.depth_base_m + calibration.depth_sqrt_m_per_sqrt_m3_s * root_q,
                valley, std::min(3.5, 1.2 + 0.5 * valley * std::abs(cross_slope))};
        }
        for (size_t i = 0; i < field->reaches.size(); ++i) {
            auto const& r = field->reaches[i];
            const auto [start_key, target_key] = endpoints[i];
            const Node& start = field->nodes.at(start_key);
            const Node& target = field->nodes.at(target_key);
            const WorldXY a = start.course_xy;
            WorldXY b = target.course_xy;
            const WorldXY expected = add(start.xy, r.delta_xy);
            b.x_m += period * std::nearbyint((expected.x_m - b.x_m) / period);
            b.y_m += period * std::nearbyint((expected.y_m - b.y_m) / period);
            const double distance = length(sub(b, a));
            require(finite(distance) && distance / 4.0 < 1'000'000,
                    FailureCode::InvalidInput, "course sampling budget exceeded");
            const int count = std::max(2, static_cast<int>(std::ceil(distance / 4.0)) + 1);
            const WorldXY c0 = add(a, mul(start.tangent, distance * 0.33));
            const WorldXY c1 = sub(b, mul(target.tangent, distance * 0.33));
            int32_t const receiver_owner = r.shore_truncated ? outlets.at(i).owner : -1;
            Course course{i, {}, start_key, target_key, r.shore_truncated, false, receiver_owner};
            course.xy.reserve(count);
            for (int j = 0; j < count; ++j) {
                const double t = static_cast<double>(j) / (count - 1);
                course.xy.push_back(add(add(mul(a, std::pow(1-t, 3)), mul(c0, 3 * std::pow(1-t, 2) * t)),
                    add(mul(c1, 3 * (1-t) * t*t), mul(b, t*t*t))));
            }
            field->courses.push_back(std::move(course));
            if (r.shore_truncated) {
                const WorldXY extension = add(outlets.at(i).extension, sub(b, target.course_xy));
                require(finite(length(sub(extension, b))) && length(sub(extension, b)) / 4.0 < 1'000'000,
                        FailureCode::InvalidInput, "receiver course sampling budget exceeded");
                const int n = std::max(2, static_cast<int>(std::ceil(length(sub(extension, b)) / 4.0)) + 1);
                Course tail{i, {}, target_key, target_key, true, true, receiver_owner};
                tail.xy.reserve(n);
                for (int j = 0; j < n; ++j) {
                    tail.xy.push_back(add(b, mul(sub(extension, b), static_cast<double>(j) / (n - 1))));
                }
                field->courses.push_back(std::move(tail));
            }
        }
        admitJoinedRoutes(*field, native_sampler);
        return std::shared_ptr<PreparedRiverField const>(std::move(field));
    } catch (Failure const& error) {
        return std::unexpected(error);
    } catch (std::exception const& error) {
        return std::unexpected(Failure{FailureCode::NativeQueryFailed, error.what()});
    }
}

std::vector<RiverMapCourse> exportRiverMapCourses(PreparedRiverField const& field)
{
    std::vector<RiverMapCourse> result;
    result.reserve(field.courses.size());
    for (size_t id = 0U; id < field.courses.size(); ++id)
    {
        auto const& course = field.courses[id];
        auto const& reach = field.reaches[course.reach_index];
        auto const& start = field.nodes.at(course.start);
        auto const& end = field.nodes.at(course.target);
        RiverMapCourse output{id, field.source_reach_indices[course.reach_index],
            reach.start_vertex, reach.target_vertex, course.receiver_extension, course.receiver_owner, {}};
        output.points.reserve(course.xy.size());
        for (size_t i = 0U; i < course.xy.size(); ++i)
        {
            double const fraction = static_cast<double>(i) / static_cast<double>(course.xy.size() - 1U);
            output.points.push_back({course.xy[i], start.halfwidth * (1.0 - fraction) + end.halfwidth * fraction});
        }
        result.push_back(std::move(output));
    }
    return result;
}

std::expected<std::vector<RiverChannelSample>, Failure> sampleRiverChannel(
    PreparedRiverField const& field, std::span<NativePointSample const> native_samples)
{
    try {
        std::vector<RiverChannelSample> result;
        result.reserve(native_samples.size());
        for (auto const& native : native_samples) {
            require(validSample(native), FailureCode::NativeSampleMismatch, "invalid native sample");
            result.push_back(sampleOne(field, native, true));
        }
        return result;
    } catch (Failure const& error) {
        return std::unexpected(error);
    }
}

std::expected<std::vector<RiverBedSample>, Failure> sampleRiverBed(
    PreparedRiverField const& field, std::span<NativePointSample const> native_samples)
{
    try {
        std::vector<RiverBedSample> result;
        result.reserve(native_samples.size());
        for (auto const& native : native_samples) {
            require(validSample(native), FailureCode::NativeSampleMismatch, "invalid native sample");
            result.push_back(sampleOne(field, native).bed);
        }
        return result;
    } catch (Failure const& error) {
        return std::unexpected(error);
    }
}

namespace
{
bool clipSegment(WorldXY a, WorldXY b, WorldXY lower, WorldXY upper, WorldXY& out_a, WorldXY& out_b)
{
    double enter = 0.0;
    double leave = 1.0;
    const WorldXY d = sub(b, a);
    const std::array<std::pair<double, double>, 4> edges{{
        {-d.x_m, a.x_m - lower.x_m}, {d.x_m, upper.x_m - a.x_m},
        {-d.y_m, a.y_m - lower.y_m}, {d.y_m, upper.y_m - a.y_m}
    }};
    for (auto const& [p, q] : edges) {
        if (p == 0.0) {
            if (q < 0.0) {
                return false;
            }
            continue;
        }
        const double ratio = q / p;
        if (p < 0.0) {
            enter = std::max(enter, ratio);
        }
        else {
            leave = std::min(leave, ratio);
        }
        if (enter > leave) {
            return false;
        }
    }
    out_a = add(a, mul(d, enter));
    out_b = add(a, mul(d, leave));
    return length(sub(out_b, out_a)) > 1e-10;
}

std::vector<WorldXY> densify(std::vector<WorldXY> const& raw, double limit)
{
    std::vector<WorldXY> dense;
    if (raw.empty()) {
        return dense;
    }
    dense.push_back(raw.front());
    for (size_t i = 1; i < raw.size(); ++i) {
        const WorldXY d = sub(raw[i], raw[i-1]);
        const double needed = std::ceil(length(d) / limit);
        require(finite(needed) && needed <= 1'000'000 &&
                    dense.size() + static_cast<size_t>(needed) <= 1'000'000,
                FailureCode::ConstraintBudgetExceeded, "constraint subdivision budget exceeded");
        const int segments = std::max(1, static_cast<int>(needed));
        for (int j = 1; j <= segments; ++j) {
            dense.push_back(add(raw[i-1], mul(d, static_cast<double>(j) / segments)));
        }
    }
    return dense;
}

std::string nodeName(NodeKey key)
{
    return std::string(key.first == 0 ? "vertex:" : "shore:") + std::to_string(key.second);
}
} // namespace

std::expected<RiverConstraintExport, Failure> exportRiverConstraints(
    PreparedRiverField const& field, ConstraintRequest const& request,
    NativeBatchSampler const& native_sampler)
{
    try {
        const double period = field.identity.period_m;
        require(finite(request.crop_min) && finite(request.crop_max) &&
                    request.crop_min.x_m < request.crop_max.x_m &&
                    request.crop_min.y_m < request.crop_max.y_m &&
                    finite(request.base_spacing_m) && request.base_spacing_m > 0 &&
                    finite(request.max_xy_spacing_m) && request.max_xy_spacing_m > 0 &&
                    request.max_xy_spacing_m <= 1.0,
                FailureCode::InvalidInput, "invalid crop or XY spacing request");
        if (request.consumer == ConsumerKind::Columns1m) {
            require(request.base_spacing_m == 1.0, FailureCode::InvalidInput,
                    "1m columns require 1m base spacing");
        }
        RiverConstraintExport result{field.identity, field.calibration, field.options, request,
            {}, 0.0, 0.0, 0.0, 0.0};
        std::vector<WorldXY> native_xy;
        std::vector<std::vector<size_t>> vertex_indexes;
        // Bucket image bounds include the maximum valley radius, which covers
        // every rail and transverse span. Keep the periodic image offset as well
        // as the course ID so unrelated images never enter clipping or sampling.
        using ImageShift = std::pair<int, int>;
        std::vector<std::set<ImageShift>> candidates(field.courses.size());
        struct CropRange { double lower, upper; int period_index; };
        auto split = [period](double lower, double upper) -> std::vector<CropRange> {
            const double first = std::floor(lower / period);
            const double last = std::floor(upper / period);
            const double bound = static_cast<double>(std::numeric_limits<int>::max() - 3);
            if (!finite(first) || !finite(last) || first <= -bound || last >= bound || last - first > 10'000) {
                return {};
            }
            std::vector<CropRange> ranges;
            for (int index = static_cast<int>(first); index <= static_cast<int>(last); ++index) {
                const double origin = index * period;
                ranges.push_back({std::max(0.0, lower - origin),
                    std::min(period, upper - origin), index});
            }
            return ranges;
        };
        const auto x_ranges = split(request.crop_min.x_m, request.crop_max.x_m);
        const auto y_ranges = split(request.crop_min.y_m, request.crop_max.y_m);
        const bool brute_fallback = x_ranges.empty() || y_ranges.empty() ||
            x_ranges.size() * y_ranges.size() > 10'000;
        if (!brute_fallback) {
            const int max_bucket = static_cast<int>(std::floor((period - 1e-8) / field.bucket_m));
            for (auto const& xr : x_ranges) {
                for (auto const& yr : y_ranges) {
                    const int x0 = std::min(max_bucket, static_cast<int>(std::floor(xr.lower / field.bucket_m)));
                    const int x1 = std::min(max_bucket, static_cast<int>(std::floor(xr.upper / field.bucket_m)));
                    const int y0 = std::min(max_bucket, static_cast<int>(std::floor(yr.lower / field.bucket_m)));
                    const int y1 = std::min(max_bucket, static_cast<int>(std::floor(yr.upper / field.bucket_m)));
                    auto it = field.buckets.lower_bound({x0, std::numeric_limits<int>::min()});
                    for (; it != field.buckets.end() && it->first.first <= x1; ++it) {
                        if (it->first.second < y0 || it->first.second > y1) {
                            continue;
                        }
                        for (auto const& image : it->second) {
                            const int sx = xr.period_index
                            + static_cast<int>(std::nearbyint(image.offset.x_m / period));
                            const int sy = yr.period_index
                            + static_cast<int>(std::nearbyint(image.offset.y_m / period));
                            candidates[image.course_index].emplace(sx, sy);
                        }
                    }
                }
            }
        }
        auto emit = [&](Course const& course, size_t course_index, FeatureRole role,
                        size_t feature_ordinal, std::vector<WorldXY> const& raw,
                        double halfwidth) {
            const double limit = std::min({request.base_spacing_m, halfwidth / 4.0,
                request.max_xy_spacing_m});
            auto dense = densify(raw, limit);
            if (dense.size() < 2) {
                return;
            }
            WorldXY minimum{std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::infinity()};
            WorldXY maximum{-minimum.x_m, -minimum.y_m};
            for (WorldXY xy : dense) {
                minimum.x_m = std::min(minimum.x_m, xy.x_m);
                minimum.y_m = std::min(minimum.y_m, xy.y_m);
                maximum.x_m = std::max(maximum.x_m, xy.x_m);
                maximum.y_m = std::max(maximum.y_m, xy.y_m);
            }
            const double x0 = std::floor((request.crop_min.x_m - maximum.x_m) / period);
            const double x1 = std::ceil((request.crop_max.x_m - minimum.x_m) / period);
            const double y0 = std::floor((request.crop_min.y_m - maximum.y_m) / period);
            const double y1 = std::ceil((request.crop_max.y_m - minimum.y_m) / period);
            const double image_count = (x1 - x0 + 1.0) * (y1 - y0 + 1.0);
            const double index_bound = static_cast<double>(std::numeric_limits<int>::max() - 1);
            require(finite(image_count) && image_count > 0 && image_count <= 10'000 &&
                        x0 > -index_bound && x1 < index_bound &&
                        y0 > -index_bound && y1 < index_bound,
                    FailureCode::ConstraintBudgetExceeded, "too many periodic crop images");
            const int sx0 = static_cast<int>(x0);
            const int sx1 = static_cast<int>(x1);
            const int sy0 = static_cast<int>(y0);
            const int sy1 = static_cast<int>(y1);
            auto emitImage = [&](int sx, int sy) {
                const WorldXY shift{sx * period, sy * period};
                std::vector<WorldXY> piece;
                size_t ordinal = 0;
                auto finish = [&] {
                    if (piece.size() < 2) { piece.clear(); return; }
                    RiverConstraintPolyline polyline;
                    polyline.feature_id = std::to_string(course.reach_index) + ":" +
                        std::to_string(course_index) + ":" + std::to_string(sx) + ":" +
                        std::to_string(sy) + ":" + std::to_string(static_cast<int>(role)) +
                        ":" + std::to_string(feature_ordinal) + ":" +
                        std::to_string(ordinal++);
                    polyline.reach_index = course.reach_index;
                    polyline.course_index = course_index;
                    polyline.start_node_key = nodeName(course.start);
                    polyline.target_node_key = nodeName(course.target);
                    polyline.receiver_extension = course.receiver_extension;
                    polyline.role = role;
                    polyline.periodic_image_offset = shift;
                    std::vector<size_t> indexes;
                    for (WorldXY xy : piece) {
                        indexes.push_back(native_xy.size());
                        native_xy.push_back(xy);
                        polyline.vertices.push_back({});
                    }
                    vertex_indexes.push_back(std::move(indexes));
                    result.polylines.push_back(std::move(polyline));
                    piece.clear();
                };
                for (size_t i = 1; i < dense.size(); ++i) {
                    WorldXY a, b;
                    if (!clipSegment(add(dense[i-1], shift), add(dense[i], shift),
                        request.crop_min, request.crop_max, a, b)) {
                        finish();
                        continue;
                    }
                    if (!piece.empty() && length(sub(piece.back(), a)) > 1e-8) {
                        finish();
                    }
                    if (piece.empty()) {
                        piece.push_back(a);
                    }
                    if (length(sub(piece.back(), b)) > 1e-10) {
                        piece.push_back(b);
                    }
                }
                finish();
            };
            if (brute_fallback) {
                for (int sx = sx0; sx <= sx1; ++sx) {
                    for (int sy = sy0; sy <= sy1; ++sy) {
                        emitImage(sx, sy);
                    }
                }
            } else {
                for (auto [sx, sy] : candidates[course_index]) {
                    if (sx >= sx0 && sx <= sx1 && sy >= sy0 && sy <= sy1) {
                        emitImage(sx, sy);
                    }
                }
            }
        };

        constexpr std::array<FeatureRole, 7> roles{
            FeatureRole::ValleyLeft, FeatureRole::ChannelBankLeft,
            FeatureRole::ChannelInnerLeft, FeatureRole::Centreline,
            FeatureRole::ChannelInnerRight, FeatureRole::ChannelBankRight,
            FeatureRole::ValleyRight
        };
        for (size_t course_index = 0; course_index < field.courses.size(); ++course_index) {
            if (!brute_fallback && candidates[course_index].empty()) {
                continue;
            }
            const Course& course = field.courses[course_index];
            const Node& start = field.nodes.at(course.start);
            const Node& end = field.nodes.at(course.target);
            const size_t count = course.xy.size();
            std::array<std::vector<WorldXY>, 7> rails;
            std::vector<WorldXY> normals;
            std::vector<double> widths, valleys;
            const double reach_min_width = std::min(start.halfwidth, end.halfwidth);
            normals.reserve(count);
            widths.reserve(count);
            valleys.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                const WorldXY tangent = i == 0 ? sub(course.xy[1], course.xy[0]) :
                    i+1 == count ? sub(course.xy[i], course.xy[i-1]) :
                    sub(course.xy[i+1], course.xy[i-1]);
                require(length(tangent) > 1e-12, FailureCode::InvalidInput,
                        "course tangent undefined at constraint sample");
                const WorldXY normal{-tangent.y_m / length(tangent), tangent.x_m / length(tangent)};
                const double t = static_cast<double>(i) / (count - 1);
                const double width = start.halfwidth*(1-t) + end.halfwidth*t;
                const double valley = start.valley*(1-t) + end.valley*t;
                normals.push_back(normal);
                widths.push_back(width);
                valleys.push_back(valley);
                const std::array<double, 7> offsets{-valley, -width, -0.65*width, 0.0,
                    0.65*width, width, valley};
                for (size_t j = 0; j < roles.size(); ++j) {
                    rails[j].push_back(add(course.xy[i], mul(normal, offsets[j])));
                }
            }
            for (size_t j = 0; j < roles.size(); ++j) {
                emit(course, course_index, roles[j], 0, rails[j], reach_min_width);
            }
            for (size_t i = 0; i < count; i += 8) {
                std::vector<WorldXY> transverse;
                for (double offset : {-valleys[i], -widths[i], 0.0, widths[i], valleys[i]}) {
                    transverse.push_back(add(course.xy[i], mul(normals[i], offset)));
                }
                emit(course, course_index, FeatureRole::Transverse, i, transverse, widths[i]);
            }
        }
        if (result.polylines.empty()) {
            return result;
        }
        std::vector<std::vector<size_t>> midpoint_indexes;
        for (auto const& polyline : result.polylines) {
            std::vector<size_t> indexes;
            for (size_t i = 1; i < polyline.vertices.size(); ++i) {
                auto const& vertices = vertex_indexes[midpoint_indexes.size()];
                const WorldXY a = native_xy[vertices[i-1]];
                const WorldXY b = native_xy[vertices[i]];
                const double xy_length = length(sub(b, a));
                require(xy_length <= request.max_xy_spacing_m + 1e-6 &&
                            xy_length <= request.base_spacing_m + 1e-6,
                        FailureCode::ConstraintBudgetExceeded, "constraint XY spacing exceeded");
                result.max_xy_segment_m = std::max(result.max_xy_segment_m, xy_length);
                indexes.push_back(native_xy.size());
                native_xy.push_back(mul(add(a, b), 0.5));
            }
            midpoint_indexes.push_back(std::move(indexes));
        }
        auto queried = native_sampler(native_xy);
        if (!queried) {
            return std::unexpected(queried.error());
        }
        require(queried->size() == native_xy.size(), FailureCode::NativeSampleMismatch,
                "constraint native result count differs");
        for (size_t i = 0; i < native_xy.size(); ++i) {
            require(validSample((*queried)[i]) && (*queried)[i].xy.x_m == native_xy[i].x_m &&
                        (*queried)[i].xy.y_m == native_xy[i].y_m,
                    FailureCode::NativeSampleMismatch, "constraint native XY, order or value differs");
        }
        auto beds = sampleRiverBed(field, *queried);
        if (!beds) {
            return std::unexpected(beds.error());
        }
        for (size_t p = 0; p < result.polylines.size(); ++p) {
            auto& polyline = result.polylines[p];
            for (size_t i = 0; i < polyline.vertices.size(); ++i) {
                const size_t index = vertex_indexes[p][i];
                polyline.vertices[i] = {(*queried)[index], (*beds)[index].bed_m,
                    (*beds)[index].signed_delta_m,
                    (*beds)[index].corridor_support};
            }
            for (size_t i = 1; i < polyline.vertices.size(); ++i) {
                const auto& a = polyline.vertices[i-1];
                const auto& b = polyline.vertices[i];
                const size_t middle = midpoint_indexes[p][i-1];
                const double dx = b.native.xy.x_m - a.native.xy.x_m;
                const double dy = b.native.xy.y_m - a.native.xy.y_m;
                const double dz = b.bed_m - a.bed_m;
                result.max_xyz_chord_m = std::max(result.max_xyz_chord_m,
                    std::sqrt(dx*dx + dy*dy + dz*dz));
                result.max_native_midpoint_height_error_m = std::max(
                    result.max_native_midpoint_height_error_m,
                    std::abs((*queried)[middle].foundation_m -
                             (a.native.foundation_m + b.native.foundation_m)/2.0));
                result.max_bed_midpoint_height_error_m = std::max(
                    result.max_bed_midpoint_height_error_m,
                    std::abs((*beds)[middle].bed_m - (a.bed_m + b.bed_m)/2.0));
            }
        }
        return result;
    } catch (Failure const& error) {
        return std::unexpected(error);
    } catch (std::exception const& error) {
        return std::unexpected(Failure{FailureCode::NativeQueryFailed, error.what()});
    }
}

} // namespace river_field_candidate
