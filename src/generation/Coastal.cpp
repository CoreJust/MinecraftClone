#include "Numerics.hpp"

#include <generation/GenerationStages.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace generation::detail
{

namespace
{

static constexpr double WORLD_SIZE = 65'536.0;
static constexpr double PI = 3.141'592'653'589'793'238'462'643'383'279'502'88;

[[nodiscard]]
double wrapCoordinate(double value)
{
    double wrapped = std::fmod(value, WORLD_SIZE);
    if (wrapped < 0.0) {
        wrapped += WORLD_SIZE;
    }
    return wrapped;
}

[[nodiscard]]
// Match NumPy's contiguous float32 reduction tree used by the prototype's std().
float pairwiseSumFloat32(std::span<float const> values)
{
    static constexpr size_t PAIRWISE_BLOCK_SIZE = 128U;
    if (values.size() < 8U) {
        float sum = -0.0F;
        for (float value : values) {
            sum += value;
        }
        return sum;
    }
    if (values.size() <= PAIRWISE_BLOCK_SIZE) {
        std::array<float, 8U> partials{};
        for (size_t lane = 0U; lane < partials.size(); ++lane) {
            partials[lane] = values[lane];
        }
        size_t index = partials.size();
        for (; index < values.size() - values.size() % partials.size(); index += partials.size()) {
            for (size_t lane = 0U; lane < partials.size(); ++lane) {
                partials[lane] += values[index + lane];
            }
        }
        float sum = ((partials[0] + partials[1]) + (partials[2] + partials[3]))
            + ((partials[4] + partials[5]) + (partials[6] + partials[7]));
        for (; index < values.size(); ++index) {
            sum += values[index];
        }
        return sum;
    }

    size_t split = values.size() / 2U;
    split -= split % 8U;
    return pairwiseSumFloat32(values.first(split))
        + pairwiseSumFloat32(values.subspan(split));
}

[[nodiscard]]
FloatField makeDissectionField(
    uint64_t seed,
    uint32_t domain,
    uint32_t resolution,
    std::stop_token cancellation
)
{
    FloatField broad = periodicFbm(seed + domain, resolution, 3U, 4U, cancellation);
    FloatField fine = periodicFbm(seed + domain + 1U, resolution, 3U, 16U, cancellation);
    uint64_t const field_size = broad.size();
    FloatField values(field_size);
    for (uint64_t index = 0U; index < field_size; ++index) {
        values[index] = 0.35F * broad[index] + 0.65F * fine[index];
    }
    float const mean = pairwiseSumFloat32(values) / static_cast<float>(field_size);
    FloatField squared_deviations(field_size);
    for (uint64_t index = 0U; index < field_size; ++index) {
        float const delta = values[index] - mean;
        squared_deviations[index] = delta * delta;
    }
    float const variance = pairwiseSumFloat32(squared_deviations)
        / static_cast<float>(field_size);
    float const standard_deviation = std::sqrt(variance);
    float const normalizer = std::max(standard_deviation, 1.0e-8F);
    FloatField result(field_size);
    for (uint64_t index = 0U; index < field_size; ++index) {
        result[index] = std::clamp(values[index] / normalizer, -2.5F, 2.5F);
    }
    return result;
}

} // namespace

CoastalFields elaborateCoasts(
    GenerationConfiguration const& configuration,
    std::span<float const> elevation,
    std::span<float const> activity,
    std::stop_token cancellation,
    ProgressCallback const& progress
)
{
    checkCancellation(cancellation);
    uint32_t const resolution = configuration.resolution;
    uint64_t const field_size = static_cast<uint64_t>(resolution) * resolution;
    if (resolution < 8U || field_size > std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument("coastal resolution must be at least 8 and fit a 32-bit field size");
    }
    if (elevation.size() != field_size || activity.size() != field_size) {
        throw std::invalid_argument("coastal fields must match the configured square resolution");
    }
    if (!std::all_of(elevation.begin(), elevation.end(), [](float value) {
            return std::isfinite(value);
        })
        || !std::all_of(activity.begin(), activity.end(), [](float value) {
            return std::isfinite(value);
        })) {
        throw std::invalid_argument("coastal fields must contain only finite values");
    }

    DoubleField bed(field_size);
    DoubleField activity_values(field_size);
    for (uint64_t index = 0U; index < field_size; ++index) {
        bed[index] = elevation[index];
        activity_values[index] = activity[index];
    }
    double const active_sigma = std::max(0.008 * resolution, 0.5);
    DoubleField active = gaussianFilterWrap(activity_values, resolution, active_sigma, cancellation);

    CoastalFields result{
        .elevation = FloatField(field_size),
        .displacement = FloatField(field_size),
        .roughness_potential = FloatField(field_size),
    };
    DoubleField displacement_x(field_size, 0.0);
    DoubleField displacement_y(field_size, 0.0);
    DoubleField x_coordinates(field_size);
    DoubleField y_coordinates(field_size);
    DoubleField amplitude(field_size);
    DoubleField nearshore(field_size);
    double const cell_size = WORLD_SIZE / resolution;
    for (uint32_t row = 0U; row < resolution; ++row) {
        double const world_y = static_cast<double>(row) * cell_size;
        double const cold_component = 0.5 + 0.5 * std::cos(2.0 * PI * world_y / WORLD_SIZE);
        double const cold_row = cold_component * cold_component;
        for (uint32_t column = 0U; column < resolution; ++column) {
            uint64_t const index = static_cast<uint64_t>(row) * resolution + column;
            double const world_x = static_cast<double>(column) * cell_size;
            double const elevation_value = bed[index];
            x_coordinates[index] = world_x;
            y_coordinates[index] = world_y;
            double const active_value = std::clamp(active[index], 0.0, 1.0);
            amplitude[index] = 70.0 + 650.0 * active_value + 1'400.0 * cold_row;
            double const normalized_height = elevation_value / 1.2;
            double const nearshore_value = 1.0 - normalized_height * normalized_height;
            nearshore[index] = std::abs(elevation_value) < 1.2
                ? nearshore_value * nearshore_value
                : 0.0;
        }
    }

    if (configuration.coastal_elaboration) {
        FloatField dissection_x = makeDissectionField(
            configuration.seed,
            811U,
            resolution,
            cancellation
        );
        FloatField dissection_y = makeDissectionField(
            configuration.seed,
            821U,
            resolution,
            cancellation
        );
        for (uint64_t index = 0U; index < field_size; ++index) {
            if (index % 4'096U == 0U) {
                checkCancellation(cancellation);
            }
            displacement_x[index] = amplitude[index] * nearshore[index] * dissection_x[index];
            displacement_y[index] = amplitude[index] * nearshore[index] * dissection_y[index];
        }
    }

    DoubleField sample_x(field_size);
    DoubleField sample_y(field_size);
    for (uint64_t index = 0U; index < field_size; ++index) {
        if (index % 4'096U == 0U) {
            checkCancellation(cancellation);
        }
        sample_x[index] = wrapCoordinate(x_coordinates[index] + displacement_x[index]);
        sample_y[index] = wrapCoordinate(y_coordinates[index] + displacement_y[index]);
        result.displacement[index] = static_cast<float>(std::hypot(
            displacement_x[index],
            displacement_y[index]
        ));
        result.roughness_potential[index] = static_cast<float>(amplitude[index]);
    }
    for (uint64_t index = 0U; index < field_size; ++index) {
        if (index % 4'096U == 0U) {
            checkCancellation(cancellation);
        }
        result.elevation[index] = static_cast<float>(bilinearPeriodic(
            bed,
            resolution,
            sample_x[index],
            sample_y[index]
        ));
    }
    reportProgress(progress, GenerationStage::CoastalElaboration, 1U, 1U);
    return result;
}

} // namespace generation::detail
