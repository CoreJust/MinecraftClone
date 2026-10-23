#include "Numerics.hpp"

#include "NumpyRandom.hpp"

#include <generation/Generation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

struct GridResolution final
{
    uint32_t value;
};

size_t fieldSize(uint32_t resolution)
{
    if (resolution == 0U)
    {
        throw std::invalid_argument("field resolution must be positive");
    }
    return static_cast<size_t>(resolution) * resolution;
}

void validateFieldSize(size_t actual, uint32_t resolution)
{
    if (actual != fieldSize(resolution))
    {
        throw std::invalid_argument("field size does not match resolution");
    }
}

int64_t wrappedIndex(int64_t index, GridResolution resolution)
{
    int64_t const extent = static_cast<int64_t>(resolution.value);
    int64_t const remainder = index % extent;
    return remainder < 0 ? remainder + extent : remainder;
}

int filterStart(uint32_t filter_size)
{
    return -static_cast<int>(filter_size / 2U);
}

int filterEnd(uint32_t filter_size)
{
    return static_cast<int>((filter_size - 1U) / 2U);
}

double numpyPairwiseSum(std::span<double const> values)
{
    constexpr size_t BLOCK_SIZE = 128U;
    if (values.size() < 8U)
    {
        double total = -0.0;
        for (double value : values)
        {
            total += value;
        }
        return total;
    }
    if (values.size() <= BLOCK_SIZE)
    {
        std::array<double, 8U> sums;
        for (size_t lane = 0U; lane < sums.size(); ++lane)
        {
            sums[lane] = values[lane];
        }

        size_t index = sums.size();
        size_t const vector_end = values.size() - values.size() % sums.size();
        for (; index < vector_end; index += sums.size())
        {
            for (size_t lane = 0U; lane < sums.size(); ++lane)
            {
                sums[lane] += values[index + lane];
            }
        }

        double total = ((sums[0U] + sums[1U]) + (sums[2U] + sums[3U]))
            + ((sums[4U] + sums[5U]) + (sums[6U] + sums[7U]));
        for (; index < values.size(); ++index)
        {
            total += values[index];
        }
        return total;
    }

    size_t const split = values.size() / 2U - (values.size() / 2U) % 8U;
    return numpyPairwiseSum(values.first(split)) + numpyPairwiseSum(values.subspan(split));
}

std::vector<double> gaussianWeights(double sigma)
{
    if (!std::isfinite(sigma) || sigma <= 0.0)
    {
        throw std::invalid_argument("Gaussian sigma must be finite and positive");
    }

    constexpr double TRUNCATE = 4.0;
    int const radius = static_cast<int>(std::floor(TRUNCATE * sigma + 0.5));
    std::vector<double> weights(static_cast<size_t>(radius) * 2U + 1U);
    double const sigma_squared = sigma * sigma;
    for (int offset = -radius; offset <= radius; ++offset)
    {
        double const exponent = (-0.5 / sigma_squared) * static_cast<double>(offset * offset);
        double const weight = std::exp(exponent);
        weights[static_cast<size_t>(static_cast<int64_t>(offset) + radius)] = weight;
    }
    double const total = numpyPairwiseSum(weights);
    for (double& weight : weights)
    {
        weight /= total;
    }
    return weights;
}

