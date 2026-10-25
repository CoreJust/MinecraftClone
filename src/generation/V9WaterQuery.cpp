#include "V9WaterQuery.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>
#include <queue>
#include <stdexcept>

namespace generation::detail
{
namespace
{
constexpr double WORLD = 65'536;
constexpr std::array<std::array<int, 2>, 6> NEIGHBOURS{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, 1}}};
int wrap(int value, int n) { return (value % n + n) % n; }
double periodic(double value, double period)
{
    double result = std::fmod(value, period);
    if (result < 0) {
        result += period;
    }
    return result < period ? result : 0;
}
std::optional<int> integerCoordinate(double value)
{
    if (!std::isfinite(value) || value < std::numeric_limits<int>::min() + 1.0 ||
        value > std::numeric_limits<int>::max() - 1.0) {
        return {};
    }
    return static_cast<int>(value);
}
int clippingCoordinate(double value)
{
    auto const integer = integerCoordinate(value);
    if (!integer) {
        throw std::runtime_error("regional clipping coordinate range exceeded");
    }
    return *integer;
}
V9WaterQueryGrid periodicGrid(V9WaterQueryGrid grid)
{
    grid.origin_x_m = periodic(grid.origin_x_m, WORLD);
    grid.origin_y_m = periodic(grid.origin_y_m, WORLD);
    return grid;
}
int32_t owner(V9WaterEquilibrium const& e, double x, double y)
{
    double xx = periodic(x, WORLD) / (WORLD / e.resolution);
    double yy = periodic(y, WORLD) / (WORLD / e.resolution);
    int ix = static_cast<int>(std::floor(xx)), iy = static_cast<int>(std::floor(yy));
    return e.triangle_basin_owner[2 * (iy * e.resolution + ix) + (xx - ix < yy - iy)];
}
bool domain(V9WaterEquilibrium const& e, int32_t id, double x, double y)
{
    x = periodic(x, WORLD);
    y = periodic(y, WORLD);
    if (owner(e, x, y) == id) {
        return true;
    }
    double xx = periodic(x / (WORLD / e.resolution), 1), yy = periodic(y / (WORLD / e.resolution), 1);
    if (std::min(xx, 1 - xx) < 1e-10 || std::min(yy, 1 - yy) < 1e-10 || std::abs(xx - yy) < 1e-10) {
        for (int dx : {-1, 1}) {
            for (int dy : {-1, 1}) {
                if (owner(e, x + dx * 1e-5, y + dy * 1e-5) == id) {
                    return true;
                }
            }
        }
    }
    return false;
}
std::vector<uint8_t> component(std::vector<uint8_t> const& eligible, std::vector<uint8_t> const& seeds, int n)
{
    std::vector<uint8_t> wet(eligible.size());
    std::queue<int> pending;
    for (size_t i = 0; i < wet.size(); ++i) {
        if (seeds[i] && eligible[i]) {
            wet[i] = 1;
            pending.push(static_cast<int>(i));
        }
    }
    while (!pending.empty()) {
        int p = pending.front();
        pending.pop();
        for (auto d : NEIGHBOURS) {
            int q = wrap(p / n + d[0], n) * n + wrap(p % n + d[1], n);
            if (eligible[q] && !wet[q]) {
                wet[q] = 1;
                pending.push(q);
            }
        }
    }
    return wet;
}
bool valid(V9WaterEquilibrium const& e)
{
    size_t count = static_cast<size_t>(e.resolution) * e.resolution;
    return e.resolution > 0 && e.resolution <= 256 && e.domain_m == WORLD && e.hydraulic_bed_m.size() == count &&
           e.ocean.size() == count && e.triangle_basin_owner.size() == 2 * count;
}
V9WaterFailure failure(char const* text) { return {V9WaterFailure::Kind::InvalidInput, text}; }
V9WaterFailure cancelled() { return {V9WaterFailure::Kind::Cancelled, "water query cancelled"}; }
bool validGrid(V9WaterQueryGrid const& g)
{
    double const extent_x = (g.columns - 1.0) * g.spacing_m;
    double const extent_y = (g.rows - 1.0) * g.spacing_m;
    return g.rows >= 2 && g.columns >= 2 && g.rows < std::numeric_limits<int>::max() &&
           g.columns < std::numeric_limits<int>::max() && g.bed_m.size() == static_cast<size_t>(g.rows) * g.columns &&
           std::isfinite(g.origin_x_m) && std::isfinite(g.origin_y_m) && std::isfinite(g.spacing_m) &&
           g.spacing_m > 0 && std::isfinite(extent_x) && std::isfinite(extent_y) &&
           std::isfinite(g.origin_x_m + extent_x) && std::isfinite(g.origin_y_m + extent_y) &&
           std::ranges::all_of(g.bed_m, [](double z) { return std::isfinite(z); });
}
std::optional<std::array<int, 4>> riverImages(V9WaterQueryGrid const& g, double xmin, double xmax, double ymin,
                            double ymax)
{
    auto const x0 = integerCoordinate(std::ceil((g.origin_x_m - xmax) / WORLD));
    auto const x1 = integerCoordinate(std::floor((g.origin_x_m + (g.columns - 1) * g.spacing_m - xmin) / WORLD));
    auto const y0 = integerCoordinate(std::ceil((g.origin_y_m - ymax) / WORLD));
    auto const y1 = integerCoordinate(std::floor((g.origin_y_m + (g.rows - 1) * g.spacing_m - ymin) / WORLD));
    if (!x0 || !x1 || !y0 || !y1 || int64_t(*x1) - *x0 > 2 || int64_t(*y1) - *y0 > 2) {
        return {};
    }
    return std::array<int, 4>{*x0, *x1, *y0, *y1};
}

