#include "Numerics.hpp"
#include "NumpyRandom.hpp"

#include <generation/Generation.hpp>
#include <generation/GenerationStages.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <utility>
#include <vector>

namespace generation::detail
{
namespace
{

constexpr double PI = std::numbers::pi_v<double>;

struct FaceFields
{
    DoubleField u;
    DoubleField v;
};

struct AdvectionParameters final
{
    uint32_t resolution;
    uint32_t steps;
};

[[nodiscard]]
double maximumFaceSpeed(FaceFields const& faces)
{
    double maximum = 0.0;
    for (size_t index = 0U; index < faces.u.size(); ++index) {
        maximum = std::max(maximum, std::abs(faces.u[index]) + std::abs(faces.v[index]));
    }
    return maximum;
}

[[nodiscard]]
DoubleField faceFlux(
    std::span<double const> field,
    FaceFields const& faces,
    uint32_t resolution
)
{
    size_t const count = field.size();
    DoubleField x_flux(count);
    DoubleField y_flux(count);
    DoubleField divergence(count);
    for (uint32_t y = 0U; y < resolution; ++y) {
        size_t const row = static_cast<size_t>(y) * resolution;
        size_t const previous_row = static_cast<size_t>((y + resolution - 1U) % resolution) * resolution;
        for (uint32_t x = 0U; x < resolution; ++x) {
            size_t const index = row + x;
            size_t const previous_x = (x + resolution - 1U) % resolution;
            x_flux[index] = faces.u[index]
                * (faces.u[index] >= 0.0 ? field[row + previous_x] : field[index]);
            y_flux[index] = faces.v[index]
                * (faces.v[index] >= 0.0 ? field[previous_row + x] : field[index]);
        }
    }
    for (uint32_t y = 0U; y < resolution; ++y) {
        size_t const row = static_cast<size_t>(y) * resolution;
        size_t const next_row = static_cast<size_t>((y + 1U) % resolution) * resolution;
        for (uint32_t x = 0U; x < resolution; ++x) {
            size_t const index = row + x;
            size_t const next_x = (x + 1U) % resolution;
            divergence[index] = x_flux[row + next_x] - x_flux[index]
                + y_flux[next_row + x] - y_flux[index];
        }
    }
    return divergence;
}

[[nodiscard]]
FaceFields makeOceanFaces(
    std::span<uint8_t const> ocean,
    std::span<double const> streamfunction,
    uint32_t resolution
)
{
    size_t const count = ocean.size();
    DoubleField psi(streamfunction.begin(), streamfunction.end());
    for (uint32_t y = 0U; y < resolution; ++y) {
        size_t const row = static_cast<size_t>(y) * resolution;
        size_t const previous_row = static_cast<size_t>((y + resolution - 1U) % resolution) * resolution;
        for (uint32_t x = 0U; x < resolution; ++x) {
            size_t const index = row + x;
            size_t const previous_x = (x + resolution - 1U) % resolution;
            bool const wet_vertex = ocean[index] != 0U
                && ocean[previous_row + x] != 0U
                && ocean[row + previous_x] != 0U
                && ocean[previous_row + previous_x] != 0U;
            if (!wet_vertex) {
                psi[index] = 0.0;
            }
        }
    }

    FaceFields faces{DoubleField(count, 0.0), DoubleField(count, 0.0)};
    for (uint32_t y = 0U; y < resolution; ++y) {
        size_t const row = static_cast<size_t>(y) * resolution;
        size_t const previous_row = static_cast<size_t>((y + resolution - 1U) % resolution) * resolution;
        size_t const next_row = static_cast<size_t>((y + 1U) % resolution) * resolution;
        for (uint32_t x = 0U; x < resolution; ++x) {
            size_t const index = row + x;
            size_t const previous_x = (x + resolution - 1U) % resolution;
            size_t const next_x = (x + 1U) % resolution;
            bool const wet_u_face = ocean[index] != 0U && ocean[row + previous_x] != 0U;
            bool const wet_v_face = ocean[index] != 0U && ocean[previous_row + x] != 0U;
            if (wet_u_face) {
                faces.u[index] = psi[next_row + x] - psi[index];
            }
            if (wet_v_face) {
                faces.v[index] = psi[index] - psi[row + next_x];
            }
        }
    }
    return faces;
}

void advect(
    DoubleField& field,
    FaceFields const& faces,
    AdvectionParameters const& parameters,
    std::stop_token const& cancellation
)
{
    DoubleField u = faces.u;
    DoubleField v = faces.v;
    double const divisor = static_cast<double>(parameters.steps);
    for (double& value : u) {
        value /= divisor;
    }
    for (double& value : v) {
        value /= divisor;
    }
    FaceFields const scaled_faces{.u = std::move(u), .v = std::move(v)};
    for (uint32_t step = 0U; step < parameters.steps; ++step) {
        checkCancellation(cancellation);
        DoubleField const flux = faceFlux(field, scaled_faces, parameters.resolution);
        for (size_t index = 0U; index < field.size(); ++index) {
            field[index] -= flux[index];
        }
    }
}

} // namespace

ClimateFields simulateClimate(
    GenerationConfiguration const& configuration,
    std::span<float const> elevation,
    ClimateParameters const& parameters,
    std::span<uint8_t const> ocean_mask,
    std::span<uint8_t const> lake_mask,
    GenerationStage stage,
    std::stop_token const& cancellation,
    ProgressCallback const& progress
)
{
    checkCancellation(cancellation);
    uint32_t const iterations = parameters.iterations;
    float const sea_level = parameters.sea_level;
    uint32_t const resolution = configuration.resolution;
    size_t const field_size = static_cast<size_t>(resolution) * resolution;
    if (resolution < 8U || field_size > std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument("climate resolution must be at least 8 and fit a 32-bit field size");
    }
    if (elevation.size() != field_size || (!ocean_mask.empty() && ocean_mask.size() != field_size)
        || (!lake_mask.empty() && lake_mask.size() != field_size)) {
        throw std::invalid_argument("climate fields must match the configured square resolution");
    }
    if (iterations == 0U || (configuration.wind_sign != -1 && configuration.wind_sign != 1)) {
        throw std::invalid_argument("climate requires positive iterations and wind sign -1 or +1");
    }
    if (!std::isfinite(sea_level)
        || !std::all_of(elevation.begin(), elevation.end(), [](float value) { return std::isfinite(value); })) {
        throw std::invalid_argument("climate elevation and sea level must be finite");
    }

    ByteField ocean(field_size, 0U);
    ByteField lakes(field_size, 0U);
    for (size_t index = 0U; index < field_size; ++index) {
        ocean[index] = ocean_mask.empty()
            ? static_cast<uint8_t>(elevation[index] <= sea_level)
            : static_cast<uint8_t>(ocean_mask[index] != 0U);
        lakes[index] = !lake_mask.empty() && lake_mask[index] != 0U && ocean[index] == 0U;
    }

    DoubleField insolation(field_size);
    DoubleField base_temperature(field_size);
    DoubleField base_ocean_temperature(field_size);
    for (uint32_t y = 0U; y < resolution; ++y) {
        double const solar = (1.0 - std::cos(2.0 * PI * static_cast<double>(y) / resolution)) * 0.5;
        for (uint32_t x = 0U; x < resolution; ++x) {
            size_t const index = static_cast<size_t>(y) * resolution + x;
            insolation[index] = solar;
            base_ocean_temperature[index] = -18.0 + 48.0 * solar;
            base_temperature[index] = base_ocean_temperature[index]
                - 6.5 * std::max(static_cast<double>(elevation[index]) - sea_level, 0.0);
        }
    }

    NumpyRandom random(configuration.seed);
    double const phase = random.uniform(0.0, 2.0 * PI);
    FloatField const noise = periodicFbm(
        configuration.seed + 1987U,
        FbmParameters{.resolution = resolution, .octaves = 3U, .start_frequency = 2U},
        cancellation
    );
    FloatField smooth_noise = gaussianFilterWrap(
        noise,
        GaussianFilterParameters{.resolution = resolution, .sigma = std::max(1.0, 0.035 * resolution)},
        cancellation
    );
    DoubleField const pressure = gaussianFilterWrap(
        base_temperature,
        GaussianFilterParameters{.resolution = resolution, .sigma = std::max(1.0, 0.07 * resolution)},
        cancellation
    );
    auto const [pressure_x, pressure_y] = gradientPeriodic(pressure, resolution);
    double const speed_scale = 0.42 * resolution / iterations;
    DoubleField wind_u(field_size);
    DoubleField wind_v(field_size);
    for (uint32_t y = 0U; y < resolution; ++y) {
        double const row_term = 0.90 + 0.16 * std::cos(4.0 * PI * y / resolution);
        for (uint32_t x = 0U; x < resolution; ++x) {
            size_t const index = static_cast<size_t>(y) * resolution + x;
            wind_u[index] = configuration.wind_sign * speed_scale * (
                row_term + 0.25 * smooth_noise[index] + 0.0025 * resolution * pressure_y[index]);
            wind_v[index] = configuration.wind_sign * speed_scale * (
                0.23 * std::sin(2.0 * PI * x / resolution + phase) - 0.0025 * resolution * pressure_x[index]);
        }
    }

    FaceFields wind_faces{DoubleField(field_size), DoubleField(field_size)};
    for (uint32_t y = 0U; y < resolution; ++y) {
        size_t const row = static_cast<size_t>(y) * resolution;
        size_t const previous_row = static_cast<size_t>((y + resolution - 1U) % resolution) * resolution;
        for (uint32_t x = 0U; x < resolution; ++x) {
            size_t const index = row + x;
            size_t const previous_x = (x + resolution - 1U) % resolution;
            wind_faces.u[index] = 0.5 * (wind_u[index] + wind_u[row + previous_x]);
            wind_faces.v[index] = 0.5 * (wind_v[index] + wind_v[previous_row + x]);
        }
    }

    auto const [wind_v_x, unused_wind_v_y] = gradientPeriodic(wind_v, resolution);
    auto const [unused_wind_u_x, wind_u_y] = gradientPeriodic(wind_u, resolution);
    (void)unused_wind_v_y;
    (void)unused_wind_u_x;
    DoubleField curl(field_size);
    for (size_t index = 0U; index < field_size; ++index) {
        curl[index] = wind_v_x[index] - wind_u_y[index];
    }
    DoubleField streamfunction = gaussianFilterWrap(
        curl,
        GaussianFilterParameters{.resolution = resolution, .sigma = std::max(1.0, 0.06 * resolution)},
        cancellation
    );
    DoubleField ocean_values(field_size);
    for (size_t index = 0U; index < field_size; ++index) {
        ocean_values[index] = static_cast<double>(ocean[index]);
    }
    DoubleField const ocean_weight_for_current = gaussianFilterWrap(
        ocean_values,
        GaussianFilterParameters{.resolution = resolution, .sigma = std::max(1.0, 0.025 * resolution)},
        cancellation
    );
    for (size_t index = 0U; index < field_size; ++index) {
        streamfunction[index] *= ocean_weight_for_current[index]
            * ocean_weight_for_current[index] * ocean_weight_for_current[index];
    }
    FaceFields current_faces = makeOceanFaces(ocean, streamfunction, resolution);
    double const raw_max = maximumFaceSpeed(current_faces);
    if (configuration.currents && raw_max > 1.0e-12) {
        double const scale = (0.30 * resolution / iterations) / raw_max;
        for (double& value : current_faces.u) value *= scale;
        for (double& value : current_faces.v) value *= scale;
    } else {
        std::fill(current_faces.u.begin(), current_faces.u.end(), 0.0);
        std::fill(current_faces.v.begin(), current_faces.v.end(), 0.0);
    }

    DoubleField ocean_temperature(field_size, 0.0);
    DoubleField moisture(field_size);
    DoubleField rainfall(field_size, 0.0);
    DoubleField evaporation_water(field_size, 0.0);
    for (size_t index = 0U; index < field_size; ++index) {
        if (ocean[index] != 0U) {
            ocean_temperature[index] = base_ocean_temperature[index];
            moisture[index] = 1.0;
            evaporation_water[index] = 1.0;
        } else if (lakes[index] != 0U) {
            moisture[index] = 1.0;
            evaporation_water[index] = 1.0;
        } else {
            moisture[index] = 0.24;
        }
    }
    DoubleField const ocean_target = ocean_temperature;
    uint32_t const current_steps = std::max(1U, static_cast<uint32_t>(std::ceil(maximumFaceSpeed(current_faces) / 0.65)));
    uint32_t const wind_steps = std::max(1U, static_cast<uint32_t>(std::ceil(maximumFaceSpeed(wind_faces) / 0.65)));
    auto const [height_x, height_y] = gradientPeriodic(
        DoubleField(elevation.begin(), elevation.end()), resolution);
    DoubleField removal_fraction(field_size);
    double const evaporation_fraction = std::min(0.08 * 80.0 / iterations, 1.0);
    double const heat_relaxation = std::min(0.020 * 80.0 / iterations, 1.0);
    for (uint32_t y = 0U; y < resolution; ++y) {
        size_t const row = static_cast<size_t>(y) * resolution;
        for (uint32_t x = 0U; x < resolution; ++x) {
            size_t const index = row + x;
            double const uphill = std::max(0.0, wind_u[index] * height_x[index]
                + wind_v[index] * height_y[index]) * static_cast<double>(ocean[index] == 0U);
            double const base_loss = (evaporation_water[index] != 0.0 ? 0.008 : 0.020)
                * 80.0 / iterations;
            removal_fraction[index] = std::clamp(-std::expm1(-base_loss - 1.1 * uphill), 0.0, 1.0);
        }
    }

    uint32_t const progress_period = std::max(1U, iterations / 4U);
    reportProgress(progress, stage, 0U, iterations);
    for (uint32_t step = 0U; step < iterations; ++step) {
        checkCancellation(cancellation);
        if (configuration.currents) {
            advect(
                ocean_temperature,
                current_faces,
                AdvectionParameters{.resolution = resolution, .steps = current_steps},
                cancellation
            );
        }
        for (size_t index = 0U; index < field_size; ++index) {
            ocean_temperature[index] += heat_relaxation * (ocean_target[index] - ocean_temperature[index])
                * static_cast<double>(ocean[index] != 0U);
        }
        advect(
            moisture,
            wind_faces,
            AdvectionParameters{.resolution = resolution, .steps = wind_steps},
            cancellation
        );
        for (size_t index = 0U; index < field_size; ++index) {
            double const removed = moisture[index] * removal_fraction[index];
            moisture[index] -= removed;
            rainfall[index] += removed;
            double const replenished = evaporation_water[index]
                * evaporation_fraction * std::max(1.0 - moisture[index], 0.0);
            moisture[index] += replenished;
        }
        if ((step + 1U) % progress_period == 0U || step + 1U == iterations) {
            reportProgress(progress, stage, step + 1U, iterations);
        }
    }

    DoubleField anomaly(field_size);
    for (size_t index = 0U; index < field_size; ++index) {
        anomaly[index] = (ocean_temperature[index] - ocean_target[index]) * ocean[index];
    }
    double const coastal_sigma = std::max(1.0, 0.012 * resolution);
    GaussianFilterParameters const coastal_filter{
        .resolution = resolution,
        .sigma = coastal_sigma,
    };
    DoubleField const ocean_weight = gaussianFilterWrap(ocean_values, coastal_filter, cancellation);
    DoubleField coastal_anomaly = gaussianFilterWrap(anomaly, coastal_filter, cancellation);
    for (size_t index = 0U; index < field_size; ++index) {
        coastal_anomaly[index] /= std::max(ocean_weight[index], 1.0e-8);
    }

    ClimateFields result{
        .temperature = FloatField(field_size),
        .precipitation = FloatField(field_size),
        .moisture = FloatField(field_size),
        .wind_u = FloatField(field_size),
        .wind_v = FloatField(field_size),
        .current_u = FloatField(field_size),
        .current_v = FloatField(field_size),
        .climate_zone = ByteField(field_size, 0U),
        .snow_mask = ByteField(field_size, 0U),
    };
    for (uint32_t y = 0U; y < resolution; ++y) {
        for (uint32_t x = 0U; x < resolution; ++x) {
            size_t const index = static_cast<size_t>(y) * resolution + x;
            bool const land = ocean[index] == 0U;
            double const influence = std::clamp(ocean_weight[index] / 0.3, 0.0, 1.0);
            double const temperature = land
                ? base_temperature[index] + 0.85 * coastal_anomaly[index] * influence
                : ocean_temperature[index];
            bool const snow = land && temperature < -2.0 && rainfall[index] > 0.025;
            uint8_t zone = 0U;
            if (land && temperature >= 18.0) {
                zone = rainfall[index] < 0.35 ? 1U : 2U;
            } else if (land && temperature < 18.0 && temperature >= 2.0) {
                zone = 3U;
            } else if (land && temperature < 2.0) {
                zone = 4U;
            }
            if (snow) zone = 5U;
            result.temperature[index] = static_cast<float>(temperature);
            result.precipitation[index] = static_cast<float>(rainfall[index]);
            result.moisture[index] = static_cast<float>(moisture[index]);
            result.wind_u[index] = static_cast<float>(wind_u[index]);
            result.wind_v[index] = static_cast<float>(wind_v[index]);
            result.current_u[index] = static_cast<float>(0.5 * (current_faces.u[index]
                + current_faces.u[static_cast<size_t>(y) * resolution + (x + 1U) % resolution]));
            result.current_v[index] = static_cast<float>(0.5 * (current_faces.v[index]
                + current_faces.v[static_cast<size_t>((y + 1U) % resolution) * resolution + x]));
            result.climate_zone[index] = zone;
            result.snow_mask[index] = static_cast<uint8_t>(snow);
        }
    }
    return result;
}

} // namespace generation::detail