template <typename T>
std::vector<T> gaussianAxis(
    std::span<T const> values,
    uint32_t resolution,
    std::span<double const> weights,
    bool rows,
    std::stop_token const& cancellation
)
{
    int const radius = static_cast<int>(weights.size() / 2U);
    std::vector<T> output(values.size());
    for (uint32_t y = 0U; y < resolution; ++y)
    {
        if ((y & 15U) == 0U)
        {
            checkCancellation(cancellation);
        }
        for (uint32_t x = 0U; x < resolution; ++x)
        {
            double sum = 0.0;
            for (int offset = -radius; offset <= radius; ++offset)
            {
                uint32_t sample_x = x;
                uint32_t sample_y = y;
                if (rows)
                {
                    sample_y = static_cast<uint32_t>(wrappedIndex(
                        static_cast<int64_t>(y) + offset,
                        GridResolution{.value = resolution}
                    ));
                }
                else
                {
                    sample_x = static_cast<uint32_t>(wrappedIndex(
                        static_cast<int64_t>(x) + offset,
                        GridResolution{.value = resolution}
                    ));
                }
                size_t const index = static_cast<size_t>(sample_y) * resolution + sample_x;
                double const weight = weights[
                    static_cast<size_t>(offset) + static_cast<size_t>(radius)
                ];
                sum += static_cast<double>(values[index]) * weight;
            }
            output[static_cast<size_t>(y) * resolution + x] = static_cast<T>(sum);
        }
    }
    return output;
}

struct AxisFilterParameters final
{
    uint32_t resolution;
    uint32_t filter_size;
    bool rows;
};

DoubleField uniformAxis(
    std::span<double const> values,
    AxisFilterParameters const& parameters,
    std::stop_token const& cancellation
)
{
    std::vector<double> output(values.size());
    int const first_offset = filterStart(parameters.filter_size);
    int const last_offset = filterEnd(parameters.filter_size);
    double const divisor = static_cast<double>(parameters.filter_size);

    for (uint32_t y = 0U; y < parameters.resolution; ++y)
    {
        if ((y & 15U) == 0U)
        {
            checkCancellation(cancellation);
        }
        for (uint32_t x = 0U; x < parameters.resolution; ++x)
        {
            double sum = 0.0;
            for (int offset = first_offset; offset <= last_offset; ++offset)
            {
                uint32_t const sample_x = parameters.rows
                    ? x
                    : static_cast<uint32_t>(wrappedIndex(
                        static_cast<int64_t>(x) + offset,
                        GridResolution{.value = parameters.resolution}
                    ));
                uint32_t const sample_y = parameters.rows
                    ? static_cast<uint32_t>(wrappedIndex(
                        static_cast<int64_t>(y) + offset,
                        GridResolution{.value = parameters.resolution}
                    ))
                    : y;
                sum += values[static_cast<size_t>(sample_y) * parameters.resolution + sample_x];
            }
            output[static_cast<size_t>(y) * parameters.resolution + x] = sum / divisor;
        }
    }
    return output;
}

ByteField maximumAxis(
    std::span<uint8_t const> values,
    AxisFilterParameters const& parameters,
    std::stop_token const& cancellation
)
{
    std::vector<uint8_t> output(values.size());
    int const first_offset = filterStart(parameters.filter_size);
    int const last_offset = filterEnd(parameters.filter_size);

    for (uint32_t y = 0U; y < parameters.resolution; ++y)
    {
        if ((y & 15U) == 0U)
        {
            checkCancellation(cancellation);
        }
        for (uint32_t x = 0U; x < parameters.resolution; ++x)
        {
            uint8_t maximum = 0U;
            for (int offset = first_offset; offset <= last_offset; ++offset)
            {
                uint32_t const sample_x = parameters.rows
                    ? x
                    : static_cast<uint32_t>(wrappedIndex(
                        static_cast<int64_t>(x) + offset,
                        GridResolution{.value = parameters.resolution}
                    ));
                uint32_t const sample_y = parameters.rows
                    ? static_cast<uint32_t>(wrappedIndex(
                        static_cast<int64_t>(y) + offset,
                        GridResolution{.value = parameters.resolution}
                    ))
                    : y;
                maximum = std::max(
                    maximum,
                    values[static_cast<size_t>(sample_y) * parameters.resolution + sample_x]
                );
            }
            output[static_cast<size_t>(y) * parameters.resolution + x] = maximum;
        }
    }
    return output;
}

} // namespace

void checkCancellation(std::stop_token const& cancellation)
{
    if (cancellation.stop_requested())
    {
        throw GenerationCancelled();
    }
}

