#pragma once

#include <generation/Generation.hpp>

#include <span>
#include <stop_token>
#include <utility>
#include <vector>

namespace generation::detail
{

using FloatField = std::vector<float>;
using DoubleField = std::vector<double>;
using ByteField = std::vector<uint8_t>;

void checkCancellation(std::stop_token cancellation);

[[nodiscard]]
double numpyTanh(double value);

void reportProgress(
    ProgressCallback const& progress,
    GenerationStage stage,
    uint32_t completed,
    uint32_t total,
    std::shared_ptr<GenerationSnapshot const> snapshot = {}
);

[[nodiscard]]
FloatField periodicFbm(
    uint64_t seed,
    uint32_t resolution,
    uint32_t octaves,
    uint32_t start_frequency,
    std::stop_token cancellation
);

[[nodiscard]]
DoubleField gaussianFilterWrap(
    std::span<double const> values,
    uint32_t resolution,
    double sigma,
    std::stop_token cancellation
);

[[nodiscard]]
FloatField gaussianFilterWrap(
    std::span<float const> values,
    uint32_t resolution,
    double sigma,
    std::stop_token cancellation
);

[[nodiscard]]
DoubleField uniformFilterWrap(
    std::span<double const> values,
    uint32_t resolution,
    uint32_t filter_size,
    std::stop_token cancellation
);

[[nodiscard]]
ByteField maximumFilterWrap(
    std::span<uint8_t const> values,
    uint32_t resolution,
    uint32_t filter_size,
    std::stop_token cancellation
);

[[nodiscard]]
std::pair<DoubleField, DoubleField> gradientPeriodic(
    std::span<double const> values,
    uint32_t resolution
);

[[nodiscard]]
double bilinearPeriodic(
    std::span<float const> values,
    uint32_t resolution,
    double x,
    double y
);

[[nodiscard]]
double bilinearPeriodic(
    std::span<double const> values,
    uint32_t resolution,
    double x,
    double y
);

[[nodiscard]]
float medianFloat32(std::span<float const> values);

} // namespace generation::detail
