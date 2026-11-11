#include "V9WaterQuery.hpp"

#include <algorithm>
#include <array>
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
std::pair<double, double> project(std::array<double, 2> const& a, std::array<double, 2> const& delta,
    double x, double y, double domain_m)
{
    double const dx = periodic(x - a[0] + domain_m / 2, domain_m) - domain_m / 2;
    double const dy = periodic(y - a[1] + domain_m / 2, domain_m) - domain_m / 2;
    double const t = std::clamp(
        (dx * delta[0] + dy * delta[1])
            / (delta[0] * delta[0] + delta[1] * delta[1]),
        0.0, 1.0);
    double const distance_x = dx - t * delta[0];
    double const distance_y = dy - t * delta[1];
    return {t, distance_x * distance_x + distance_y * distance_y};
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
bool validGrid(V9WaterQueryGrid const& g, bool allow_singleton = false)
{
    double const extent_x = (g.columns - 1.0) * g.spacing_m;
    double const extent_y = (g.rows - 1.0) * g.spacing_m;
    uint32_t const minimum_extent = allow_singleton ? 1U : 2U;
    return g.rows >= minimum_extent && g.columns >= minimum_extent && g.rows < std::numeric_limits<int>::max() &&
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
    struct DomainOwners final
    {
        std::array<int32_t, 5U> owners{};
        uint8_t neighbor_count = 0U;
    };

    int n = static_cast<int>(e.resolution), size = 2 * n;
    double step = WORLD / size;
    std::vector<double> base(size * size);
    std::vector<uint8_t> eligible(base.size()), seeds(base.size());
    std::vector<DomainOwners> domain_owners(base.size());
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
    for (int y = 0; y < size; ++y) {
        double const world_y = periodic(y * step, WORLD);
        for (int x = 0; x < size; ++x) {
            int const p = y * size + x;
            double const world_x = periodic(x * step, WORLD);
            DomainOwners& owners = domain_owners[p];
            owners.owners[0U] = owner(e, world_x, world_y);
            double const xx = periodic(world_x / (WORLD / e.resolution), 1);
            double const yy = periodic(world_y / (WORLD / e.resolution), 1);
            if (std::min(xx, 1 - xx) < 1e-10 || std::min(yy, 1 - yy) < 1e-10 || std::abs(xx - yy) < 1e-10) {
                size_t candidate = 1U;
                for (int dx : {-1, 1}) {
                    for (int dy : {-1, 1}) {
                        owners.owners[candidate++] = owner(e, world_x + dx * 1e-5, world_y + dy * 1e-5);
                    }
                }
                owners.neighbor_count = 4U;
            }
        }
    }

    V9WaterFixedSupport result;
    result.size = size;
    auto permission = [&](int32_t id, double level, std::vector<uint8_t> const& wet, std::vector<uint8_t> const* dom) {
        V9WaterQueryPermission row{id, level, wet};
        for (int y = 0; y < size; ++y) {
            int const previous_row = (y == 0 ? size - 1 : y - 1) * size;
            int const next_row = (y + 1 == size ? 0 : y + 1) * size;
            int const row_start = y * size;
            for (int x = 0; x < size; ++x) {
                int const previous_x = x == 0 ? size - 1 : x - 1;
                int const next_x = x + 1 == size ? 0 : x + 1;
                int const p = row_start + x;
                uint8_t permission_value = wet[p];
                permission_value |= wet[previous_row + x];
                permission_value |= wet[next_row + x];
                permission_value |= wet[row_start + previous_x];
                permission_value |= wet[row_start + next_x];
                permission_value |= wet[previous_row + previous_x];
                permission_value |= wet[next_row + next_x];
                row.permission[p] = permission_value;
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
                DomainOwners const& owners = domain_owners[p];
                bool within_domain = owners.owners[0U] == body.id;
                for (size_t candidate = 1U; !within_domain && candidate <= owners.neighbor_count; ++candidate) {
                    within_domain = owners.owners[candidate] == body.id;
                }
                dom[p] = static_cast<uint8_t>(within_domain);
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
static std::expected<V9WaterLocalSupport, V9WaterFailure> queryLocalSupportImpl(V9WaterEquilibrium const& e,
                            V9WaterFixedSupport const& fixed,
                            V9WaterQueryGrid const& query,
                            V9WaterQueryModel const& model,
                            bool allow_singleton,
                            bool require_float32_river_depth,
                            bool closed_river_endpoints)
{
    if (!valid(e) || !validGrid(query, allow_singleton) || fixed.size != 2 * e.resolution) {
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
    std::vector<size_t> reach_order(model.reaches.size());
    std::iota(reach_order.begin(), reach_order.end(), size_t{0});
    std::stable_sort(reach_order.begin(), reach_order.end(), [&model](size_t a, size_t b) {
        return model.reaches[a].discharge_m3_s > model.reaches[b].discharge_m3_s;
    });
    std::vector<double> grid_x(g.columns), grid_y(g.rows);
    for (uint32_t column = 0U; column < g.columns; ++column)
    {
        grid_x[column] = g.origin_x_m + static_cast<double>(column) * g.spacing_m;
    }
    for (uint32_t row = 0U; row < g.rows; ++row)
    {
        grid_y[row] = g.origin_y_m + static_cast<double>(row) * g.spacing_m;
    }
    for (size_t ordered = 0U; ordered < reach_order.size(); ++ordered) {
        V9RefinementReach const& r = model.reaches[reach_order[ordered]];
        if (model.progress && !model.progress("local-river-support", ordered, reach_order.size())) {
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
                double const image_x = static_cast<double>(ix) * WORLD;
                double const image_y = static_cast<double>(iy) * WORLD;
                double const xmin = std::min(r.a[0], r.a[0] + r.delta[0]) - r.halfwidth + image_x;
                double const xmax = std::max(r.a[0], r.a[0] + r.delta[0]) + r.halfwidth + image_x;
                double const ymin = std::min(r.a[1], r.a[1] + r.delta[1]) - r.halfwidth + image_y;
                double const ymax = std::max(r.a[1], r.a[1] + r.delta[1]) + r.halfwidth + image_y;
                if (xmax < grid_x.front() || xmin > grid_x.back()
                    || ymax < grid_y.front() || ymin > grid_y.back())
                {
                    continue;
                }
                uint32_t const column_begin = static_cast<uint32_t>(std::clamp(
                    std::ceil((xmin - g.origin_x_m) / g.spacing_m), 0.0, static_cast<double>(g.columns - 1U)));
                uint32_t const column_end = static_cast<uint32_t>(std::clamp(
                    std::floor((xmax - g.origin_x_m) / g.spacing_m), 0.0, static_cast<double>(g.columns - 1U)));
                uint32_t const row_begin = static_cast<uint32_t>(std::clamp(
                    std::ceil((ymin - g.origin_y_m) / g.spacing_m), 0.0, static_cast<double>(g.rows - 1U)));
                uint32_t const row_end = static_cast<uint32_t>(std::clamp(
                    std::floor((ymax - g.origin_y_m) / g.spacing_m), 0.0, static_cast<double>(g.rows - 1U)));
                if (column_begin > column_end || row_begin > row_end)
                {
                    continue;
                }
                for (uint32_t row = row_begin; row <= row_end; ++row)
                {
                    for (uint32_t column = column_begin; column <= column_end; ++column)
                    {
                        size_t const p = static_cast<size_t>(row) * g.columns + column;
                        if (s.ocean[p] || s.lake[p] || s.river[p])
                        {
                            continue;
                        }
                        double const dx = grid_x[column] - r.a[0] - image_x;
                        double const dy = grid_y[row] - r.a[1] - image_y;
                        double const projected_t = (dx * r.delta[0] + dy * r.delta[1]) / length2;
                        double const t = closed_river_endpoints
                            ? std::clamp(projected_t, 0.0, 1.0)
                            : projected_t;
                        double const z = r.water_stage0 + t * (r.water_stage1 - r.water_stage0);
                        double const depth = z - g.bed_m[p];
                        bool resolved = depth > 0.0;
                        if (resolved && require_float32_river_depth)
                        {
                            double const magnitude = std::max({std::abs(z), std::abs(g.bed_m[p]), 1.0});
                            float const float_magnitude = static_cast<float>(magnitude);
                            double const ulp = static_cast<double>(std::nextafter(
                            float_magnitude, std::numeric_limits<float>::infinity()))
                            - static_cast<double>(float_magnitude);
                            resolved = depth > std::max(1e-6, 2.0 * ulp);
                        }
                        if ((closed_river_endpoints || (t >= 0.0 && t <= 1.0))
                            && std::hypot(dx - t * r.delta[0], dy - t * r.delta[1]) <= r.halfwidth
                            && resolved)
                        {
                            s.river[p] = 1U;
                            s.river_owner_id[p] = r.start;
                            s.water_level_m[p] = static_cast<float>(z);
                            s.water_depth_m[p] = static_cast<float>(depth);
                        }
                    }
                }
            }
        }
    }
    return s;
}

std::expected<V9WaterLocalSupport, V9WaterFailure> queryV9WaterLocalSupport(V9WaterEquilibrium const& e,
                            V9WaterFixedSupport const& fixed,
                            V9WaterQueryGrid const& query,
                            V9WaterQueryModel const& model)
{
    return queryLocalSupportImpl(e, fixed, query, model, false, false, false);
}

std::expected<V9WaterLocalSupport, V9WaterFailure> queryV21WaterLocalSupport(V9WaterEquilibrium const& e,
                            V9WaterFixedSupport const& fixed,
                            V9WaterQueryGrid const& query,
                            V9WaterQueryModel const& model)
{
    return queryLocalSupportImpl(e, fixed, query, model, true, true, true);
}
std::expected<V9WaterPointSupport, V9WaterFailure>
queryV9WaterLocalPointSupport(V9WaterEquilibrium const& e, V9WaterFixedSupport const& fixed, double x_m,
                              double y_m, double bed_m, std::span<V9RefinementReach const> reaches,
                              std::span<size_t const> reach_order)
{
    if (!valid(e) || fixed.size != 2 * e.resolution || !std::isfinite(x_m) || !std::isfinite(y_m) ||
        !std::isfinite(bed_m) || reach_order.size() != reaches.size()) {
        return std::unexpected(failure("invalid local water point query"));
    }
    size_t const fixed_extent = static_cast<size_t>(fixed.size) * fixed.size;
    for (auto const& body : fixed.bodies) {
        if (body.permission.size() != fixed_extent) {
            return std::unexpected(failure("invalid fixed permission extent"));
        }
    }
    double const x = periodic(x_m, WORLD), y = periodic(y_m, WORLD);
    int const ix = wrap(static_cast<int>(std::nearbyint(x / (WORLD / fixed.size))), fixed.size);
    int const iy = wrap(static_cast<int>(std::nearbyint(y / (WORLD / fixed.size))), fixed.size);
    size_t const support_index = static_cast<size_t>(iy) * fixed.size + ix;
    int32_t water_owner = -1;
    double water_stage = std::numeric_limits<double>::infinity();
    for (auto const& body : fixed.bodies) {
        if (body.permission[support_index] != 0U && (body.body_id == 0 || domain(e, body.body_id, x, y)) &&
            bed_m < body.stage_m) {
            water_owner = body.body_id;
            water_stage = body.stage_m;
            break;
        }
    }

    double const cell = WORLD / e.resolution;
    double const cx = x / cell, cy = y / cell;
    size_t const native = static_cast<size_t>(wrap(static_cast<int>(std::nearbyint(cy)), e.resolution)) *
                              e.resolution +
                          wrap(static_cast<int>(std::nearbyint(cx)), e.resolution);
    if (e.lake.size() == e.ocean.size() && e.lake_id.size() == e.ocean.size() &&
        e.water_level_m.size() == e.ocean.size() && std::abs(cx - std::nearbyint(cx)) < 1e-10 &&
        std::abs(cy - std::nearbyint(cy)) < 1e-10 && std::abs(bed_m - e.hydraulic_bed_m[native]) < 1e-8) {
        water_owner = -1;
        water_stage = std::numeric_limits<double>::infinity();
        if (e.ocean[native] != 0U) {
            water_owner = 0;
            water_stage = 0;
        }
        if (e.lake[native] != 0U) {
            water_owner = e.lake_id[native];
            water_stage = e.water_level_m[native];
        }
    }

    V9WaterPointSupport support;
    support.ocean = water_owner == 0;
    support.lake = water_owner > 0;
    support.lake_id = std::max(water_owner, 0);
    support.water_level_m = static_cast<float>(water_owner >= 0 ? water_stage : bed_m);
    support.water_depth_m = static_cast<float>(water_owner >= 0 ? water_stage - bed_m : 0.0);

    if (water_owner < 0) {
        for (size_t const edge : reach_order) {
            if (edge >= reaches.size()) {
                return std::unexpected(failure("invalid fixed river reach order"));
            }
            V9RefinementReach const& reach = reaches[edge];
            if (!(reach.length2 > 0.0)) {
                continue;
            }
            auto const [t, distance_squared] = project(reach.a, reach.delta, x, y, WORLD);
            if (distance_squared > reach.halfwidth * reach.halfwidth) {
                continue;
            }
            double const stage = reach.water_stage0 + t * (reach.water_stage1 - reach.water_stage0);
            double const depth = stage - bed_m;
            if (!(depth > 0.0)) {
                continue;
            }
            double const magnitude = std::max({std::abs(stage), std::abs(bed_m), 1.0});
            float const float_magnitude = static_cast<float>(magnitude);
            double const ulp = static_cast<double>(std::nextafter(
                float_magnitude, std::numeric_limits<float>::infinity())) -
                static_cast<double>(float_magnitude);
            double const minimum_depth = std::max(1e-6, 2.0 * ulp);
            if (!(depth > minimum_depth)) {
                continue;
            }
            support.river = true;
            support.river_owner_id = reach.start;
            support.water_level_m = static_cast<float>(stage);
            support.water_depth_m = static_cast<float>(depth);
            break;
        }
    }
    return support;
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