void reportProgress(
    ProgressCallback const& progress,
    GenerationStage stage,
    uint32_t completed,
    uint32_t total,
    std::shared_ptr<GenerationSnapshot const> snapshot
)
{
    if (progress)
    {
        progress(GenerationProgress{stage, completed, total, std::move(snapshot)});
    }
}

FloatField periodicFbm(
    uint64_t seed,
    FbmParameters const& parameters,
    std::stop_token const& cancellation
)
{
    size_t const count = fieldSize(parameters.resolution);
    if (parameters.octaves == 0U || parameters.start_frequency == 0U)
    {
        throw std::invalid_argument("periodic fBm requires positive octave and frequency counts");
    }

    std::vector<double> unit_x(parameters.resolution);
    std::vector<double> unit_y(parameters.resolution);
    for (uint32_t coordinate = 0U; coordinate < parameters.resolution; ++coordinate)
    {
        double const world_coordinate = static_cast<double>(coordinate) * WORLD_SIZE_BLOCKS
            / parameters.resolution;
        unit_x[coordinate] = world_coordinate / WORLD_SIZE_BLOCKS;
        unit_y[coordinate] = world_coordinate / WORLD_SIZE_BLOCKS;
    }

    DoubleField result(count, 0.0);
    double weight_total = 0.0;
    NumpyRandom random(seed);
    for (uint32_t octave = 0U; octave < parameters.octaves; ++octave)
    {
        checkCancellation(cancellation);
        uint32_t const frequency = parameters.start_frequency * (1U << octave);
        double const amplitude = std::ldexp(1.0, -static_cast<int>(octave));
        for (uint32_t mode = 0U; mode < 3U; ++mode)
        {
            int64_t const kx = random.integers(
                -static_cast<int64_t>(frequency),
                static_cast<int64_t>(frequency) + 1
            );
            int64_t ky = random.integers(
                -static_cast<int64_t>(frequency),
                static_cast<int64_t>(frequency) + 1
            );
            int64_t corrected_kx = kx;
            if (corrected_kx == 0 && ky == 0)
            {
                corrected_kx = frequency;
            }
            double const phase = random.uniform(0.0, 2.0 * std::numbers::pi_v<double>);
            for (uint32_t y = 0U; y < parameters.resolution; ++y)
            {
                if ((y & 15U) == 0U)
                {
                    checkCancellation(cancellation);
                }
                for (uint32_t x = 0U; x < parameters.resolution; ++x)
                {
                    size_t const index = static_cast<size_t>(y) * parameters.resolution + x;
                    double const cycles = static_cast<double>(corrected_kx) * unit_x[x]
                        + static_cast<double>(ky) * unit_y[y];
                    double const angle = 2.0 * std::numbers::pi_v<double> * cycles + phase;
                    result[index] += amplitude * std::cos(angle);
                }
            }
            weight_total += amplitude;
        }
    }

    FloatField output(count);
    for (size_t index = 0U; index < count; ++index)
    {
        output[index] = static_cast<float>(result[index] / weight_total);
    }
    return output;
}

DoubleField gaussianFilterWrap(
    std::span<double const> values,
    GaussianFilterParameters const& parameters,
    std::stop_token const& cancellation
)
{
    validateFieldSize(values.size(), parameters.resolution);
    std::vector<double> const weights = gaussianWeights(parameters.sigma);
    checkCancellation(cancellation);
    DoubleField const rows = gaussianAxis<double>(
        values,
        parameters.resolution,
        weights,
        true,
        cancellation
    );
    checkCancellation(cancellation);
    return gaussianAxis<double>(rows, parameters.resolution, weights, false, cancellation);
}

FloatField gaussianFilterWrap(
    std::span<float const> values,
    GaussianFilterParameters const& parameters,
    std::stop_token const& cancellation
)
{
    validateFieldSize(values.size(), parameters.resolution);
    std::vector<double> const weights = gaussianWeights(parameters.sigma);
    checkCancellation(cancellation);
    FloatField const rows = gaussianAxis<float>(
        values,
        parameters.resolution,
        weights,
        true,
        cancellation
    );
    checkCancellation(cancellation);
    return gaussianAxis<float>(rows, parameters.resolution, weights, false, cancellation);
}

