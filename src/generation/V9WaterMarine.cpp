#include "V9WaterMarine.hpp"

#include "gswteos-10.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace generation::detail
{
namespace
{
V9MarineFailure failure(V9MarineFailure::Kind kind, char const* message) { return {kind, message}; }
double salinity(double liquid, double salt) { return liquid + salt > 0 ? 1000 * salt / (liquid + salt) : 0; }
} // namespace
std::expected<double, V9MarineFailure> v9MarineFreezing(double sa)
{
    if (!std::isfinite(sa) || sa < 0 || sa > 90) {
        return std::unexpected(failure(V9MarineFailure::Kind::ChemistryDomain,
                            "GSW reference chemistry requires finite 0..90 g/kg at p=0, air-free."));
    }
    return gsw_t_freezing_poly(sa, 0, 0);
}
static std::expected<V9MarinePhase, V9MarineFailure> phaseEquilibrium(double mass, double salt, double energy, bool wet,
                            uint32_t minimum_iterations)
{
    constexpr double CP = 3'990, CI = 2'100, LF = 334'000;
    if (!std::isfinite(mass) || !std::isfinite(salt) || !std::isfinite(energy) || mass < 0 || salt < 0 ||
        (wet && mass <= 0)) {
        return std::unexpected(failure(V9MarineFailure::Kind::InvalidInput, "Invalid phase inventory."));
    }
    double sa = salinity(mass, salt);
    auto initial_tf = v9MarineFreezing(sa);
    if (!initial_tf) {
        return std::unexpected(initial_tf.error());
    }
    double tf = *initial_tf, liquid = mass, ice = 0;
    double temperature = mass > 0 ? energy / (CP * mass) : 0;
    double threshold = CP * mass * tf;
    bool mixed = wet && energy < threshold;
    bool frozen = wet && salt == 0 && energy <= CI * mass * tf - LF * mass;
    uint32_t iterations = 0;
    if (frozen) {
        ice = mass;
        liquid = 0;
        temperature = (energy + LF * mass) / (CI * mass);
    } else if (mixed && salt == 0) {
        ice = (threshold - energy) / (LF + (CP - CI) * tf);
        liquid = mass - ice;
    } else if (mixed) {
        auto liquid_at = [&](double value) {
            double l = std::min(mass, salt * (1000 - value) / value);
            for (int i = 0; i < 4 && salinity(l, salt) > 90; ++i) {
                l = std::nextafter(l, std::numeric_limits<double>::infinity());
            }
            return l;
        };
        auto residual = [&](double value) {
            double l = liquid_at(value);
            return (CI * mass + (CP - CI) * l) * gsw_t_freezing_poly(value, 0, 0) - LF * (mass - l) - energy;
        };
        double low = sa, high = 90;
        double tolerance = 2e-6 + 2e-12 * std::max(std::abs(energy), LF * mass);
        if (residual(high) > tolerance) {
            return std::unexpected(
                failure(V9MarineFailure::Kind::ChemistryDomain, "Energy requires brine above 90 g/kg."));
        }
        double guess_i = std::min(mass, std::max(0., (threshold - energy) / (LF + (CP - CI) * tf)));
        double guess_l = std::max(mass - guess_i, liquid_at(high));
        double guess = std::min(high, std::max(low, salinity(guess_l, salt)));
        for (iterations = 1; iterations <= 48; ++iterations) {
            double r = residual(guess);
            if (std::abs(r) <= tolerance && iterations >= minimum_iterations) {
                break;
            }
            if (r > 0) {
                low = guess;
            }
            if (r < 0) {
                high = guess;
            }
            double l = liquid_at(guess), dtf = 0;
            gsw_t_freezing_first_derivatives_poly(guess, 0, 0, &dtf, nullptr);
            double dl = -1000 * salt / guess / guess;
            double derivative =
                dl * ((CP - CI) * gsw_t_freezing_poly(guess, 0, 0) + LF) + (CI * mass + (CP - CI) * l) * dtf;
            double next = guess - r / derivative;
            guess = next > low && next < high && std::isfinite(next) ? next : .5 * (low + high);
        }
        if (iterations > 48) {
            return std::unexpected(failure(V9MarineFailure::Kind::IterationLimit, "Phase inversion did not converge."));
        }
        liquid = liquid_at(guess);
        ice = mass - liquid;
    }
    sa = salinity(liquid, salt);
    auto final_tf = v9MarineFreezing(sa);
    if (!final_tf) {
        return std::unexpected(final_tf.error());
    }
    tf = *final_tf;
    if (mixed && !frozen) {
        temperature = tf;
    }
    double residual = wet ? (CP * liquid + CI * ice) * temperature - LF * ice - energy : 0;
    if (std::abs(residual) > 4e-6 + 4e-12 * std::max(std::abs(energy), LF * mass)) {
        return std::unexpected(
            failure(V9MarineFailure::Kind::Residual, "Phase enthalpy inversion exceeded tolerance."));
    }
    return V9MarinePhase{liquid, ice, temperature, sa, tf, residual, iterations};
}
std::expected<V9MarinePhase, V9MarineFailure> v9MarinePhaseEquilibrium(double mass, double salt, double energy,
                            bool wet)
{
    return phaseEquilibrium(mass, salt, energy, wet, 0);
}
std::expected<std::vector<V9MarinePhase>, V9MarineFailure> v9MarinePhaseEquilibrium(std::span<double const> total,
                            std::span<double const> salt,
                            std::span<double const> enthalpy,
                            std::span<uint8_t const> wet)
{
    if (total.size() != salt.size() || total.size() != enthalpy.size() || total.size() != wet.size()) {
        return std::unexpected(failure(V9MarineFailure::Kind::InvalidInput, "Phase inventory registrations differ."));
    }
    std::vector<V9MarinePhase> result;
    result.reserve(total.size());
    uint32_t shared_iterations = 0;
    for (size_t i = 0; i < total.size(); ++i) {
        auto value = phaseEquilibrium(total[i], salt[i], enthalpy[i], wet[i] != 0, 0);
        if (!value) {
            return std::unexpected(value.error());
        }
        shared_iterations = std::max(shared_iterations, value->iterations);
        result.push_back(*value);
    }
    // The NumPy loop keeps updating previously converged cells. Their later
    // residual can temporarily leave tolerance, so find a shared fixed-point
    // iteration rather than merely taking the first individual stopping times.
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < total.size(); ++i) {
            if (result[i].iterations == 0 || result[i].iterations == shared_iterations) {
                continue;
            }
            auto value = phaseEquilibrium(total[i], salt[i], enthalpy[i], wet[i] != 0, shared_iterations);
            if (!value) {
                return std::unexpected(value.error());
            }
            result[i] = *value;
            if (value->iterations > shared_iterations) {
                shared_iterations = value->iterations;
                changed = true;
            }
        }
    }
    return result;
}
std::expected<V9MarineSalt, V9MarineFailure> deriveV9MarineSalt(V9MarineSaltInput const& in)
{
    size_t m = in.resolution, n = m * m, phases = in.phase_count;
    auto finite = [](std::span<double const> a) {
        return std::all_of(a.begin(), a.end(), [](double v) { return std::isfinite(v); });
    };
    bool north_extra = in.north_volume_m3_s.size() == phases * (m + 1) * m;
    if (m < 2 || (phases != 4 && phases != 8) || in.runoff_resolution < m || in.runoff_resolution % m != 0 ||
        in.wet_fraction.size() != n || in.east_volume_m3_s.size() != phases * n ||
        (!north_extra && in.north_volume_m3_s.size() != phases * n) || in.rain_kg_m2_s.size() != phases * n ||
        in.evaporation_kg_m2_s.size() != phases * n ||
        in.runoff_outlets_m_s.size() != size_t(in.runoff_resolution) * in.runoff_resolution ||
        !std::isfinite(in.horizontal_metric_scale) || in.horizontal_metric_scale <= 0 || !finite(in.wet_fraction) ||
        !finite(in.east_volume_m3_s) || !finite(in.north_volume_m3_s) || !finite(in.rain_kg_m2_s) ||
        !finite(in.evaporation_kg_m2_s) || !finite(in.runoff_outlets_m_s)) {
        return std::unexpected(failure(V9MarineFailure::Kind::InvalidInput, "Invalid marine forcing registration."));
    }
    if (std::any_of(in.wet_fraction.begin(), in.wet_fraction.end(), [](double v) { return v < 0 || v > 1; })) {
        return std::unexpected(failure(V9MarineFailure::Kind::InvalidInput, "Wet fraction outside [0,1]."));
    }
    constexpr double REFERENCE = 1000. * 35 / 965;
    double cell_area = std::pow(65'536. / double(m), 2), dt = 360. * 86'400 / double(phases);
    double scale = in.horizontal_metric_scale * in.horizontal_metric_scale;
    std::vector<double> volume(n), salt(n), runoff(n);
    size_t ratio = in.runoff_resolution / m;
    for (size_t y = 0; y < m; ++y) {
        for (size_t x = 0; x < m; ++x) {
            size_t i = y * m + x;
            volume[i] = cell_area * in.wet_fraction[i] * 20;
            salt[i] = volume[i] * REFERENCE;
            for (size_t a = 0; a < ratio; ++a) {
                for (size_t b = 0; b < ratio; ++b) {
                    runoff[i] += in.runoff_outlets_m_s[(y * ratio + a) * in.runoff_resolution + x * ratio + b];
                }
            }
            runoff[i] /= double(ratio * ratio);
        }
    }
    V9MarineSalt result;
    result.initial_salt_kg = std::accumulate(salt.begin(), salt.end(), 0.);
    result.salinity_g_kg.reserve(phases * n);
    struct Entry
    {
        size_t source;
        double value;
    };
    for (size_t phase = 0; phase < phases; ++phase) {
        std::vector<std::vector<Entry>> rows(n);
        std::vector<double> diagonal(n, 1), net(n), incoming(n), export_rate(n), rhs(n);
        for (size_t axis = 0; axis < 2; ++axis) {
            for (size_t y = 0; y < m; ++y) {
                for (size_t x = 0; x < m; ++x) {
                    if (axis == 1 && y == m - 1) {
                        continue;
                    }
                    size_t i = y * m + x, j = axis == 0 ? y * m + (x + 1) % m : (y + 1) * m + x;
                    if (volume[i] <= 0 || volume[j] <= 0) {
                        continue;
                    }
                    size_t north_offset = north_extra ? phase * (m + 1) * m + (y + 1) * m + x : phase * n + i;
                    double q =
                        (axis == 0 ? in.east_volume_m3_s[phase * n + i] : in.north_volume_m3_s[north_offset]) / scale;
                    size_t src = q >= 0 ? i : j, dst = q >= 0 ? j : i;
                    double rate = dt * std::abs(q) / volume[src];
                    diagonal[src] += rate;
                    rows[dst].push_back({src, -rate});
                    net[src] -= std::abs(q);
                    net[dst] += std::abs(q);
                }
            }
        }
        for (size_t i = 0; i < n; ++i) {
            double fresh = volume[i] > 0 ? cell_area * (in.rain_kg_m2_s[phase * n + i] * in.wet_fraction[i] / 1000 -
                            in.evaporation_kg_m2_s[phase * n + i] / 1000 + runoff[i])
                            : 0;
            double vertical = -net[i] - fresh;
            incoming[i] = std::max(vertical, 0.);
            export_rate[i] = std::max(-vertical, 0.);
            if (volume[i] > 0) {
                diagonal[i] += dt * export_rate[i] / volume[i];
            }
            rhs[i] = salt[i] + dt * incoming[i] * REFERENCE;
            result.water_rate_residual_m3_s =
                std::max(result.water_rate_residual_m3_s, std::abs(net[i] + fresh + vertical));
        }
        // The transport matrix is a nonsingular M-matrix (identity plus
        // conservative donor export). Gauss-Seidel solves that same sparse
        // implicit system; an unconverged candidate is never returned.
        bool converged = false;
        double relative = 0;
        for (uint32_t iteration = 0; iteration < 100'000; ++iteration) {
            for (size_t i = 0; i < n; ++i) {
                double value = rhs[i];
                for (auto const& e : rows[i]) {
                    value -= e.value * salt[e.source];
                }
                salt[i] = value / diagonal[i];
            }
            relative = 0;
            for (size_t i = 0; i < n; ++i) {
                double value = diagonal[i] * salt[i] - rhs[i];
                for (auto const& e : rows[i]) {
                    value += e.value * salt[e.source];
                }
                relative = std::max(relative, std::abs(value) / std::max(1., std::abs(rhs[i])));
            }
            if (relative <= 2e-14) {
                converged = true;
                break;
            }
        }
        if (!converged) {
            return std::unexpected(failure(V9MarineFailure::Kind::IterationLimit,
                            "Implicit marine sparse solve did not meet residual tolerance."));
        }
        result.max_linear_relative_residual = std::max(result.max_linear_relative_residual, relative);
        for (size_t i = 0; i < n; ++i) {
            result.vertical_salt_import_kg += dt * incoming[i] * REFERENCE;
            if (volume[i] > 0) {
                result.vertical_salt_export_kg += dt * export_rate[i] * salt[i] / volume[i];
            }
            result.salinity_g_kg.push_back(volume[i] > 0 ? 1000 * salt[i] / (1000 * volume[i] + salt[i]) : 0);
        }
    }
    result.final_salt_kg = std::accumulate(salt.begin(), salt.end(), 0.);
    result.salt_residual_kg = result.final_salt_kg - result.initial_salt_kg -
                            (result.vertical_salt_import_kg - result.vertical_salt_export_kg);
    return result;
}
} // namespace generation::detail
