#include "V9WaterRefinement.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace generation::detail
{
namespace
{
double wrap(double x, double period) { return x - std::floor(x / period) * period; }
double cube(double x) { return x * x * x; }
std::array<double, 4> basis(double t)
{
    return {((2 * t - 3) * t * t + 1), ((-2 * t + 3) * t * t), ((t - 2) * t * t + t), ((t - 1) * t * t)};
}
std::pair<double, double> projection(std::array<double, 2> a, std::array<double, 2> delta, double x, double y,
                            double domain)
{
    double const dx = wrap(x - a[0] + domain / 2, domain) - domain / 2;
    double const dy = wrap(y - a[1] + domain / 2, domain) - domain / 2;
    double const t = std::clamp((dx * delta[0] + dy * delta[1]) / (delta[0] * delta[0] + delta[1] * delta[1]), 0., 1.);
    return {t, (dx - t * delta[0]) * (dx - t * delta[0]) + (dy - t * delta[1]) * (dy - t * delta[1])};
}
} // namespace

void V9WaterRefinement::interpolateSlopes(std::vector<int32_t> const& protected_vertices)
{
    fx_.assign(bed_.size(), 0);
    fy_.assign(bed_.size(), 0);
    auto at = [&](int x, int y) { return bed_[wrap(y, n_) * n_ + wrap(x, n_)]; };
    for (uint32_t y = 0; y < n_; ++y) {
        for (uint32_t x = 0; x < n_; ++x) {
            size_t const id = y * n_ + x;
            double const z = bed_[id];
            double const bx = (z - at(int(x) - 1, y)) / cell_, ax = (at(x + 1, y) - z) / cell_;
            double const by = (z - at(x, int(y) - 1)) / cell_, ay = (at(x, y + 1) - z) / cell_;
            fx_[id] = bx * ax > 0 ? 2 * bx * ax / (bx + ax) : 0;
            fy_[id] = by * ay > 0 ? 2 * by * ay / (by + ay) : 0;
        }
    }
    for (int32_t id : protected_vertices) {
        fx_[id] = 0;
        fy_[id] = 0;
    }
    for (uint32_t y = 0; y < n_; ++y) {
        for (uint32_t x = 0; x < n_; ++x) {
            size_t const id = y * n_ + x;
            double scale = 1, z = bed_[id];
            for (int sx : {-1, 1}) {
                for (int sy : {-1, 1}) {
                    double const z10 = at(int(x) + sx, y), z01 = at(x, int(y) + sy), z11 = at(int(x) + sx, int(y) + sy);
                    double const lo = std::min({z, z10, z01, z11}), hi = std::max({z, z10, z01, z11});
                    for (double d : {sx * fx_[id] * cell_ / 3, sy * fy_[id] * cell_ / 3,
                            (sx * fx_[id] + sy * fy_[id]) * cell_ / 3}) {
                        double const limit = d == 0 ? 1 : (d > 0 ? hi - z : z - lo) / std::abs(d);
                        scale = std::min(scale, std::clamp(limit, 0., 1.));
                    }
                }
            }
            // Scaling does not feed other vertices: each incident-cell bound uses this node alone.
            fx_[id] *= scale;
            fy_[id] *= scale;
        }
    }
}

double V9WaterRefinement::interpolate(double x, double y) const
{
    x = wrap(x, domain_) / cell_;
    y = wrap(y, domain_) / cell_;
    auto const ix = static_cast<uint32_t>(std::floor(x)), iy = static_cast<uint32_t>(std::floor(y));
    auto const hx = basis(x - ix), hy = basis(y - iy);
    double z = 0;
    for (uint32_t j = 0; j < 2; ++j) {
        for (uint32_t i = 0; i < 2; ++i) {
            size_t const id = ((iy + j) % n_) * n_ + (ix + i) % n_;
            z += bed_[id] * hx[i] * hy[j] + cell_ * fx_[id] * hx[2 + i] * hy[j] + cell_ * fy_[id] * hx[i] * hy[2 + j];
        }
    }
    return z;
}
int V9WaterRefinement::bin(double x, double y) const
{
    return static_cast<int>(std::floor(wrap(y, domain_) / 256)) * bins_ +
           static_cast<int>(std::floor(wrap(x, domain_) / 256));
}
std::pair<double, int32_t> V9WaterRefinement::standing(double x, double y) const
{
    x = wrap(x, domain_) / cell_;
    y = wrap(y, domain_) / cell_;
    auto const ix = static_cast<uint32_t>(std::floor(x)), iy = static_cast<uint32_t>(std::floor(y));
    int32_t const owner = owners_[2 * (iy * n_ + ix) + (x - ix < y - iy)];
    return {stages_[owner], owner};
}
double V9WaterRefinement::ridgeBarrier(double z, double x, double y) const
{
    auto const found = ridge_index_.find(bin(x, y));
    if (found == ridge_index_.end()) {
        return z;
    }
    std::vector<double> uplifts;
    double peak = 0;
    for (size_t edge : found->second) {
        auto const& r = ridges_[edge];
        auto const [t, d2] = projection(r.a, r.delta, x, y, domain_);
        double const weight = cube(std::max(1 - d2 / (96 * 96), 0.));
        double const q = t * t * (3 - 2 * t), height = bed_[r.v0] + q * (bed_[r.v1] - bed_[r.v0]);
        double const drop = 8 * std::max(std::abs(bed_[r.v1] - bed_[r.v0]), 64.) * d2 / (96 * 96);
        double const d = height - drop - z;
        double const uplift = weight * .5 * (d + std::sqrt(d * d + 1e-12));
        uplifts.push_back(uplift);
        peak = std::max(peak, uplift);
    }
    double sum = (1. - uplifts.size()) * std::exp(-peak / 2e-7);
    for (double uplift : uplifts) {
        sum += std::exp((uplift - peak) / 2e-7);
    }
    return z + peak + 2e-7 * std::log(sum);
}
double V9WaterRefinement::base(double x, double y) const { return ridgeBarrier(interpolate(x, y), x, y); }

double V9WaterRefinement::v21FixedSupportBase(double plain_base_blocks, double x, double y) const
{
    return ridgeBarrier(plain_base_blocks, x, y);
}

std::expected<V9WaterRefinement, V9WaterFailure>
V9WaterRefinement::create(V9WaterEquilibrium const& w, V9WaterRouting const& routing, double repose_slope)
{
    auto fail = [](char const* message) {
        return std::unexpected(V9WaterFailure{V9WaterFailure::Kind::InvalidInput, message});
    };
    size_t const size = size_t(w.resolution) * w.resolution;
    if (!w.resolution || w.hydraulic_bed_m.size() != size || w.domain_m < 256 || std::fmod(w.domain_m, 256) != 0 ||
        routing.downstream.size() != size || routing.river.size() != size ||
        routing.river_virtual_through_lake.size() != size || routing.river_stage_m.size() != size ||
        routing.river_depth_m.size() != size || routing.river_width_m.size() != size ||
        routing.discharge_m3_s.size() != size || w.triangle_owner.size() != 2 * size ||
        w.triangle_stage_m.size() != 2 * size || w.triangle_basin_owner.size() != 2 * size ||
        w.vertex_basin_owner.size() != size || !std::isfinite(repose_slope) || repose_slope <= 0) {
        return fail("invalid hydraulic refinement dimensions or domain");
    }
    V9WaterRefinement out;
    out.has_native_rivers_ = std::any_of(routing.river.begin(), routing.river.end(), [](uint8_t v) { return v != 0; });
    out.n_ = w.resolution;
    out.domain_ = w.domain_m;
    out.cell_ = w.domain_m / w.resolution;
    out.bins_ = static_cast<int>(w.domain_m / 256);
    out.repose_ = repose_slope;
    out.bed_ = w.hydraulic_bed_m;
    out.owners_ = w.triangle_basin_owner;
    out.vertex_owners_ = w.vertex_basin_owner;
    for (double z : out.bed_) {
        if (!std::isfinite(z)) {
            return fail("finite square canonical bed required");
        }
    }
    int32_t max_owner = 0;
    for (int32_t owner : out.owners_) {
        if (owner < 0) {
            return fail("negative canonical basin owner");
        }
        max_owner = std::max(max_owner, owner);
    }
    for (auto const& body : w.bodies) {
        max_owner = std::max(max_owner, body.id);
    }
    out.stages_.assign(size_t(max_owner) + 1, 0);
    std::set<int32_t> active, critical;
    for (auto const& body : w.bodies) {
        if (body.id <= 0) {
            return fail("invalid body identity");
        }
        active.insert(body.id);
        out.stages_[body.id] = body.stage_m;
        for (size_t id = 0; id < size; ++id) {
            if (std::abs(out.bed_[id] - body.spill_m) < 1e-9) {
                critical.insert(static_cast<int32_t>(id));
            }
        }
    }
    if (w.bodies.empty()) {
        for (size_t tri = 0; tri < 2 * size; ++tri) {
            int32_t const owner = w.triangle_owner[tri];
            if (owner > 0) {
                active.insert(owner);
                out.stages_[owner] = w.triangle_stage_m[tri];
            }
        }
    }
    std::set<std::pair<int32_t, int32_t>> flow, candidates, active_candidates;
    for (size_t id = 0; id < size; ++id) {
        int32_t const target = routing.downstream[id];
        if (routing.river[id] && target >= 0) {
            if (target >= static_cast<int32_t>(size)) {
                return fail("unresolved river receiver");
            }
            flow.emplace(std::min(int32_t(id), target), std::max(int32_t(id), target));
        }
    }
    auto idAt = [&](int x, int y) { return int32_t(wrap(y, out.n_) * out.n_ + wrap(x, out.n_)); };
    auto addCandidate = [&](int32_t a, int32_t b, int32_t left, int32_t right) {
        if (left != right && (left > 0 || right > 0)) {
            candidates.emplace(std::min(a, b), std::max(a, b));
            if (active.contains(left) || active.contains(right)) {
                active_candidates.emplace(std::min(a, b), std::max(a, b));
            }
        }
    };
    for (uint32_t y = 0; y < out.n_; ++y) {
        for (uint32_t x = 0; x < out.n_; ++x) {
            int32_t const id = idAt(x, y), east = idAt(x + 1, y), south = idAt(x, y + 1), diag = idAt(x + 1, y + 1);
            addCandidate(id, diag, out.owners_[2 * id], out.owners_[2 * id + 1]);
            addCandidate(id, east, out.owners_[2 * id], out.owners_[2 * idAt(x, int(y) - 1) + 1]);
            addCandidate(id, south, out.owners_[2 * id + 1], out.owners_[2 * idAt(int(x) - 1, y)]);
        }
    }
    auto indexGeometry = [&](auto const& geometry, auto& index, double radius, size_t edge) {
        int const lx = int(std::floor((std::min(geometry.a[0], geometry.a[0] + geometry.delta[0]) - radius) / 256));
        int const hx = int(std::floor((std::max(geometry.a[0], geometry.a[0] + geometry.delta[0]) + radius) / 256));
        int const ly = int(std::floor((std::min(geometry.a[1], geometry.a[1] + geometry.delta[1]) - radius) / 256));
        int const hy = int(std::floor((std::max(geometry.a[1], geometry.a[1] + geometry.delta[1]) + radius) / 256));
        for (int by = ly; by <= hy; ++by) {
            for (int bx = lx; bx <= hx; ++bx) {
                index[int(wrap(by, out.bins_)) * out.bins_ + int(wrap(bx, out.bins_))].push_back(edge);
            }
        }
    };
    auto uniqueIndex = [](auto& index) {
        for (auto& [key, ids] : index) {
            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        }
    };
    auto buildRidges = [&](bool final) {
        out.ridges_.clear();
        out.ridge_index_.clear();
        std::set<int32_t> constraints = critical;
        if (final) {
            constraints.clear();
            for (auto const& contact : out.contacts_) {
                constraints.insert(contact.vertex_index);
            }
        }
        for (auto const& [v0, v1] : candidates) {
            if (flow.contains({v0, v1}) || (final && !active_candidates.contains({v0, v1})) ||
                (!final && !critical.contains(v0) && !critical.contains(v1))) {
                continue;
            }
            Ridge r{v0, v1, {double(v0 % out.n_) * out.cell_, double(v0 / out.n_) * out.cell_}, {}};
            r.delta = {wrap(double(v1 % out.n_) * out.cell_ - r.a[0] + out.domain_ / 2, out.domain_) - out.domain_ / 2,
                       wrap(double(v1 / out.n_) * out.cell_ - r.a[1] + out.domain_ / 2, out.domain_) - out.domain_ / 2};
            out.ridges_.push_back(r);
            constraints.insert(v0);
            constraints.insert(v1);
            indexGeometry(r, out.ridge_index_, 96, out.ridges_.size() - 1);
        }
        uniqueIndex(out.ridge_index_);
        if (!final) {
            out.interpolation_constraints_.assign(size, 0U);
            for (int32_t vertex : constraints) {
                out.interpolation_constraints_[static_cast<size_t>(vertex)] = 1U;
            }
        }
        out.interpolateSlopes(std::vector<int32_t>(constraints.begin(), constraints.end()));
    };
    buildRidges(false);
    for (size_t id = 0; id < size; ++id) {
        int32_t const target = routing.downstream[id];
        if (!routing.river[id] || target < 0 || routing.river_virtual_through_lake[id]) {
            continue;
        }
        V9RefinementReach r;
        r.start = static_cast<int32_t>(id);
        r.target = target;
        r.a = {double(id % out.n_) * out.cell_, double(id / out.n_) * out.cell_};
        r.delta = {wrap(double(target % out.n_) * out.cell_ - r.a[0] + out.domain_ / 2, out.domain_) - out.domain_ / 2,
                   wrap(double(target / out.n_) * out.cell_ - r.a[1] + out.domain_ / 2, out.domain_) - out.domain_ / 2};
        r.length2 = r.delta[0] * r.delta[0] + r.delta[1] * r.delta[1];
        r.grade0 = out.bed_[id];
        r.grade1 = out.bed_[target];
        r.water_stage0 = routing.river_stage_m[id];
        r.water_stage1 = routing.river_stage_m[target];
        r.halfwidth = std::max(double(routing.river_width_m[id]) / 2, .05);
        r.discharge_m3_s = static_cast<float>(routing.discharge_m3_s[id]);
        if (r.length2 <= 0 || r.grade1 > r.grade0 + 1e-8) {
            return fail("nonlocal zero or rising river reach unsupported");
        }
        auto wet = [&](double t) {
            double const x = r.a[0] + t * r.delta[0], y = r.a[1] + t * r.delta[1];
            return out.base(x, y) < out.standing(x, y).first;
        };
        for (int sample = 0; sample <= 64; ++sample) {
            if (!wet(double(sample) / 64)) {
                continue;
            }
            if (sample == 0) {
                break;
            }
            double lo = double(sample - 1) / 64, hi = double(sample) / 64;
            for (int iteration = 0; iteration < 40; ++iteration) {
                double const mid = (lo + hi) / 2;
                if (wet(mid)) {
                    hi = mid;
                } else {
                    lo = mid;
                }
            }
            double const level = out.standing(r.a[0] + hi * r.delta[0], r.a[1] + hi * r.delta[1]).first;
            r.delta[0] *= hi;
            r.delta[1] *= hi;
            r.length2 = r.delta[0] * r.delta[0] + r.delta[1] * r.delta[1];
            r.water_stage1 = level;
            r.grade1 = level - std::max(double(routing.river_depth_m[id]), double(routing.river_depth_m[target]));
            r.shore_truncated = true;
            break;
        }
        if (r.grade1 > r.grade0 + 1e-8) {
            return fail("shore-truncated bed grade rises downstream");
        }
        double relief = 0;
        for (int32_t vertex : {r.start, r.target}) {
            double lo = std::numeric_limits<double>::infinity(), hi = -lo;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    double const z = out.bed_[idAt(int(vertex % out.n_) + dx, int(vertex / out.n_) + dy)];
                    lo = std::min(lo, z);
                    hi = std::max(hi, z);
                }
            }
            relief = std::max(relief, hi - lo);
        }
        r.support = std::max(24., r.halfwidth + relief / repose_slope + 24.);
        out.reaches_.push_back(r);
        indexGeometry(r, out.reach_index_, r.support, out.reaches_.size() - 1);
    }
    uniqueIndex(out.reach_index_);
    for (int32_t vertex : critical) {
        V9ProtectedContact contact{
            vertex, {double(vertex % out.n_) * out.cell_, double(vertex / out.n_) * out.cell_}, out.bed_[vertex], 64};
        double distance = std::numeric_limits<double>::infinity();
        for (auto const& r : out.reaches_) {
            auto const [t, d2] = projection(r.a, r.delta, contact.xy_m[0], contact.xy_m[1], out.domain_);
            distance = std::min(distance, std::sqrt(d2));
            if (d2 < 1e-10 && std::abs(r.grade0 + t * (r.grade1 - r.grade0) - contact.height_m) > 1e-7) {
                return fail("active lake contact conflicts with rivercore grade; redirect/reconcile required");
            }
        }
        contact.radius_m = std::min(64., distance * .4);
        if (contact.radius_m >= 1e-5) {
            out.contacts_.push_back(contact);
        }
    }
    buildRidges(true);
    out.v21_interpolation_constraints_.assign(size, 0U);
    for (auto const& [v0, v1] : candidates) {
        std::pair<int32_t, int32_t> const edge{v0, v1};
        if (flow.contains(edge) || !active_candidates.contains(edge)) {
            continue;
        }
        out.v21_interpolation_constraints_[static_cast<size_t>(v0)] = 1U;
        out.v21_interpolation_constraints_[static_cast<size_t>(v1)] = 1U;
    }
    for (V9ProtectedContact const& contact : out.contacts_) {
        out.v21_interpolation_constraints_[static_cast<size_t>(contact.vertex_index)] = 1U;
    }
    return out;
}