DoubleField uniformFilterWrap(
    std::span<double const> values,
    uint32_t resolution,
    uint32_t filter_size,
    std::stop_token const& cancellation
)
{
    validateFieldSize(values.size(), resolution);
    if (filter_size == 0U)
    {
        throw std::invalid_argument("uniform filter size must be positive");
    }
    checkCancellation(cancellation);
    DoubleField const rows = uniformAxis(
        values,
        AxisFilterParameters{.resolution = resolution, .filter_size = filter_size, .rows = true},
        cancellation
    );
    checkCancellation(cancellation);
    return uniformAxis(
        rows,
        AxisFilterParameters{.resolution = resolution, .filter_size = filter_size, .rows = false},
        cancellation
    );
}

ByteField maximumFilterWrap(
    std::span<uint8_t const> values,
    uint32_t resolution,
    uint32_t filter_size,
    std::stop_token const& cancellation
)
{
    validateFieldSize(values.size(), resolution);
    if (filter_size == 0U)
    {
        throw std::invalid_argument("maximum filter size must be positive");
    }
    checkCancellation(cancellation);
    ByteField const rows = maximumAxis(
        values,
        AxisFilterParameters{.resolution = resolution, .filter_size = filter_size, .rows = true},
        cancellation
    );
    checkCancellation(cancellation);
    return maximumAxis(
        rows,
        AxisFilterParameters{.resolution = resolution, .filter_size = filter_size, .rows = false},
        cancellation
    );
}

std::pair<DoubleField, DoubleField> gradientPeriodic(std::span<double const> values, uint32_t resolution)
{
    validateFieldSize(values.size(), resolution);
    DoubleField x_gradient(values.size());
    DoubleField y_gradient(values.size());
    for (uint32_t y = 0U; y < resolution; ++y)
    {
        uint32_t const previous_y = y == 0U ? resolution - 1U : y - 1U;
        uint32_t const next_y = y + 1U == resolution ? 0U : y + 1U;
        for (uint32_t x = 0U; x < resolution; ++x)
        {
            uint32_t const previous_x = x == 0U ? resolution - 1U : x - 1U;
            uint32_t const next_x = x + 1U == resolution ? 0U : x + 1U;
            size_t const index = static_cast<size_t>(y) * resolution + x;
            x_gradient[index] = (values[static_cast<size_t>(y) * resolution + next_x]
                - values[static_cast<size_t>(y) * resolution + previous_x]) * 0.5;
            y_gradient[index] = (values[static_cast<size_t>(next_y) * resolution + x]
                - values[static_cast<size_t>(previous_y) * resolution + x]) * 0.5;
        }
    }
    return {std::move(x_gradient), std::move(y_gradient)};
}

