#pragma once

#include <generation/Generation.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <utility>
#include <vector>

namespace generation::detail
{

using FloatField = std::vector<float>;
using DoubleField = std::vector<double>;
using ByteField = std::vector<uint8_t>;

struct FbmParameters final
{
    uint32_t resolution;
    uint32_t octaves;
    uint32_t start_frequency;
};

struct GaussianFilterParameters final
{
    uint32_t resolution;
    double sigma;
};

struct WorldPoint final
{
    double x;
    double y;
};

void checkCancellation(std::stop_token const& cancellation);

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
    FbmParameters const& parameters,
    std::stop_token const& cancellation
);

[[nodiscard]]
DoubleField gaussianFilterWrap(
    std::span<double const> values,
    GaussianFilterParameters const& parameters,
    std::stop_token const& cancellation
);

[[nodiscard]]
FloatField gaussianFilterWrap(
    std::span<float const> values,
    GaussianFilterParameters const& parameters,
    std::stop_token const& cancellation
);

[[nodiscard]]
DoubleField uniformFilterWrap(
    std::span<double const> values,
    uint32_t resolution,
    uint32_t filter_size,
    std::stop_token const& cancellation
);

[[nodiscard]]
ByteField maximumFilterWrap(
    std::span<uint8_t const> values,
    uint32_t resolution,
    uint32_t filter_size,
    std::stop_token const& cancellation
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
    WorldPoint position
);

[[nodiscard]]
double bilinearPeriodic(
    std::span<double const> values,
    uint32_t resolution,
    WorldPoint position
);

[[nodiscard]]
float medianFloat32(std::span<float const> values);

} // namespace generation::detail