double V9WaterRefinement::apply(double native_zero, double native_lod, double x, double y, uint32_t lod) const
{
    double const canonical = base(x, y);
    if (lod == 0) {
        return canonical;
    }
    auto const [stage, owner] = standing(x, y);
    double const raw = native_lod - native_zero, limit = .5 * std::abs(canonical - stage);
    double const bed = canonical + raw * limit * limit / (raw * raw + limit * limit + 1e-24);
    double result = bed, closest = std::numeric_limits<double>::infinity(), center = 0, half = 0;
    auto const found = reach_index_.find(bin(x, y));
    if (found != reach_index_.end()) {
        for (size_t edge : found->second) {
            auto const& r = reaches_[edge];
            auto const [t, d2] = projection(r.a, r.delta, x, y, domain_);
            double const distance = std::sqrt(d2), grade = r.grade0 + t * (r.grade1 - r.grade0);
            double const blend = cube(std::max(1 - (distance / 32) * (distance / 32), 0.));
            bool const same = owner == vertex_owners_[r.start];
            double const cut = std::clamp(bed - (grade + repose_ * distance), 0., 16.);
            result -= std::min(cut * blend * same, std::max(result - (bed - 16), 0.));
            if (distance < closest) {
                closest = distance;
                center = grade;
                half = r.halfwidth;
            }
        }
    }
    bool const active = closest < std::min(half, .5);
    double blend = 0;
    if (active) {
        double const u = std::clamp(closest / std::min(half, .5), 0., 1.);
        blend = cube(1 - u * u);
        result = center * blend + result * (1 - blend);
    }
    result = ridgeBarrier(result, x, y);
    double weight = 1;
    for (auto const& contact : contacts_) {
        double const dx = wrap(x - contact.xy_m[0] + domain_ / 2, domain_) - domain_ / 2;
        double const dy = wrap(y - contact.xy_m[1] + domain_ / 2, domain_) - domain_ / 2;
        double const u = std::clamp(std::hypot(dx, dy) / contact.radius_m, 0., 1.);
        weight *= 1 - cube(1 - u * u);
    }
    result = canonical + (result - canonical) * weight;
    if (active) {
        result = center * blend + result * (1 - blend);
    }
    return result;
}
} // namespace generation::detail