double bilinearPeriodic(std::span<float const> values, uint32_t resolution, WorldPoint position)
{
    validateFieldSize(values.size(), resolution);
    double const x_remainder = std::fmod(position.x, static_cast<double>(WORLD_SIZE_BLOCKS));
    double const y_remainder = std::fmod(position.y, static_cast<double>(WORLD_SIZE_BLOCKS));
    double const wrapped_x = x_remainder < 0.0 ? x_remainder + WORLD_SIZE_BLOCKS : x_remainder;
    double const wrapped_y = y_remainder < 0.0 ? y_remainder + WORLD_SIZE_BLOCKS : y_remainder;
    double const sample_x = wrapped_x * resolution / WORLD_SIZE_BLOCKS;
    double const sample_y = wrapped_y * resolution / WORLD_SIZE_BLOCKS;
    int64_t const floor_x = static_cast<int64_t>(std::floor(sample_x));
    int64_t const floor_y = static_cast<int64_t>(std::floor(sample_y));
    double const fraction_x = sample_x - static_cast<double>(floor_x);
    double const fraction_y = sample_y - static_cast<double>(floor_y);
    uint32_t const x0 = static_cast<uint32_t>(wrappedIndex(floor_x, GridResolution{.value = resolution}));
    uint32_t const y0 = static_cast<uint32_t>(wrappedIndex(floor_y, GridResolution{.value = resolution}));
    uint32_t const x1 = (x0 + 1U) % resolution;
    uint32_t const y1 = (y0 + 1U) % resolution;
    double const top_left = values[static_cast<size_t>(y0) * resolution + x0];
    double const top_right = values[static_cast<size_t>(y0) * resolution + x1];
    double const bottom_left = values[static_cast<size_t>(y1) * resolution + x0];
    double const bottom_right = values[static_cast<size_t>(y1) * resolution + x1];
    return (1.0 - fraction_x) * (1.0 - fraction_y) * top_left
        + fraction_x * (1.0 - fraction_y) * top_right
        + (1.0 - fraction_x) * fraction_y * bottom_left
        + fraction_x * fraction_y * bottom_right;
}

double bilinearPeriodic(std::span<double const> values, uint32_t resolution, WorldPoint position)
{
    validateFieldSize(values.size(), resolution);
    double const x_remainder = std::fmod(position.x, static_cast<double>(WORLD_SIZE_BLOCKS));
    double const y_remainder = std::fmod(position.y, static_cast<double>(WORLD_SIZE_BLOCKS));
    double const wrapped_x = x_remainder < 0.0 ? x_remainder + WORLD_SIZE_BLOCKS : x_remainder;
    double const wrapped_y = y_remainder < 0.0 ? y_remainder + WORLD_SIZE_BLOCKS : y_remainder;
    double const sample_x = wrapped_x * resolution / WORLD_SIZE_BLOCKS;
    double const sample_y = wrapped_y * resolution / WORLD_SIZE_BLOCKS;
    int64_t const floor_x = static_cast<int64_t>(std::floor(sample_x));
    int64_t const floor_y = static_cast<int64_t>(std::floor(sample_y));
    double const fraction_x = sample_x - static_cast<double>(floor_x);
    double const fraction_y = sample_y - static_cast<double>(floor_y);
    uint32_t const x0 = static_cast<uint32_t>(wrappedIndex(floor_x, GridResolution{.value = resolution}));
    uint32_t const y0 = static_cast<uint32_t>(wrappedIndex(floor_y, GridResolution{.value = resolution}));
    uint32_t const x1 = (x0 + 1U) % resolution;
    uint32_t const y1 = (y0 + 1U) % resolution;
    double const top_left = values[static_cast<size_t>(y0) * resolution + x0];
    double const top_right = values[static_cast<size_t>(y0) * resolution + x1];
    double const bottom_left = values[static_cast<size_t>(y1) * resolution + x0];
    double const bottom_right = values[static_cast<size_t>(y1) * resolution + x1];
    return (1.0 - fraction_x) * (1.0 - fraction_y) * top_left
        + fraction_x * (1.0 - fraction_y) * top_right
        + (1.0 - fraction_x) * fraction_y * bottom_left
        + fraction_x * fraction_y * bottom_right;
}

float medianFloat32(std::span<float const> values)
{
    if (values.empty())
    {
        throw std::invalid_argument("median requires at least one value");
    }
    std::vector<float> ordered(values.begin(), values.end());
    size_t const middle = ordered.size() / 2U;
    std::nth_element(ordered.begin(), ordered.begin() + static_cast<ptrdiff_t>(middle), ordered.end());
    if ((ordered.size() & 1U) != 0U)
    {
        return ordered[middle];
    }
    float const upper = ordered[middle];
    float const lower = *std::max_element(
        ordered.begin(),
        ordered.begin() + static_cast<ptrdiff_t>(middle)
    );
    return (lower + upper) * 0.5F;
}

} // namespace generation::detail
