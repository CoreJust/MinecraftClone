#include "V21Terrain.hpp"

#include "Numerics.hpp"
#include "Sha256.hpp"

// DONT_CHECK INCLUDE_ORDER: the frozen structural header requires <algorithm> first.
#include <algorithm>

#if defined(__clang__)
#pragma clang fp contract(on)
#endif
#include "world_surface.h"
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

#include <array>
#include <cmath>
#include <future>
#include <span>
#include <stdexcept>
#include <string>

namespace generation::detail
{
namespace
{

constexpr uint32_t N = 256U;
constexpr size_t COUNT = N * N;

// Frozen SciPy binary64 Gaussian coefficients, from the negative radius to the
// centre. The pinned wheel contracts the small control kernels only.
constexpr std::array<double, 7U> BASE_WEIGHTS{
    0x1.763'8649a0fcd1p-14, 0x1.0d891ab53'680ap-10, 0x1.f1e741f80c9b8p-8,
    0x1.26dd9e7a284e1p-5, 0x1.bfdc88'289323cp-4, 0x1.b42'851d1c8a96p-3,
    0x1.1058ef29a59d7p-2,
};
constexpr std::array<double, 5U> SLOPE_WEIGHTS{
    0x1.18a9c4fd536c6p-13, 0x1.22'724cb7eb269p-8, 0x1.ba4b99d1799abp-5,
    0x1.ef8eb9ad499bap-3, 0x1.9884a307'594fbp-2,
};
constexpr std::array<double, 25U> LOWPASS_WEIGHTS{
    0x1.763b931f10'805p-16, 0x1.676d110f133a3p-15, 0x1.4fbf851'751e70p-14,
    0x1.3109b9034c193p-13, 0x1.0d8b655f7b04ep-12, 0x1.cf4fd0eab9b7cp-12,
    0x1.8347'1566d3f38p-11, 0x1.3ada5e279d656p-10, 0x1.f1eb7db1bd0cbp-10,
    0x1.7eeda023d0e59p-9, 0x1.1e6ce7e9da4b2p-8, 0x1.a0'bf1bcbe1a45p-8,
    0x1.26e0204'679b92p-7, 0x1.95db0e7acbd52p-7, 0x1.0fa695da23fc2p-6,
    0x1.61af4ce1724abp-6, 0x1.bfe056f6b92d4p-6, 0x1.13cebab768bf4p-5,
    0x1.4a6293d0084b2p-5, 0x1.80eba0f8732c1p-5, 0x1.b42c0'7267189bp-5,
    0x1.e0b51'a9d9de6ap-5, 0x1.01a35e4c1139ep-4, 0x1.0c9990adce751p-4,
    0x1.105b3ff2ae294p-4,
};

std::vector<double> gaussianPeriodic(
    std::span<double const> values,
    std::span<double const> weights,
    std::stop_token cancellation
)
{
    int32_t const radius = static_cast<int32_t>(weights.size()) - 1;
    std::vector<double> input(values.begin(), values.end());
    std::vector<double> output(COUNT);
    for (uint32_t axis = 0U; axis < 2U; ++axis)
    {
        for (int32_t y = 0; y < static_cast<int32_t>(N); ++y)
        {
            checkCancellation(cancellation);
            for (int32_t x = 0; x < static_cast<int32_t>(N); ++x)
            {
                auto const at = [&](int32_t offset) {
                    int32_t const sx = axis == 1U ? (x + offset + N) % N : x;
                    int32_t const sy = axis == 0U ? (y + offset + N) % N : y;
                    return input[sy * N + sx];
                };
                double sum = at(0) * weights[radius];
                for (int32_t offset = -radius; offset < 0; ++offset)
                {
                    double const pair = at(offset) + at(-offset);
                    if (radius <= 6)
                    {
                        sum = std::fma(pair, weights[offset + radius], sum);
                    }
                    else
                    {
                        sum += pair * weights[offset + radius];
                    }
                }
                output[y * N + x] = sum;
            }
        }
        input.swap(output);
    }
    return input;
}

int32_t textureSeed(uint64_t user_seed)
{
    std::string const material = "structural-worlds-dendry-v1/" + std::to_string(user_seed);
    auto const digest = sha256Digest(std::span(
        reinterpret_cast<uint8_t const*>(material.data()), material.size()
    ));
    uint32_t value = 0U;
    for (uint32_t byte = 0U; byte < 4U; ++byte)
    {
        value |= uint32_t{digest[byte]} << (8U * byte);
    }
    return static_cast<int32_t>(value & 0x7fff'ffffU);
}

} // namespace

V21TerrainPreparation prepareV21Terrain(
    uint64_t user_seed,
    V9PlatecSourceResult const& source,
    std::stop_token cancellation,
    V9TerrainProgressCallback progress
)
{
    auto const signal = [&](V9TerrainStage stage) {
        checkCancellation(cancellation);
        if (progress)
        {
            progress(stage);
        }
        checkCancellation(cancellation);
    };
    signal(V9TerrainStage::SourceMapping);
    if (source.height_raw.size() != COUNT || source.current_plate_id.size() != COUNT)
    {
        throw std::invalid_argument{"V21 preparation requires registered 256-square Platec fields"};
    }
    V21TerrainPreparation result;
    V9TerrainResult& native = result.native;
    native.dendry_seed = textureSeed(user_seed);
    native.macro_bed_centres_m.resize(COUNT);
    std::vector<double> base(COUNT);
    std::vector<double> slope(COUNT);
    for (size_t index = 0U; index < COUNT; ++index)
    {
        if (!std::isfinite(source.height_raw[index]))
        {
            throw std::invalid_argument{"V21 preparation received a nonfinite Platec height"};
        }
        float const elevation = v9PlatecHeightToSourceMetres(source.height_raw[index]);
        if (!std::isfinite(elevation))
        {
            throw std::invalid_argument{"V21 source height exceeds the finite metre mapping"};
        }
        native.macro_bed_centres_m[index] = elevation;
        double const uplift = std::max(double{elevation}, 0.0);
        double const continentality = source.height_raw[index] >= 1.0F ? 1.0 : 0.0;
        base[index] = (80.0 + (220.0 * continentality) / 2.0) + .08 * uplift;
        double const potential = std::clamp(uplift / 1600.0, 0.0, 1.0);
        slope[index] = .018 + ((.8 * std::pow(potential, .65)) * std::pow(.5, .7));
    }
    signal(V9TerrainStage::StructuralPreparation);
    base = gaussianPeriodic(base, BASE_WEIGHTS, cancellation);
    slope = gaussianPeriodic(slope, SLOPE_WEIGHTS, cancellation);
    native.control_layers.resize(2U * COUNT);
    native.structural_layers.resize(3U * COUNT, 0.0);
    for (size_t index = 0U; index < COUNT; ++index)
    {
        native.control_layers[index] = base[index] / 2000.0;
        native.control_layers[COUNT + index] = slope[index];
        native.structural_layers[index] = native.macro_bed_centres_m[index];
        native.structural_layers[2U * COUNT + index] = slope[index];
    }
    signal(V9TerrainStage::CanonicalVertices);
    native.canonical_macro_vertices_m.resize(COUNT);
    result.canonical_d3_vertices_m.resize(COUNT);
    // Each sampler owns its mutable hierarchy caches. Rows are disjoint and
    // every sample keeps the sequential arithmetic; only evaluation order varies.
    constexpr uint32_t WORKERS = 4U;
    std::array<std::future<void>, WORKERS> workers;
    for (uint32_t worker = 0U; worker < WORKERS; ++worker)
    {
        workers[worker] = std::async(std::launch::async, [&, worker] {
            checkCancellation(cancellation);
            SeededWorldSurface const surface{
                native.control_layers, native.structural_layers, native.dendry_seed,
            };
            for (uint32_t y = worker * N / WORKERS; y < (worker + 1U) * N / WORKERS; ++y)
            {
                checkCancellation(cancellation);
                for (uint32_t x = 0U; x < N; ++x)
                {
                    size_t const index = y * N + x;
                    native.canonical_macro_vertices_m[index] = cubicPeriodic(
                        static_cast<double>(x) - .5, static_cast<double>(y) - .5, N,
                        [&](uint32_t cx, uint32_t cy) {
                            return native.structural_layers[cy * N + cx];
                        }
                    );
                    result.canonical_d3_vertices_m[index] = 2000.0
                        * surface.terrain().evaluateTerrainUncached(
                            static_cast<double>(x) / 8.0, static_cast<double>(y) / 8.0
                        );
                }
            }
        });
    }
    // Join all workers before propagating a failure or starting the shared
    // lowpass. Future destruction also joins if launching a later worker fails.
    for (auto& worker : workers)
    {
        worker.wait();
    }
    for (auto& worker : workers)
    {
        worker.get();
    }
    result.lowpass_d3_vertices_m = gaussianPeriodic(
        result.canonical_d3_vertices_m, LOWPASS_WEIGHTS, cancellation
    );
    result.canonical_h0_vertices_m.resize(COUNT);
    for (size_t index = 0U; index < COUNT; ++index)
    {
        native.structural_layers[COUNT + index] = result.lowpass_d3_vertices_m[index];
        result.canonical_h0_vertices_m[index] = (native.canonical_macro_vertices_m[index]
            + result.canonical_d3_vertices_m[index]) - result.lowpass_d3_vertices_m[index];
        if (!std::isfinite(result.canonical_h0_vertices_m[index]))
        {
            throw std::runtime_error{"V21 preparation produced a nonfinite canonical H0"};
        }
    }
    signal(V9TerrainStage::Complete);
    return result;
}

} // namespace generation::detail