using Point = std::array<double, 4>;
using Polygon = std::vector<Point>;
double cross(Point const& a, Point const& b, Point const& p)
{
    return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]);
}
Polygon clean(Polygon p)
{
    Polygon result;
    for (auto a : p) {
        if (result.empty() || std::hypot(a[0] - result.back()[0], a[1] - result.back()[1]) > 1e-9) {
            result.push_back(a);
        }
    }
    if (result.size() > 1 &&
        std::hypot(result.front()[0] - result.back()[0], result.front()[1] - result.back()[1]) <= 1e-9) {
        result.pop_back();
    }
    double area = 0;
    for (size_t i = 0; i < result.size(); ++i) {
        area += cross(result[0], result[i], result[(i + 1) % result.size()]);
    }
    if (result.size() < 3 || std::abs(area) <= 1e-10) {
        result.clear();
    }
    return result;
}
template <typename F> Polygon clip(Polygon const& p, F signed_distance)
{
    Polygon result;
    for (size_t i = 0; i < p.size(); ++i) {
        auto a = p[i], b = p[(i + 1) % p.size()];
        double sa = signed_distance(a), sb = signed_distance(b);
        if (sa >= 0) {
            result.push_back(a);
        }
        if ((sa >= 0) != (sb >= 0)) {
            Point q{};
            for (int k = 0; k < 4; ++k) {
                q[k] = a[k] + (b[k] - a[k]) * sa / (sa - sb);
            }
            result.push_back(q);
        }
    }
    return clean(std::move(result));
}
std::vector<Polygon> subtract(Polygon p, Polygon const& cover)
{
    double area = 0;
    for (size_t i = 0; i < cover.size(); ++i) {
        area += cover[i][0] * cover[(i + 1) % cover.size()][1] - cover[(i + 1) % cover.size()][0] * cover[i][1];
    }
    double sign = area >= 0 ? 1 : -1;
    std::vector<Polygon> pieces;
    for (size_t i = 0; i < cover.size() && !p.empty(); ++i) {
        auto a = cover[i], b = cover[(i + 1) % cover.size()];
        auto outside = clip(p, [&](Point q) { return -sign * cross(a, b, q); });
        if (!outside.empty()) {
            pieces.push_back(std::move(outside));
        }
        p = clip(p, [&](Point q) { return sign * cross(a, b, q); });
    }
    return pieces;
}
std::vector<Polygon> subtractOccupied(std::vector<Polygon> fragments, std::vector<Polygon> const& occupied)
{
    for (auto const& previous : occupied) {
        std::vector<Polygon> next;
        for (auto const& fragment : fragments) {
            auto parts = subtract(fragment, previous);
            next.insert(next.end(), parts.begin(), parts.end());
        }
        fragments = std::move(next);
    }
    return fragments;
}
std::vector<Polygon> domainPolygons(Polygon const& p, int32_t id, V9WaterEquilibrium const& e)
{
    if (p.empty()) {
        return {};
    }
    double cell = WORLD / e.resolution;
    double xmin = p[0][0], xmax = xmin, ymin = p[0][1], ymax = ymin;
    for (auto q : p) {
        xmin = std::min(xmin, q[0]);
        xmax = std::max(xmax, q[0]);
        ymin = std::min(ymin, q[1]);
        ymax = std::max(ymax, q[1]);
    }
    int x0 = clippingCoordinate(std::floor(xmin / cell)), x1 = clippingCoordinate(std::floor(xmax / cell)),
        y0 = clippingCoordinate(std::floor(ymin / cell)), y1 = clippingCoordinate(std::floor(ymax / cell));
    int64_t const width = int64_t(x1) - x0 + 1, height = int64_t(y1) - y0 + 1;
    if (width > 64 || height > 64 || width * height > 64) {
        throw std::runtime_error("catchment polygon intersection work cap exceeded");
    }
    std::vector<Polygon> pieces;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            for (int k = 0; k < 2; ++k) {
                if (e.triangle_basin_owner[2 * (wrap(y, e.resolution) * e.resolution + wrap(x, e.resolution)) + k] !=
                    id) {
                    continue;
                }
                Point a{x * cell, y * cell, 0, 0}, b{(x + 1) * cell, y * cell, 0, 0},
                    c{(x + 1) * cell, (y + 1) * cell, 0, 0}, d{x * cell, (y + 1) * cell, 0, 0};
                std::array<Point, 3> corners = k == 0 ? std::array<Point, 3>{a, b, c} : std::array<Point, 3>{a, c, d};
                Polygon piece = p;
                for (int j = 0; j < 3; ++j) {
                    piece = clip(piece, [&](Point q) { return cross(corners[j], corners[(j + 1) % 3], q); });
                }
                if (!piece.empty()) {
                    pieces.push_back(std::move(piece));
                }
            }
        }
    }
    return pieces;
}
} // namespace
V9WaterQueryModel makeV9WaterQueryModel(V9WaterRefinement const& model)
{
    if (!model.hasNativeRivers()) {
        return {};
    }
    return {[&model](double x, double y) { return model.base(x, y); }, model.reaches(), model.protectedContacts(), {}};
}
std::expected<V9WaterFixedSupport, V9WaterFailure> buildV9WaterFixedSupport(V9WaterEquilibrium const& e,
                            V9WaterQueryModel const& model)
{
    if (!valid(e)) {
        return std::unexpected(failure("fixed support requires valid 65536m native grid cap256"));
    }
    if (model.progress && !model.progress("fixed-base", 0, 4ULL * e.resolution * e.resolution)) {
        return std::unexpected(cancelled());
    }
    int n = static_cast<int>(e.resolution), size = 2 * n;
    double step = WORLD / size;
    std::vector<double> base(size * size);
    std::vector<uint8_t> eligible(base.size()), seeds(base.size());
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int p = y * size + x;
            if (model.base) {
                base[p] = model.base(x * step, y * step);
            } else {
                double tx = (x % 2) * .5, ty = (y % 2) * .5;
                int ix = x / 2, iy = y / 2;
                auto sample = [&](int dx, int dy) {
                    return e.hydraulic_bed_m[wrap(iy + dy, n) * n + wrap(ix + dx, n)];
                };
                base[p] = (1 - ty) * ((1 - tx) * sample(0, 0) + tx * sample(1, 0)) +
                          ty * ((1 - tx) * sample(0, 1) + tx * sample(1, 1));
            }
            eligible[p] = base[p] < 0;
            if (x % 2 == 0 && y % 2 == 0) {
                seeds[p] = e.ocean[(y / 2) * n + x / 2];
            }
        }
    }
    V9WaterFixedSupport result;
    result.size = size;
    auto permission = [&](int32_t id, double level, std::vector<uint8_t> const& wet, std::vector<uint8_t> const* dom) {
        V9WaterQueryPermission row{id, level, wet};
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                int p = y * size + x;
                for (auto d : NEIGHBOURS) {
                    row.permission[p] |= wet[wrap(y - d[0], size) * size + wrap(x - d[1], size)];
                }
                if (dom) {
                    row.permission[p] &= (*dom)[p];
                }
            }
        }
        result.bodies.push_back(std::move(row));
    };
    permission(0, 0, component(eligible, seeds, size), nullptr);
    for (auto const& body : e.bodies) {
        if (model.progress && !model.progress("fixed-body-support", result.bodies.size() - 1, e.bodies.size())) {
            return std::unexpected(cancelled());
        }
        std::vector<uint8_t> dom(base.size());
        std::fill(seeds.begin(), seeds.end(), 0);
        size_t lowest = base.size();
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                int p = y * size + x;
                dom[p] = domain(e, body.id, x * step, y * step);
                eligible[p] = dom[p] && base[p] < body.stage_m;
                if (eligible[p] && (lowest == base.size() || base[p] < base[lowest])) {
                    lowest = p;
                }
            }
        }
        if (lowest < base.size()) {
            seeds[lowest] = 1;
        }
        auto wet = component(eligible, seeds, size);
        permission(body.id, body.stage_m, wet, &dom);
        double best = std::numeric_limits<double>::infinity();
        int from = -1, to = -1;
        for (auto d : NEIGHBOURS) {
            for (int p = 0; p < size * size; ++p) {
                int q = wrap(p / size + d[0], size) * size + wrap(p % size + d[1], size);
                if (wet[p] && !dom[q] && std::max(base[p], base[q]) < best) {
                    best = std::max(base[p], base[q]);
                    from = p;
                    to = q;
                }
            }
        }
        if (best < body.stage_m - 1e-6) {
            result.escape_failures.push_back({body.id,
                            body.stage_m,
                            body.spill_m,
                            best,
                            {(from % size) * step, (from / size) * step},
                            {(to % size) * step, (to / size) * step}});
        }
    }
    std::stable_sort(result.bodies.begin(), result.bodies.end(), [](auto const& a, auto const& b) {
        return a.stage_m == b.stage_m ? a.body_id < b.body_id : a.stage_m < b.stage_m;
    });
    if (model.base) {
        for (auto const& c : model.contacts) {
            double actual = model.base(c.xy_m[0], c.xy_m[1]);
            result.contact_actual_height_m.push_back(actual);
            result.maximum_contact_error_m = std::max(result.maximum_contact_error_m, std::abs(actual - c.height_m));
        }
    }
    result.contacts_pass = result.maximum_contact_error_m <= 2e-6;
    return result;
}
std::expected<V9WaterLocalSupport, V9WaterFailure> queryV9WaterLocalSupport(V9WaterEquilibrium const& e,
                            V9WaterFixedSupport const& fixed,
                            V9WaterQueryGrid const& query,
                            V9WaterQueryModel const& model)
{
    if (!valid(e) || !validGrid(query) || fixed.size != 2 * e.resolution) {
        return std::unexpected(failure("invalid regional support grid"));
    }
    auto const g = periodicGrid(query);
    if (model.progress && !model.progress("local-support", 0, g.bed_m.size())) {
        return std::unexpected(cancelled());
    }
    size_t count = g.bed_m.size();
    for (auto const& b : fixed.bodies) {
        if (b.permission.size() != static_cast<size_t>(fixed.size) * fixed.size) {
            return std::unexpected(failure("invalid fixed permission extent"));
        }
    }
    V9WaterLocalSupport s;
    s.ocean.resize(count);
    s.lake.resize(count);
    s.river.resize(count);
    s.lake_id.resize(count);
    s.river_owner_id.assign(count, -1);
    s.water_level_m.resize(count);
    s.water_depth_m.resize(count);
    std::vector<int32_t> owners(count, -1);
    std::vector<double> stages(count, std::numeric_limits<double>::infinity());
    for (size_t p = 0; p < count; ++p) {
        double x = g.origin_x_m + (p % g.columns) * g.spacing_m, y = g.origin_y_m + (p / g.columns) * g.spacing_m;
        int ix = wrap(static_cast<int>(std::nearbyint(periodic(x, WORLD) / (WORLD / fixed.size))), fixed.size),
            iy = wrap(static_cast<int>(std::nearbyint(periodic(y, WORLD) / (WORLD / fixed.size))), fixed.size);
        for (auto const& b : fixed.bodies) {
            if (b.permission[iy * fixed.size + ix] && (b.body_id == 0 || domain(e, b.body_id, x, y)) &&
                g.bed_m[p] < b.stage_m) {
                owners[p] = b.body_id;
                stages[p] = b.stage_m;
                break;
            }
        }
        double cx = periodic(x, WORLD) / (WORLD / e.resolution), cy = periodic(y, WORLD) / (WORLD / e.resolution);
        size_t native = wrap(static_cast<int>(std::nearbyint(cy)), e.resolution) * e.resolution +
                        wrap(static_cast<int>(std::nearbyint(cx)), e.resolution);
        if (e.lake.size() == e.ocean.size() && e.lake_id.size() == e.ocean.size() &&
            e.water_level_m.size() == e.ocean.size() && std::abs(cx - std::nearbyint(cx)) < 1e-10 &&
            std::abs(cy - std::nearbyint(cy)) < 1e-10 && std::abs(g.bed_m[p] - e.hydraulic_bed_m[native]) < 1e-8) {
            owners[p] = -1;
            stages[p] = std::numeric_limits<double>::infinity();
            if (e.ocean[native]) {
                owners[p] = 0;
                stages[p] = 0;
            }
            if (e.lake[native]) {
                owners[p] = e.lake_id[native];
                stages[p] = e.water_level_m[native];
            }
        }
        s.ocean[p] = owners[p] == 0;
        s.lake[p] = owners[p] > 0;
        s.lake_id[p] = std::max(owners[p], 0);
        s.water_level_m[p] = static_cast<float>(owners[p] >= 0 ? stages[p] : g.bed_m[p]);
        s.water_depth_m[p] = static_cast<float>(owners[p] >= 0 ? stages[p] - g.bed_m[p] : 0);
    }
    std::vector<V9RefinementReach> reaches(model.reaches.begin(), model.reaches.end());
    std::stable_sort(reaches.begin(), reaches.end(),
                     [](auto const& a, auto const& b) { return a.discharge_m3_s > b.discharge_m3_s; });
    for (auto const& r : reaches) {
        if (model.progress && !model.progress("local-river-support", &r - reaches.data(), reaches.size())) {
            return std::unexpected(cancelled());
        }
        double length2 = r.delta[0] * r.delta[0] + r.delta[1] * r.delta[1];
        if (!(length2 > 0)) {
            continue;
        }
        double xmin = std::min(r.a[0], r.a[0] + r.delta[0]) - r.halfwidth,
               xmax = std::max(r.a[0], r.a[0] + r.delta[0]) + r.halfwidth,
               ymin = std::min(r.a[1], r.a[1] + r.delta[1]) - r.halfwidth,
               ymax = std::max(r.a[1], r.a[1] + r.delta[1]) + r.halfwidth;
        auto const images = riverImages(g, xmin, xmax, ymin, ymax);
        if (!images) {
            return std::unexpected(failure("regional periodic image range exceeded"));
        }
        auto const [x0, x1, y0, y1] = *images;
        for (int iy = y0; iy <= y1; ++iy) {
            for (int ix = x0; ix <= x1; ++ix) {
                for (size_t p = 0; p < count; ++p) {
                    if (s.ocean[p] || s.lake[p] || s.river[p]) {
                        continue;
                    }
                    double dx = g.origin_x_m + (p % g.columns) * g.spacing_m - r.a[0] - ix * WORLD,
                           dy = g.origin_y_m + (p / g.columns) * g.spacing_m - r.a[1] - iy * WORLD,
                           t = (dx * r.delta[0] + dy * r.delta[1]) / length2,
                           z = r.water_stage0 + t * (r.water_stage1 - r.water_stage0);
                    if (t >= 0 && t <= 1 && std::hypot(dx - t * r.delta[0], dy - t * r.delta[1]) <= r.halfwidth &&
                        g.bed_m[p] < z) {
                        s.river[p] = 1;
                        s.river_owner_id[p] = r.start;
                        s.water_level_m[p] = static_cast<float>(z);
                        s.water_depth_m[p] = static_cast<float>(z - g.bed_m[p]);
                    }
                }
            }
        }
    }
    return s;
}
std::expected<V9WaterQueryMesh, V9WaterFailure>
queryV9WaterMesh(V9WaterEquilibrium const& e, V9WaterFixedSupport const& fixed, V9WaterQueryGrid const& query,
                 V9WaterQueryModel const& model, uint64_t max_triangles, uint64_t max_output_triangles)
{
    if (!validGrid(query)) {
        return std::unexpected(failure("invalid regional mesh grid"));
    }
    double const endpoint_x = query.origin_x_m + (query.columns - 1) * query.spacing_m;
    double const endpoint_y = query.origin_y_m + (query.rows - 1) * query.spacing_m;
    double const coordinate_limit = std::numeric_limits<float>::max();
    if (std::abs(query.origin_x_m) > coordinate_limit || std::abs(query.origin_y_m) > coordinate_limit ||
        std::abs(endpoint_x) > coordinate_limit || std::abs(endpoint_y) > coordinate_limit) {
        return std::unexpected(failure("regional mesh position exceeds float32 range"));
    }
    auto const g = periodicGrid(query);
    double const shift_x = query.origin_x_m - g.origin_x_m, shift_y = query.origin_y_m - g.origin_y_m;
    uint64_t count = 2ULL * (g.rows - 1) * (g.columns - 1);
    if (count > max_triangles || count > std::numeric_limits<int32_t>::max() || max_output_triangles < 1) {
        return std::unexpected(failure("regional triangle cap exceeded"));
    }
    auto local = queryV9WaterLocalSupport(e, fixed, g, model);
    if (!local) {
        return std::unexpected(local.error());
    }
    if (!fixed.contacts_pass) {
        return std::unexpected(failure("critical_contact_source_mismatch"));
    }
    for (auto const& escape : fixed.escape_failures) {
        for (size_t p = 0; p < g.bed_m.size(); ++p) {
            if (local->lake[p] && local->lake_id[p] == escape.body_id) {
                return std::unexpected(failure("lower_escape_requires_terrain_reconciliation"));
            }
        }
    }
    V9WaterQueryMesh mesh;
    std::vector<Polygon> tin;
    std::vector<double> levels(count, std::numeric_limits<double>::infinity());
    std::vector<int32_t> owners(count, -1);
    std::vector<std::vector<Polygon>> occupied(count);
    auto point = [&](size_t p) {
        return Point{g.origin_x_m + (p % g.columns) * g.spacing_m, g.origin_y_m + (p / g.columns) * g.spacing_m,
                     g.bed_m[p], 0};
    };
    for (uint32_t y = 0; y + 1 < g.rows; ++y) {
        for (uint32_t x = 0; x + 1 < g.columns; ++x) {
            size_t a = y * g.columns + x, b = a + 1, c = a + g.columns + 1, d = a + g.columns;
            for (auto ids : {std::array<size_t, 3>{a, b, c}, std::array<size_t, 3>{a, c, d}}) {
                size_t index = tin.size();
                tin.push_back({point(ids[0]), point(ids[1]), point(ids[2])});
                for (auto p : ids) {
                    double z = std::numeric_limits<double>::infinity();
                    int32_t id = -1;
                    if (local->ocean[p]) {
                        z = 0;
                        id = 0;
                    } else if (local->lake[p]) {
                        for (auto const& body : e.bodies) {
                            if (body.id == local->lake_id[p]) {
                            z = body.stage_m;
                            id = body.id;
                            break;
                            }
                        }
                    }
                    if (z < levels[index]) {
                        levels[index] = z;
                        owners[index] = id;
                    }
                }
            }
        }
    }
    auto emit = [&](Polygon const& poly, size_t index, uint8_t kind, int32_t id) {
        auto pieces = kind == 2 ? domainPolygons(poly, id, e) : std::vector<Polygon>{poly};
        for (auto const& p : pieces) {
            for (size_t j = 1; j + 1 < p.size(); ++j) {
                std::array<Point, 3> tri{p[0], p[j], p[j + 1]};
                double area = std::abs(cross(tri[0], tri[1], tri[2])) / 2;
                if (area < 5e-11) {
                    continue;
                }
                if (mesh.triangles.size() >= max_output_triangles) {
                    throw std::runtime_error("regional output triangle cap exceeded");
                }
                V9WaterSurfaceTriangle t{};
                t.owner = id;
                t.parent_triangle = static_cast<int32_t>(index);
                for (int k = 0; k < 3; ++k) {
                    double const x = tri[k][0] + shift_x, y = tri[k][1] + shift_y;
                    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(tri[k][3]) ||
                        std::abs(x) > coordinate_limit || std::abs(y) > coordinate_limit ||
                        std::abs(tri[k][3]) > coordinate_limit) {
                        throw std::runtime_error("regional mesh position exceeds float32 range");
                    }
                    t.xyz_m[k] = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(tri[k][3])};
                }
                mesh.triangles.push_back(t);
                mesh.kind.push_back(kind);
                if (kind <= 2) {
                    auto row = std::find_if(mesh.inventory.begin(), mesh.inventory.end(),
                            [&](auto const& r) { return r.body_id == id; });
                    if (row == mesh.inventory.end()) {
                        mesh.inventory.push_back({id});
                        row = mesh.inventory.end() - 1;
                    }
                    row->query_area_m2 += area;
                    row->query_volume_m3 += area *
                            (std::max(tri[0][3] - tri[0][2], 0.) + std::max(tri[1][3] - tri[1][2], 0.) +
                            std::max(tri[2][3] - tri[2][2], 0.)) /
                            3;
                }
            }
        }
    };
    try {
        for (size_t index = 0; index < count; ++index) {
            if (std::isfinite(levels[index])) {
                auto p = tin[index];
                for (auto& q : p) {
                    q[3] = levels[index];
                }
                emit(clip(p, [](Point q) { return q[3] - q[2]; }), index, owners[index] == 0 ? 1 : 2, owners[index]);
            }
        }
        std::vector<V9RefinementReach> reaches(model.reaches.begin(), model.reaches.end());
        std::stable_sort(reaches.begin(), reaches.end(),
                         [](auto const& a, auto const& b) { return a.discharge_m3_s > b.discharge_m3_s; });
        for (auto const& r : reaches) {
            if (model.progress && !model.progress("river-clipping", &r - reaches.data(), reaches.size())) {
                return std::unexpected(cancelled());
            }
            double length = std::hypot(r.delta[0], r.delta[1]);
            if (!(length > 0)) {
                continue;
            }
            double ux = r.delta[0] / length, uy = r.delta[1] / length, nx = -uy, ny = ux;
            double xmin = std::min(r.a[0], r.a[0] + r.delta[0]) - r.halfwidth,
                   xmax = std::max(r.a[0], r.a[0] + r.delta[0]) + r.halfwidth,
                   ymin = std::min(r.a[1], r.a[1] + r.delta[1]) - r.halfwidth,
                   ymax = std::max(r.a[1], r.a[1] + r.delta[1]) + r.halfwidth;
            auto const images = riverImages(g, xmin, xmax, ymin, ymax);
            if (!images) {
                throw std::runtime_error("regional periodic image range exceeded");
            }
            auto const [x0, x1, y0, y1] = *images;
            for (int iy = y0; iy <= y1; ++iy) {
                for (int ix = x0; ix <= x1; ++ix) {
                    double ax = r.a[0] + ix * WORLD, ay = r.a[1] + iy * WORLD;
                    int bx0 = std::max(0, clippingCoordinate(std::floor(
                            (xmin + ix * WORLD - g.spacing_m - g.origin_x_m) / g.spacing_m))),
                        bx1 = std::min(static_cast<int>(g.columns) - 2,
                            clippingCoordinate(
                            std::ceil((xmax + ix * WORLD + g.spacing_m - g.origin_x_m) / g.spacing_m))),
                        by0 = std::max(0, clippingCoordinate(std::floor(
                            (ymin + iy * WORLD - g.spacing_m - g.origin_y_m) / g.spacing_m))),
                        by1 = std::min(static_cast<int>(g.rows) - 2,
                            clippingCoordinate(
                            std::ceil((ymax + iy * WORLD + g.spacing_m - g.origin_y_m) / g.spacing_m)));
                    size_t const row_width = static_cast<size_t>(bx1 - bx0 + 1);
                    size_t const row_count = static_cast<size_t>(by1 - by0 + 1);
                    size_t const triangles_per_row = row_width * 2;
                    for (size_t triangle = 0; triangle < row_count * triangles_per_row; ++triangle) {
                        int const y = by0 + static_cast<int>(triangle / triangles_per_row);
                        int const x = bx0 + static_cast<int>((triangle % triangles_per_row) / 2);
                        int const k = static_cast<int>(triangle % 2);
                        size_t index = 2 * (y * (g.columns - 1) + x) + k;
                        if (++mesh.river_candidate_clips > max_triangles * 16 + 1024) {
                            throw std::runtime_error("bounded river clipping work cap exceeded");
                        }
                        auto p = tin[index];
                        for (auto& q : p) {
                            q[3] = r.water_stage0 + (r.water_stage1 - r.water_stage0) *
                            ((q[0] - ax) * ux + (q[1] - ay) * uy) / length;
                        }
                        p = clip(p, [](Point q) { return q[3] - q[2]; });
                        p = clip(p, [&](Point q) { return r.halfwidth + (q[0] - ax) * nx + (q[1] - ay) * ny; });
                        p = clip(p, [&](Point q) { return r.halfwidth - (q[0] - ax) * nx - (q[1] - ay) * ny; });
                        p = clip(p, [&](Point q) { return (q[0] - ax) * ux + (q[1] - ay) * uy; });
                        p = clip(p, [&](Point q) { return length - (q[0] - ax) * ux - (q[1] - ay) * uy; });
                        if (std::isfinite(levels[index])) {
                            p = clip(p, [&](Point q) { return q[2] - levels[index]; });
                        }
                        if (p.empty()) {
                            continue;
                        }
                        auto fragments = subtractOccupied({p}, occupied[index]);
                        if (fragments.size() > 64) {
                            throw std::runtime_error("bounded river junction fragmentation cap exceeded");
                        }
                        for (auto const& fragment : fragments) {
                            emit(fragment, index, 3, r.start);
                        }
                        occupied[index].insert(occupied[index].end(), fragments.begin(), fragments.end());
                    }
                }
            }
        }
    } catch (std::runtime_error const& error) {
        return std::unexpected(V9WaterFailure{V9WaterFailure::Kind::InvalidInput, error.what()});
    }
    for (size_t i = 0; i < count; ++i) {
        if (std::isfinite(levels[i]) && std::none_of(mesh.inventory.begin(), mesh.inventory.end(),
                            [&](auto const& row) { return row.body_id == owners[i]; })) {
            mesh.inventory.push_back({owners[i]});
        }
    }
    std::sort(mesh.inventory.begin(), mesh.inventory.end(),
              [](auto const& a, auto const& b) { return a.body_id < b.body_id; });
    return mesh;
}
} // namespace generation::detail
