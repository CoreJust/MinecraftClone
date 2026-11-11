#include "V9Climate.hpp"

#include "V9ClimateQg.hpp"
#include "V9ClimateThermal.hpp"
#include "V9Numerics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <utility>
namespace generation::detail
{
namespace
{
constexpr int32_t N = 32, S = N * N, L = 2 * S, P = 8;
constexpr double DAY = 86'400., DX = 512'000., PHASE = 45 * DAY;
using Array = std::vector<double>;
struct Faces
{
    Array x, y;
};
void check(std::stop_token stop)
{
    if (stop.stop_requested())
        throw std::runtime_error("v9 climate cancelled");
}
Faces weatherFaces(Array const& mass, std::span<const double> u, std::span<const double> v)
{
    Faces faces{Array(L), Array(2 * (N + 1) * N)}, sum{Array(S), Array((N + 1) * N)};
    for (int32_t l = 0; l < 2; ++l)
        for (int32_t y = 0; y < N; ++y)
            for (int32_t x = 0; x < N; ++x)
            {
                int32_t i = l * S + y * N + x, j = l * S + y * N + (x + 1) % N;
                faces.x[i] = .5 * (mass[i] + mass[j]) * .5 * (u[i] + u[j]);
                sum.x[y * N + x] += faces.x[i];
                if (y > 0)
                {
                    int32_t f = l * (N + 1) * N + y * N + x;
                    faces.y[f] = .5 * (mass[i - N] + mass[i]) * .5 * (v[i - N] + v[i]);
                    sum.y[y * N + x] += faces.y[f];
                }
            }
    Array px, py;
    projectV9Flux(sum.x, sum.y, DX, px, py);
    double total = 0.;
    std::array<double, 2> weight{};
    for (int32_t l = 0; l < 2; ++l)
        for (int32_t i = 0; i < S; ++i)
        {
            weight[l] += mass[l * S + i];
            total += mass[l * S + i];
        }
    for (int32_t l = 0; l < 2; ++l)
    {
        weight[l] /= total;
        for (int32_t i = 0; i < S; ++i)
            faces.x[l * S + i] -= weight[l] * (sum.x[i] - px[i]);
        for (int32_t i = 0; i < (N + 1) * N; ++i)
            faces.y[l * (N + 1) * N + i] -= weight[l] * (sum.y[i] - py[i]);
    }
    return faces;
}
struct Operator
{
    std::vector<std::vector<std::pair<int32_t, double>>> rows;
    double rate = 0., mass_residual = 0.;
    Array apply(Array const& a) const
    {
        Array r(L);
        for (int32_t i = 0; i < L; ++i)
            for (auto [j, v] : rows[i])
                r[i] += v * a[j];
        return r;
    }
};
Operator generator(Array const& mass, Faces const& f, Faces const& c)
{
    std::vector<std::map<int32_t, double>> rows(L);
    auto edge = [&](int32_t a, int32_t b, double rate)
    {
        if (!std::isfinite(rate) || rate < 0)
            throw std::runtime_error("v9 invalid transport rate");
        rows[a][a] -= rate;
        rows[b][a] += rate;
    };
    // Same directed donor, then symmetric mixing-ratio conductance, then vertical exchange.
    for (int32_t kind = 0; kind < 3; ++kind)
        for (int32_t axis = 0; axis < 2; ++axis)
            for (int32_t l = 0; l < 2; ++l)
                for (int32_t y = 0; y < N; ++y)
                    for (int32_t x = 0; x < N; ++x)
                    {
                        if (axis == 1 && y == N - 1)
                            continue;
                        int32_t a = l * S + y * N + x,
                            b = axis == 0 ? l * S + y * N + (x + 1) % N : a + N;
                        int32_t face = axis == 0 ? a : l * (N + 1) * N + (y + 1) * N + x;
                        double flow = (axis == 0 ? f.x[face] : f.y[face]) / DX,
                               conductance = (axis == 0 ? c.x[face] : c.y[face]) / DX;
                        if (kind == 0)
                        {
                            if (flow < 0)
                            std::swap(a, b);
                            edge(a, b, std::abs(flow) / mass[a]);
                        }
                        else if (kind == 1)
                            edge(a, b, conductance / mass[a]);
                        else
                            edge(b, a, conductance / mass[b]);
                    }
    for (int32_t y = 0; y < N; ++y)
        for (int32_t x = 0; x < N; ++x)
        {
            int32_t i = y * N + x;
            double j = (f.x[S + i] - f.x[S + y * N + (x + N - 1) % N] + f.y[(N + 1) * N + i + N] -
                        f.y[(N + 1) * N + i]) /
                       DX;
            int32_t a = j >= 0 ? i : S + i, b = j >= 0 ? S + i : i;
            edge(a, b, std::abs(j) / mass[a]);
        }
    Operator result;
    result.rows.resize(L);
    for (int32_t i = 0; i < L; ++i)
    {
        for (auto [j, v] : rows[i])
            result.rows[i].emplace_back(j, v);
        result.rate = std::max(result.rate, -rows[i][i]);
    }
    auto residual = result.apply(mass);
    for (double v : residual)
        result.mass_residual = std::max(result.mass_residual, std::abs(v));
    return result;
}
Faces diffusionFaces(Array const& mass)
{
    Array direct(N), diff(S);
    for (int32_t y = 0; y < N; ++y)
    {
        double r = std::clamp((std::abs(180 * ((y + .5) / N - .5)) - 25) / 10, 0., 1.);
        direct[y] = 1 - r * r * (3 - 2 * r);
    }
    for (int32_t i = 0; i < S; ++i)
        diff[i] = 2.4e13 / (1004 * (mass[i] + mass[S + i]));
    Faces c{Array(L), Array(2 * (N + 1) * N)};
    for (int32_t l = 0; l < 2; ++l)
        for (int32_t y = 0; y < N; ++y)
            for (int32_t x = 0; x < N; ++x)
            {
                int32_t i = y * N + x, j = y * N + (x + 1) % N;
                c.x[l * S + i] = .5 * (mass[l * S + i] + mass[l * S + j]) * .5 *
                            (diff[i] + diff[j]) * (1 - direct[y]) / DX;
                if (y > 0)
                    c.y[l * (N + 1) * N + i] = .5 * (mass[l * S + i - N] + mass[l * S + i]) * .5 *
                            (diff[i - N] + diff[i]) *
                            (1 - .5 * (direct[y - 1] + direct[y])) / DX;
            }
    return c;
}
void equilibrate(Array& vapor, Array& cloud, std::span<const double> sat, double dt, Array& cond,
                 Array& evap)
{
    double decay = std::exp(-dt / 300.);
    for (int32_t i = 0; i < L; ++i)
    {
        double total = vapor[i] + cloud[i], eq = std::min(total, sat[i]),
               updated = eq + (vapor[i] - eq) * decay, transfer = updated - vapor[i];
        vapor[i] = updated;
        cloud[i] = total - updated;
        cond[i] += std::max(-transfer, 0.);
        evap[i] += std::max(transfer, 0.);
    }
}
void field(V9ClimateResult& r, std::string key, Array a, std::vector<uint32_t> shape)
{
    r.fields.insert_or_assign(std::move(key), V9ClimateField{std::move(shape), std::move(a)});
}
} // namespace
std::expected<V9ClimateResult, V9ClimateFailure> generateV9Climate(
    std::span<const double> bed, uint64_t seed, std::stop_token stop,
    V9ClimateProgressCallback progress)
{
    if (bed.size() != 256 * 256 ||
        !std::all_of(bed.begin(), bed.end(), [](double v) { return std::isfinite(v); }))
        return std::unexpected(V9ClimateFailure::InvalidInput);
    try
    {
        check(stop);
        if (progress)
            progress({V9ClimateProgress::Stage::Thermal, 0, 1});
        auto thermal = computeV9ClimateThermal(
            bed, stop,
            [&](char const* detail)
            {
                if (progress)
                    progress({V9ClimateProgress::Stage::Thermal, 0, 1, detail});
            });
        if (progress)
            progress({V9ClimateProgress::Stage::Thermal, 1, 1});
        check(stop);
        auto& t = thermal.fields;
        auto const& mass = t.at("layer_mass");
        auto const& sat = t.at("layer_saturation_kg_m2");
        auto weather =
            computeV9Weather(t.at("theta_C"), t.at("ground_m"), seed, v9DryWaveSpeedSquared(), stop,
                             [&](uint32_t day)
                             {
                            if (progress)
                            progress({V9ClimateProgress::Stage::Weather, day, 180});
                             });
        V9ClimateResult result;
        for (double value : t.at("combined_energy_residual"))
            result.thermal_residual_W_m2 = std::max(result.thermal_residual_W_m2, std::abs(value));
        auto const& wet = t.at("wet_fraction");
        auto const& local = t.at("local_effective_C");
        auto const& ps = t.at("reference_ps");
        Faces diffusion = diffusionFaces(mass);
        std::vector<Faces> weather_faces;
        weather_faces.reserve(180);
        for (int32_t d = 0; d < 180; ++d)
        {
            check(stop);
            weather_faces.push_back(weatherFaces(mass,
                            std::span(weather.u_anomaly).subspan(d * L, L),
                            std::span(weather.v_anomaly).subspan(d * L, L)));
        }
        struct Daily
        {
            Operator op;
            Array exchange;
            int32_t substeps;
        };
        std::vector<Daily> days;
        days.reserve(360);
        for (int32_t d = 0; d < 360; ++d)
        {
            check(stop);
            int32_t p = d / 45;
            Faces flow = weather_faces[d % 180];
            for (int32_t i = 0; i < L; ++i)
                flow.x[i] += t.at("carrier_flux_x")[p * L + i];
            for (int32_t i = 0; i < 2 * (N + 1) * N; ++i)
                flow.y[i] += t.at("carrier_flux_y")[p * 2 * (N + 1) * N + i];
            auto op = generator(mass, flow, diffusion);
            result.dry_mass_residual_kg_m2_s =
                std::max(result.dry_mass_residual_kg_m2_s, op.mass_residual);
            Array exchange(S);
            for (int32_t y = 0; y < N; ++y)
                for (int32_t x = 0; x < N; ++x)
                {
                    int32_t i = y * N + x;
                    double u = .5 * (flow.x[i] + flow.x[y * N + (x + N - 1) % N]) / mass[i],
                           v = .5 * (flow.y[i + N] + flow.y[i]) / mass[i];
                    double tc = local[p * S + i], es = 611.2 * std::exp(17.67 * tc / (tc + 243.5)),
                           qg = .622 * es / (ps[i] - .378 * es);
                    exchange[i] = wet[i] * .0015 * std::hypot(u, v) * qg / sat[p * L + i];
                }
            int32_t count = std::max(1, int32_t(std::ceil(DAY * op.rate / .8)));
            days.push_back({std::move(op), std::move(exchange), count});
        }
        if (result.dry_mass_residual_kg_m2_s > 1e-10)
            throw std::runtime_error("v9 nonstationary dry carrier");
        Array V(sat.begin(), sat.begin() + L), C(L), soil(S);
        for (double& a : V)
            a *= .5;
        std::map<std::string, Array> annual;
        result.minimum_reservoir_kg_m2 = 1e99;
        for (int32_t year = 0; year < 3; ++year)
        {
            annual.clear();
            for (auto name :
                 {"rain_kg_m2_s", "ocean_evap_kg_m2_s", "runoff_kg_m2_s", "land_evap_kg_m2_s",
                  "bucket_kg_m2", "phase_soil_begin_kg_m2", "phase_soil_end_kg_m2"})
                annual[name] = Array(P * S);
            for (auto name :
                 {"layer_vapor_kg_m2", "layer_cloud_kg_m2", "condensation_kg_m2_s",
                  "cloud_evap_kg_m2_s", "layer_rain_kg_m2_s", "vapor_transport_tendency_kg_m2_s",
                  "cloud_transport_tendency_kg_m2_s", "phase_vapor_begin_kg_m2",
                  "phase_vapor_end_kg_m2", "phase_cloud_begin_kg_m2", "phase_cloud_end_kg_m2"})
                annual[name] = Array(P * L);
            auto& annual_rain = annual.at("rain_kg_m2_s");
            auto& annual_ocean_evap = annual.at("ocean_evap_kg_m2_s");
            auto& annual_runoff = annual.at("runoff_kg_m2_s");
            auto& annual_land_evap = annual.at("land_evap_kg_m2_s");
            auto& annual_bucket = annual.at("bucket_kg_m2");
            auto& phase_soil_begin = annual.at("phase_soil_begin_kg_m2");
            auto& phase_soil_end = annual.at("phase_soil_end_kg_m2");
            auto& annual_layer_vapor = annual.at("layer_vapor_kg_m2");
            auto& annual_layer_cloud = annual.at("layer_cloud_kg_m2");
            auto& annual_condensation = annual.at("condensation_kg_m2_s");
            auto& annual_cloud_evap = annual.at("cloud_evap_kg_m2_s");
            auto& annual_layer_rain = annual.at("layer_rain_kg_m2_s");
            auto& vapor_transport_tendency = annual.at("vapor_transport_tendency_kg_m2_s");
            auto& cloud_transport_tendency = annual.at("cloud_transport_tendency_kg_m2_s");
            auto& phase_vapor_begin = annual.at("phase_vapor_begin_kg_m2");
            auto& phase_vapor_end = annual.at("phase_vapor_end_kg_m2");
            auto& phase_cloud_begin = annual.at("phase_cloud_begin_kg_m2");
            auto& phase_cloud_end = annual.at("phase_cloud_end_kg_m2");
            double initial = 0.;
            for (double a : V)
                initial += a;
            for (double a : C)
                initial += a;
            for (double a : soil)
                initial += a;
            double net = 0.;
            for (int32_t day = 0; day < 360; ++day)
            {
                check(stop);
                int32_t p = day / 45;
                auto const& daily = days[day];
                double dt = DAY / daily.substeps, f = -std::expm1(-dt / (4 * 3600.)),
                       fb = -std::expm1(-dt / (30 * DAY));
                if (day % 45 == 0)
                {
                    std::copy(V.begin(), V.end(),
                              phase_vapor_begin.begin() + p * L);
                    std::copy(C.begin(), C.end(),
                              phase_cloud_begin.begin() + p * L);
                    std::copy(soil.begin(), soil.end(),
                              phase_soil_begin.begin() + p * S);
                }
                for (int32_t step = 0; step < daily.substeps; ++step)
                {
                    auto tv = daily.op.apply(V), tc = daily.op.apply(C);
                    for (int32_t i = 0; i < L; ++i)
                    {
                        V[i] += dt * tv[i];
                        C[i] += dt * tc[i];
                        vapor_transport_tendency[p * L + i] += dt * tv[i];
                        cloud_transport_tendency[p * L + i] += dt * tc[i];
                    }
                    Array eo(S), cond(L), evap(L), rain(S), fall(L);
                    for (int32_t i = 0; i < S; ++i)
                    {
                        eo[i] = std::max(sat[p * L + i] - V[i], 0.) *
                            (-std::expm1(-dt * daily.exchange[i]));
                        V[i] += eo[i];
                    }
                    equilibrate(V, C, std::span(sat).subspan(p * L, L), dt / 2, cond, evap);
                    for (int32_t i = 0; i < L; ++i)
                    {
                        fall[i] = C[i] * f;
                        C[i] -= fall[i];
                        rain[i % S] += fall[i];
                    }
                    for (int32_t i = 0; i < S; ++i)
                    {
                        soil[i] += (1 - wet[i]) * rain[i];
                        double overflow = std::max(soil[i] - 150 * (1 - wet[i]), 0.);
                        soil[i] -= overflow;
                        double drained = soil[i] * fb;
                        soil[i] -= drained;
                        double et = .25 * drained, runoff = .75 * drained + overflow;
                        V[i] += et;
                        annual_rain[p * S + i] += rain[i];
                        annual_ocean_evap[p * S + i] += eo[i];
                        annual_runoff[p * S + i] += runoff;
                        annual_land_evap[p * S + i] += et;
                        net += eo[i] - wet[i] * rain[i] - runoff;
                        annual_bucket[p * S + i] += dt * soil[i];
                        result.minimum_reservoir_kg_m2 =
                            std::min(result.minimum_reservoir_kg_m2, soil[i]);
                    }
                    equilibrate(V, C, std::span(sat).subspan(p * L, L), dt / 2, cond, evap);
                    for (int32_t i = 0; i < L; ++i)
                    {
                        annual_layer_vapor[p * L + i] += dt * V[i];
                        annual_layer_cloud[p * L + i] += dt * C[i];
                        annual_condensation[p * L + i] += cond[i];
                        annual_cloud_evap[p * L + i] += evap[i];
                        annual_layer_rain[p * L + i] += fall[i];
                        result.minimum_reservoir_kg_m2 =
                            std::min({result.minimum_reservoir_kg_m2, V[i], C[i]});
                        result.max_supersaturation_ratio =
                            std::max(result.max_supersaturation_ratio, V[i] / sat[p * L + i]);
                    }
                    result.max_cfl = std::max(result.max_cfl, dt * daily.op.rate);
                    ++result.moisture_substeps;
                }
                std::copy(V.begin(), V.end(), phase_vapor_end.begin() + p * L);
                std::copy(C.begin(), C.end(), phase_cloud_end.begin() + p * L);
                std::copy(soil.begin(), soil.end(), phase_soil_end.begin() + p * S);
                if (progress)
                    progress(
                        {V9ClimateProgress::Stage::Moisture, uint32_t(year * 360 + day + 1), 1080});
            }
            double final = 0.;
            for (double a : V)
                final += a;
            for (double a : C)
                final += a;
            for (double a : soil)
                final += a;
            result.water_budget_grid_sum_max_kg_m2 =
                std::max(result.water_budget_grid_sum_max_kg_m2, std::abs(final - initial - net));
            for (int32_t p = 0; p < P; ++p)
            {
                for (int32_t i = 0; i < L; ++i)
                {
                    int32_t j = p * L + i;
                    double vr = phase_vapor_end[j] -
                            phase_vapor_begin[j] -
                            vapor_transport_tendency[j] +
                            annual_condensation[j] - annual_cloud_evap[j];
                    if (i < S)
                        vr -= annual_ocean_evap[p * S + i] +
                              annual_land_evap[p * S + i];
                    double cr = phase_cloud_end[j] -
                            phase_cloud_begin[j] -
                            cloud_transport_tendency[j] -
                            annual_condensation[j] +
                            annual_cloud_evap[j] + annual_layer_rain[j];
                    result.local_phase_budget_max_kg_m2_s =
                        std::max({result.local_phase_budget_max_kg_m2_s, std::abs(vr) / PHASE,
                            std::abs(cr) / PHASE});
                }
                for (int32_t i = 0; i < S; ++i)
                {
                    int32_t j = p * S + i;
                    double sr = phase_soil_end[j] -
                            phase_soil_begin[j] -
                            (1 - wet[i]) * annual_rain[j] +
                            annual_land_evap[j] + annual_runoff[j];
                    result.local_phase_budget_max_kg_m2_s =
                        std::max(result.local_phase_budget_max_kg_m2_s, std::abs(sr) / PHASE);
                }
            }
            for (auto& [key, a] : annual)
                if (!key.starts_with("phase_"))
                    for (double& v : a)
                        v /= PHASE;
        }
        if (result.minimum_reservoir_kg_m2 < -1e-10)
            throw std::runtime_error("v9 negative reservoir");
        for (auto& [key, a] : t)
        {
            std::vector<uint32_t> shape;
            if (a.size() == S)
                shape = {N, N};
            else if (a.size() == L)
                shape = {2, N, N};
            else if (a.size() == P * S)
                shape = {P, N, N};
            else if (a.size() == P * L)
                shape = {P, 2, N, N};
            else if (a.size() == P * (N + 1) * N)
                shape = {P, N + 1, N};
            else if (a.size() == P * 2 * (N + 1) * N)
                shape = {P, 2, N + 1, N};
            else
                continue;
            field(result, key, std::move(a), std::move(shape));
        }
        for (auto& [key, a] : annual)
        {
            bool layer = a.size() == P * L;
            field(result, key, std::move(a),
                  layer ? std::vector<uint32_t>{P, 2, N, N} : std::vector<uint32_t>{P, N, N});
        }
        auto const& vapor = result.fields.at("layer_vapor_kg_m2").values;
        auto const& cloud = result.fields.at("layer_cloud_kg_m2").values;
        auto const& saturation = result.fields.at("layer_saturation_kg_m2").values;
        Array rh(P * S), cloud_total(P * S), layer_rh(P * L), water(P * L), capacity(S),
            latent(P * S);
        for (int32_t i = 0; i < P * L; ++i)
        {
            layer_rh[i] = vapor[i] / saturation[i];
            water[i] = vapor[i] + cloud[i];
        }
        for (int32_t p = 0; p < P; ++p)
            for (int32_t i = 0; i < S; ++i)
            {
                rh[p * S + i] = (vapor[p * L + i] + vapor[p * L + S + i]) /
                            (saturation[p * L + i] + saturation[p * L + S + i]);
                cloud_total[p * S + i] = cloud[p * L + i] + cloud[p * L + S + i];
            }
        field(result, "column_vapor_RH", std::move(rh), {P, N, N});
        field(result, "cloud_water_kg_m2", std::move(cloud_total), {P, N, N});
        field(result, "layer_vapor_RH", std::move(layer_rh), {P, 2, N, N});
        field(result, "layer_water_kg_m2", std::move(water), {P, 2, N, N});
        auto const& wet_result = result.fields.at("wet_fraction").values;
        for (int32_t i = 0; i < S; ++i)
            capacity[i] = 150 * (1 - wet_result[i]);
        field(result, "bucket_capacity_kg_m2", std::move(capacity), {N, N});
        for (int32_t p = 0; p < P; ++p)
            for (int32_t i = 0; i < S; ++i)
            {
                auto const& cond = result.fields.at("condensation_kg_m2_s").values;
                auto const& evap = result.fields.at("cloud_evap_kg_m2_s").values;
                latent[p * S + i] =
                    2.5e6 *
                    (cond[p * L + i] + cond[p * L + S + i] - evap[p * L + i] - evap[p * L + S + i] -
                     result.fields.at("ocean_evap_kg_m2_s").values[p * S + i] -
                     result.fields.at("land_evap_kg_m2_s").values[p * S + i]);
            }
        field(result, "omitted_latent_heating_W_m2", std::move(latent), {P, N, N});
        Array baseu(P * S), basev(P * S), wu(180 * S), wv(180 * S), u(P * S), v(P * S), meanu(S),
            meanv(S), displayu(24 * S), displayv(24 * S), times(24);
        auto const& mass_result = result.fields.at("layer_mass").values;
        auto const& fx = result.fields.at("carrier_flux_x").values;
        auto const& fy = result.fields.at("carrier_flux_y").values;
        for (int32_t p = 0; p < P; ++p)
            for (int32_t y = 0; y < N; ++y)
                for (int32_t x = 0; x < N; ++x)
                {
                    int32_t i = y * N + x;
                    baseu[p * S + i] =
                        .5 * (fx[p * L + i] + fx[p * L + y * N + (x + N - 1) % N]) / mass_result[i];
                    basev[p * S + i] =
                        .5 * (fy[p * 2 * (N + 1) * N + i + N] + fy[p * 2 * (N + 1) * N + i]) /
                        mass_result[i];
                }
        for (int32_t d = 0; d < 180; ++d)
            for (int32_t y = 0; y < N; ++y)
                for (int32_t x = 0; x < N; ++x)
                {
                    int32_t i = y * N + x;
                    wu[d * S + i] =
                        .5 * (weather_faces[d].x[i] + weather_faces[d].x[y * N + (x + N - 1) % N]) /
                        mass_result[i];
                    wv[d * S + i] =
                        .5 * (weather_faces[d].y[i + N] + weather_faces[d].y[i]) / mass_result[i];
                    meanu[i] += wu[d * S + i] / 180;
                    meanv[i] += wv[d * S + i] / 180;
                }
        u = baseu;
        v = basev;
        for (int32_t p = 0; p < P; ++p)
            for (int32_t d = 0; d < 45; ++d)
                for (int32_t i = 0; i < S; ++i)
                {
                    u[p * S + i] += wu[((p * 45 + d) % 180) * S + i] / 45;
                    v[p * S + i] += wv[((p * 45 + d) % 180) * S + i] / 45;
                }
        for (int32_t d = 0; d < 24; ++d)
        {
            int32_t day = d * 180 / 24;
            times[d] = day;
            std::copy_n(wu.begin() + day * S, S, displayu.begin() + d * S);
            std::copy_n(wv.begin() + day * S, S, displayv.begin() + d * S);
        }
        result.fields.erase("u");
        result.fields.erase("v");
        field(result, "u", std::move(u), {P, N, N});
        field(result, "v", std::move(v), {P, N, N});
        field(result, "weather_base_u_m_s", std::move(baseu), {P, N, N});
        field(result, "weather_base_v_m_s", std::move(basev), {P, N, N});
        field(result, "weather_u_anomaly_m_s", std::move(displayu), {24, N, N});
        field(result, "weather_v_anomaly_m_s", std::move(displayv), {24, N, N});
        field(result, "qg_mean_u_m_s", std::move(meanu), {N, N});
        field(result, "qg_mean_v_m_s", std::move(meanv), {N, N});
        field(result, "weather_sample_time_days", std::move(times), {24});
        field(result, "qg_daily_u_anomaly_m_s", std::move(weather.u_anomaly), {180, 2, N, N});
        field(result, "qg_daily_v_anomaly_m_s", std::move(weather.v_anomaly), {180, 2, N, N});
        field(result, "qg_final_PV", std::move(weather.final_pv), {2, N, N});
        auto count = uint32_t(weather.adaptive_dt_s.size());
        field(result, "qg_adaptive_dt_s", std::move(weather.adaptive_dt_s), {count});
        for (auto const& [key, f] : result.fields)
            if (!std::all_of(f.values.begin(), f.values.end(),
                             [](double x) { return std::isfinite(x); }))
                throw std::runtime_error("v9 nonfinite climate field");
        check(stop);
        if (progress)
            progress({V9ClimateProgress::Stage::Complete, 1, 1});
        return result;
    }
    catch (std::exception const&)
    {
        return std::unexpected(stop.stop_requested() ? V9ClimateFailure::Cancelled
                            : V9ClimateFailure::NumericalFailure);
    }
}
} // namespace generation::detail
