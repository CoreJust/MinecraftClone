#include "V21CourseGeometry.hpp"

#include "V9Numerics.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

#pragma STDC FP_CONTRACT OFF

namespace generation::detail
{
namespace
{
constexpr double WORLD = 65'536.0;
constexpr size_t COUNT = 256U * 256U;
using Point = V21FinePoint;
using Key = std::array<int64_t, 2U>;
struct CourseException final
{
    V21CourseFailure failure;
};
[[noreturn]] void fail(V21CourseFailure::Kind kind, char const *message, int32_t source = -1)
{
    throw CourseException{{kind, source, message}};
}
void check(std::stop_token token)
{
    if (token.stop_requested())
    {
        fail(V21CourseFailure::Kind::Cancelled, "V21 course generation cancelled");
    }
}
double wrap(double x)
{
    double const value = std::fmod(x, WORLD);
    return value < 0.0 ? value + WORLD : value;
}
double displacement(double x)
{
    return wrap(x + WORLD / 2.0) - WORLD / 2.0;
}
double norm(Point p)
{
    return std::sqrt(p[0] * p[0] + p[1] * p[1]);
}
Point add(Point a, Point b, double scale = 1.0)
{
    return {a[0] + scale * b[0], a[1] + scale * b[1]};
}
Point scale(Point a, double t)
{
    return {a[0] * t, a[1] * t};
}
Point divide(Point a, double t)
{
    return {a[0] / t, a[1] / t};
}
Key key(Point p)
{
    return {int64_t(std::nearbyint(wrap(p[0]) * 1e7)), int64_t(std::nearbyint(wrap(p[1]) * 1e7))};
}
std::vector<double> heights(V21CourseCallbacks::Heights const &callback, std::span<Point const> points)
{
    auto values = callback(points);
    if (values.size() != points.size() ||
        !std::all_of(values.begin(), values.end(), [](double v) { return std::isfinite(v); }))
    {
        fail(V21CourseFailure::Kind::InvalidInput, "finite course callback heights matching coordinates required");
    }
    return values;
}
std::vector<V21FineStanding> standing(V21CourseCallbacks const &callbacks, std::span<Point const> points)
{
    auto values = callbacks.standing(points);
    if (values.size() != points.size() ||
        !std::all_of(values.begin(), values.end(), [](auto v) { return std::isfinite(v.stage); }))
    {
        fail(V21CourseFailure::Kind::InvalidInput, "finite course standing stage/owner matching coordinates required");
    }
    return values;
}
double depth(double q, double width, double slope)
{
    auto const capacity = [&](double d)
    {
        double const area = width * .9 * d;
        double const perimeter = width * .8 + 2.0 * std::sqrt(d * d + (width * .1) * (width * .1));
        return area * std::pow(area / perimeter, 2.0 / 3.0) * std::sqrt(slope) / .04;
    };
    if (capacity(10.0) < q)
    {
        fail(V21CourseFailure::Kind::InvalidInput, "scene Q exceeds bounded 10m section depth");
    }
    double lo = 0.0, hi = 10.0;
    for (uint32_t i = 0U; i < 70U; ++i)
    {
        double const mid = (lo + hi) / 2.0;
        if (capacity(mid) < q)
        {
            lo = mid;
        }
        else
        {
            hi = mid;
        }
    }
    return (lo + hi) / 2.0;
}
Point curve(Point a, Point delta, Point m0, Point m1, Point shift, double u)
{
    Point result{};
    for (size_t i = 0U; i < 2U; ++i)
    {
        result[i] = a[i] + (3 * u * u - 2 * u * u * u) * delta[i] + (u * u * u - 2 * u * u + u) * m0[i] +
                    (u * u * u - u * u) * m1[i] + 16 * u * u * (1 - u) * (1 - u) * shift[i];
    }
    return result;
}
std::pair<Point, Point> derivatives(Point delta, Point m0, Point m1, Point shift, double u)
{
    Point first{}, second{};
    for (size_t i = 0U; i < 2U; ++i)
    {
        first[i] = (6 * u - 6 * u * u) * delta[i] + (3 * u * u - 4 * u + 1) * m0[i] + (3 * u * u - 2 * u) * m1[i] +
                   (32 * u - 96 * u * u + 64 * u * u * u) * shift[i];
        second[i] = (6 - 12 * u) * delta[i] + (6 * u - 4) * m0[i] + (6 * u - 2) * m1[i] +
                    (32 - 192 * u + 192 * u * u) * shift[i];
    }
    return {first, second};
}
struct Raw final
{
    V21FineChord row;
    double length = 0.0, depth = 0.0;
};
struct Selected final
{
    V21CourseRecord record;
    std::vector<Point> points;
    std::vector<double> fraction;
};
bool hasSceneCollision(
    std::span<Point const> points,
    std::span<V21CourseContact const> scene_contacts,
    double halfwidth)
{
    for (auto const &contact : scene_contacts)
    {
        for (size_t i = 2U; i < 63U; ++i)
        {
            Point const d{displacement(points[i][0] - contact.xy_m[0]),
                          displacement(points[i][1] - contact.xy_m[1])};
            if (norm(d) < contact.radius_m + halfwidth + 1.0)
            {
                return true;
            }
        }
    }
    return false;
}
} // namespace

std::expected<V21CourseResult, V21CourseFailure> buildV21CourseGeometry(V21CourseInput const &input,
    V21CourseCallbacks const &callbacks,
    std::stop_token token)
{
    try
    {
        check(token);
        if (input.canonical_bed.size() != COUNT || input.discharge_m3_s.size() != COUNT ||
            input.downstream.size() != COUNT || input.river.size() != COUNT ||
            input.virtual_through_lake.size() != COUNT || input.reference_stage.size() != COUNT ||
            !callbacks.scene_base || !callbacks.reference_base || !callbacks.standing)
        {
            fail(V21CourseFailure::Kind::InvalidInput, "complete V21 native scene arrays and callbacks required");
        }
        if (!std::all_of(input.canonical_bed.begin(), input.canonical_bed.end(),
                         [](double v) { return std::isfinite(v); }) ||
            !std::all_of(input.reference_stage.begin(), input.reference_stage.end(),
                         [](float v) { return std::isfinite(v); }))
        {
            fail(V21CourseFailure::Kind::InvalidInput, "finite canonical bed and native stage required");
        }
        std::vector<double> node_depth(COUNT), width(COUNT), stage(COUNT);
        for (size_t id = 0U; id < COUNT; ++id)
        {
            if ((id & 4095U) == 0U)
            {
                check(token);
            }
            double const q = input.discharge_m3_s[id];
            int32_t const target = std::max(input.downstream[id], 0);
            if (target >= int32_t(COUNT) || !std::isfinite(q) || q < 0.0 || !std::isfinite(input.canonical_bed[id]))
            {
                fail(V21CourseFailure::Kind::InvalidInput, "finite native scene bed/Q and valid receiver required");
            }
            Point const d{displacement((target % 256 - int32_t(id % 256)) * 256.0),
                          displacement((target / 256 - int32_t(id / 256)) * 256.0)};
            double const slope =
                std::max((input.canonical_bed[id] - input.canonical_bed[target]) / std::max(norm(d), 1.0), 1e-4);
            width[id] = std::sqrt(9.0 + 135.0 * std::clamp(q / 1.365'771'412'849'4263, 0.0, 1.0));
            node_depth[id] = depth(q, width[id], slope);
            stage[id] = input.river[id] ? input.canonical_bed[id] + node_depth[id] : input.reference_stage[id];
        }
        std::vector<Raw> raw;
        for (size_t id = 0U; id < COUNT; ++id)
        {
            if (!input.river[id] || input.downstream[id] < 0 || input.virtual_through_lake[id])
            {
                continue;
            }
            check(token);
            int32_t const target = input.downstream[id];
            Raw row;
            row.row.start = int32_t(id);
            row.row.target = target;
            row.row.a = {double(id % 256) * 256.0, double(id / 256) * 256.0};
            row.row.delta = {displacement((target % 256 - int32_t(id % 256)) * 256.0),
                             displacement((target / 256 - int32_t(id / 256)) * 256.0)};
            row.row.grade0 = input.canonical_bed[id];
            row.row.grade1 = input.canonical_bed[target];
            row.row.water_stage0 = stage[id];
            row.row.water_stage1 = stage[target];
            row.row.halfwidth = std::max(width[id] / 2.0, .05);
            row.row.discharge_m3_s = input.discharge_m3_s[id];
            if (row.row.grade1 > row.row.grade0 + 1e-8 || norm(row.row.delta) <= 0.0)
            {
                fail(V21CourseFailure::Kind::InvalidInput, "nonlocal zero or rising scene reach unsupported",
                     int32_t(id));
            }
            std::vector<Point> points(65U);
            for (size_t i = 0U; i < points.size(); ++i)
            {
                points[i] = add(row.row.a, row.row.delta, double(i) / 64.0);
            }
            auto z = heights(callbacks.scene_base, points);
            auto auth = standing(callbacks, points);
            for (size_t i = 0U; i < points.size(); ++i)
            {
                if (!(z[i] < auth[i].stage))
                {
                    continue;
                }
                if (i == 0U)
                {
                    break;
                }
                double lo = double(i - 1U) / 64.0, hi = double(i) / 64.0;
                for (uint32_t iteration = 0U; iteration < 40U; ++iteration)
                {
                    double const mid = (lo + hi) / 2.0;
                    std::array<Point, 1U> const point{add(row.row.a, row.row.delta, mid)};
                    auto const bed = heights(callbacks.scene_base, point);
                    auto const authority = standing(callbacks, point);
                    if (bed[0] < authority[0].stage)
                    {
                        hi = mid;
                    }
                    else
                    {
                        lo = mid;
                    }
                }
                std::array<Point, 1U> const point{add(row.row.a, row.row.delta, hi)};
                double const level = standing(callbacks, point)[0].stage;
                row.row.delta = scale(row.row.delta, hi);
                row.row.water_stage1 = level;
                row.row.grade1 = level - std::max(node_depth[id], node_depth[target]);
                row.row.shore_truncated = true;
                break;
            }
            if (row.row.grade1 > row.row.grade0 + 1e-8)
            {
                fail(V21CourseFailure::Kind::InvalidInput, "scene shore grade rises downstream", int32_t(id));
            }
            row.length = norm(row.row.delta);
            raw.push_back(row);
        }
        std::vector<V21CourseContact> scene_contacts;
        if (!input.critical_spill_vertices.empty())
        {
            scene_contacts.clear();
            std::set<int32_t> vertices(input.critical_spill_vertices.begin(), input.critical_spill_vertices.end());
            for (int32_t const vertex : vertices)
            {
                if (vertex < 0 || vertex >= int32_t(COUNT))
                {
                    fail(V21CourseFailure::Kind::InvalidInput, "valid critical spill vertex required");
                }
                Point const point{double(vertex % 256) * 256.0, double(vertex / 256) * 256.0};
                double distance = std::numeric_limits<double>::infinity();
                for (auto const &item : raw)
                {
                    auto const &row = item.row;
                    Point const offset{displacement(point[0] - row.a[0]), displacement(point[1] - row.a[1])};
                    double const t = std::clamp(
                        (offset[0] * row.delta[0] + offset[1] * row.delta[1])
                            / (row.delta[0] * row.delta[0] + row.delta[1] * row.delta[1]),
                        0.0, 1.0);
                    double const d = norm(add(offset, row.delta, -t));
                    distance = std::min(distance, d);
                    if (d < 1e-5 &&
                        std::abs(row.grade0 + t * (row.grade1 - row.grade0) - input.canonical_bed[vertex]) > 1e-7)
                    {
                        fail(V21CourseFailure::Kind::InvalidInput,
                             "active scene contact conflicts with rivercore grade", vertex);
                    }
                }
                double const radius = std::min(64.0, distance * .4);
                if (radius >= 1e-5)
                {
                    scene_contacts.push_back({point, radius, vertex, input.canonical_bed[vertex]});
                }
            }
        }
        struct Port final
        {
            std::vector<size_t> incoming, outgoing;
        };
        std::map<Key, Port> ports;
        for (size_t e = 0U; e < raw.size(); ++e)
        {
            ports[key(raw[e].row.a)].outgoing.push_back(e);
            ports[key(add(raw[e].row.a, raw[e].row.delta))].incoming.push_back(e);
        }
        std::map<Key, Point> tangents;
        for (auto const &[port, rows] : ports)
        {
            auto const unit = [&](size_t e) { return divide(raw[e].row.delta, raw[e].length); };
            auto const trunk = [&]()
            {
                return *std::max_element(rows.incoming.begin(), rows.incoming.end(),
                    [&](size_t a, size_t b)
                    {
                        return std::pair{raw[a].row.discharge_m3_s, -int64_t(a)}
                            < std::pair{raw[b].row.discharge_m3_s, -int64_t(b)};
                    });
            };
            Point vector{};
            if (rows.incoming.empty())
            {
                vector = unit(rows.outgoing.front());
            }
            else if (rows.outgoing.empty())
            {
                vector = unit(trunk());
            }
            else
            {
                size_t const up = trunk(), down = rows.outgoing.front();
                Point const d = unit(down), u = unit(up);
                for (size_t axis = 0U; axis < 2U; ++axis)
                {
                    vector[axis] = d[axis] * raw[down].row.discharge_m3_s / raw[down].length
                        + u[axis] * raw[up].row.discharge_m3_s / raw[up].length;
                }
            }
            tangents[port] = divide(vector, std::max(norm(vector), 1e-12));
        }
        std::vector<Selected> selected;
        selected.reserve(raw.size());
        for (auto const &item : raw)
        {
            check(token);
            auto const &r = item.row;
            Point const unit = divide(r.delta, item.length), normal{-unit[1], unit[0]}, mid = add(r.a, r.delta, .5);
            std::array<Point, 3U> const sides{add(mid, normal, -64.0), mid, add(mid, normal, 64.0)};
            auto const z = heights(callbacks.reference_base, sides);
            double const confinement = std::max(0.0, std::min(z[0], z[2]) - z[1]) / 64.0;
            double const slope = std::max((r.grade0 - r.grade1) / item.length, 1e-4);
            double const handle = std::clamp(.95 / (1 + .5 * confinement), .65, 1.15) * item.length;
            double const excursion = std::min(64.0, item.length * .18) / (1 + 10 * slope + 5 * confinement);
            std::vector<Point> straight(65U);
            for (size_t i = 0U; i < 65U; ++i)
            {
                straight[i] = add(r.a, r.delta, double(i) / 64.0);
            }
            auto const reference = standing(callbacks, straight);
            std::set<int32_t> allowed;
            for (size_t i = 1U; i < 64U; ++i)
            {
                allowed.insert(reference[i].owner);
            }
            bool found = false;
            std::tuple<double, double, double> best;
            Selected chosen;
            for (double candidate_scale : {.55, .8, 1.0, 1.3, 1.6, 2.0})
            {
                check(token);
                for (double candidate_shift : {0.0, -excursion, excursion})
                {
                    Point const m0 = scale(scale(tangents.at(key(r.a)), handle), candidate_scale);
                    Point const m1 = scale(scale(tangents.at(key(add(r.a, r.delta))), handle), candidate_scale);
                    Point const shift = scale(normal, candidate_shift);
                    std::vector<Point> points(65U);
                    std::array<double, 65U> arc{}, curvature{};
                    double radius = std::numeric_limits<double>::infinity(), minimum_speed = radius;
                    for (size_t i = 0U; i < 65U; ++i)
                    {
                        double const t = double(i) / 64.0;
                        points[i] = curve(r.a, r.delta, m0, m1, shift, t);
                        if (i)
                        {
                            arc[i] = arc[i - 1U] + norm(add(points[i], points[i - 1U], -1.0));
                        }
                        auto const [first, second] = derivatives(r.delta, m0, m1, shift, t);
                        double const speed = norm(first);
                        minimum_speed = std::min(minimum_speed, speed);
                        curvature[i] = std::abs(first[0] * second[1] - first[1] * second[0])
                            / std::max(speed * speed * speed, 1e-20);
                        radius = std::min(radius, 1.0 / std::max(curvature[i], 1e-12));
                    }
                    double const required = 2 * r.halfwidth * (1 + 1 / (1 + 20 * slope + 10 * confinement));
                    if (minimum_speed < .05 * item.length || radius < required)
                    {
                        continue;
                    }
                    auto const owners = standing(callbacks, points);
                    bool foreign = false;
                    for (size_t i = 3U; i < 62U; ++i)
                    {
                        if (!allowed.contains(owners[i].owner))
                        {
                            foreign = true;
                            break;
                        }
                    }
                    if (foreign)
                    {
                        continue;
                    }
                    bool collision = hasSceneCollision(points, scene_contacts, r.halfwidth);
                    if (collision)
                    {
                        continue;
                    }
                    auto const terrain = heights(callbacks.reference_base, points);
                    std::array<double, 65U> square{}, raise{}, bend{};
                    for (size_t i = 0U; i < 65U; ++i)
                    {
                        double const floor = r.grade0 + arc[i] / arc.back() * (r.grade1 - r.grade0);
                        double const diff = terrain[i] - floor, positive = std::max(floor - terrain[i], 0.0);
                        square[i] = diff * diff;
                        raise[i] = positive * positive;
                        bend[i] = curvature[i] * curvature[i];
                    }
                    double const cost = v9PairwiseSum(square) / 65.0
                        + 4 * (v9PairwiseSum(raise) / 65.0)
                        + candidate_shift * candidate_shift * .002
                        + .03 * (v9PairwiseSum(bend) / 65.0) * (item.length * item.length);
                    auto const ranking = std::tuple{cost, std::abs(candidate_shift), candidate_scale};
                    if (found && !(ranking < best))
                    {
                        continue;
                    }
                    found = true;
                    best = ranking;
                    chosen.record = {r.start,  item.length, 0.0, slope, confinement, norm(shift), radius,
                        required, cost, r.a, r.delta, m0, m1, shift};
                    size_t const n = std::max(size_t{17U}, size_t(std::ceil(arc.back() / 2.0)) + 1U);
                    chosen.points.resize(n);
                    chosen.fraction.assign(n, 0.0);
                    for (size_t i = 0U; i < n; ++i)
                    {
                        double const t = i + 1U == n ? 1.0 : double(i) * (1.0 / double(n - 1U));
                        chosen.points[i] = curve(r.a, r.delta, m0, m1, shift, t);
                        if (i)
                        {
                            chosen.fraction[i] =
                            chosen.fraction[i - 1U] + norm(add(chosen.points[i], chosen.points[i - 1U], -1.0));
                        }
                    }
                    chosen.record.arc_m = chosen.fraction.back();
                    for (double &f : chosen.fraction)
                    {
                        f /= chosen.record.arc_m;
                    }
                }
            }
            if (!found)
            {
                fail(V21CourseFailure::Kind::NoAdmissibleCourse, "no curvature-bounded C1 course", r.start);
            }
            selected.push_back(std::move(chosen));
        }
        std::vector<double> course_depth(raw.size()), course_slope(raw.size());
        for (size_t e = 0U; e < raw.size(); ++e)
        {
            auto const &r = raw[e].row;
            course_slope[e] = std::max((r.grade0 - r.grade1) / selected[e].record.arc_m, 1e-4);
            course_depth[e] = depth(r.discharge_m3_s, 2 * r.halfwidth, course_slope[e]);
        }
        for (uint32_t iteration = 0U; iteration < 12U; ++iteration)
        {
            for (size_t e = 0U; e < raw.size(); ++e)
            {
                auto const &r = raw[e].row;
                course_slope[e] = std::max(
                    (r.shore_truncated ? r.grade0 - r.water_stage1 + course_depth[e] : r.grade0 - r.grade1)
                        / selected[e].record.arc_m,
                    1e-4);
                course_depth[e] = depth(r.discharge_m3_s, 2 * r.halfwidth, course_slope[e]);
            }
        }
        for (size_t e = 0U; e < raw.size(); ++e)
        {
            node_depth[raw[e].row.start] = course_depth[e];
        }
        V21CourseResult result;
        result.contacts = std::move(scene_contacts);
        for (size_t e = 0U; e < raw.size(); ++e)
        {
            auto r = raw[e].row;
            auto const &course = selected[e];
            r.water_stage0 = r.grade0 + course_depth[e];
            if (r.shore_truncated)
            {
                r.grade1 = r.water_stage1 - course_depth[e];
            }
            else
            {
                r.water_stage1 = r.grade1 + node_depth[r.target];
            }
            result.courses.push_back(course.record);
            for (size_t i = 0U; i + 1U < course.points.size(); ++i)
            {
                auto chord = r;
                chord.a = course.points[i];
                chord.delta = add(course.points[i + 1U], course.points[i], -1.0);
                chord.grade0 = r.grade0 + course.fraction[i] * (r.grade1 - r.grade0);
                chord.grade1 = r.grade0 + course.fraction[i + 1U] * (r.grade1 - r.grade0);
                chord.water_stage0 = r.water_stage0 + course.fraction[i] * (r.water_stage1 - r.water_stage0);
                chord.water_stage1 = r.water_stage0 + course.fraction[i + 1U] * (r.water_stage1 - r.water_stage0);
                chord.global_order = result.chords.size();
                result.chords.push_back(chord);
            }
        }
        check(token);
        return result;
    }
    catch (CourseException const &error)
    {
        return std::unexpected{error.failure};
    }
    catch (std::exception const &error)
    {
        return std::unexpected{
            V21CourseFailure{
                token.stop_requested() ? V21CourseFailure::Kind::Cancelled
                    : V21CourseFailure::Kind::CallbackFailure,
                -1, error.what()}};
    }
}

} // namespace generation::detail
