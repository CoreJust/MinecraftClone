#include "V9Terrain.hpp"

#include "Numerics.hpp"
#include "Sha256.hpp"
#include "V9TerrainMorphology.hpp"

// DONT_CHECK INCLUDE_ORDER: the frozen structural header uses std::clamp without including <algorithm>.
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
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>

namespace generation::detail
{
namespace
{
constexpr uint32_t N = 256U;
constexpr size_t COUNT = N * N;
constexpr double METRES_PER_RAW_UNIT = 157.576'557'850'187'8;

// SciPy correlate1d's symmetric Gaussian path accumulates the centre
// followed by fused symmetric-pair updates, as in the pinned SciPy wheel. The
// coefficients are the frozen SciPy 1.18.1 binary64 kernel values.
std::vector<double> gaussianControls(std::span<double const> values, bool wide, std::stop_token cancellation)
{
    constexpr std::array<double, 7> WIDE{
        0x1.7638'649a'0fcd1p-14, 0x1.0d89'1ab5'3680ap-10, 0x1.f1e7'41f8'0c9b8p-8,
        0x1.26dd'9e7a'284e1p-5, 0x1.bfdc'8828'9323cp-4, 0x1.b428'51d1'c8a96p-3,
        0x1.1058'ef29'a59d7p-2,
    };
    constexpr std::array<double, 5> NARROW{
        0x1.18a9'c4fd'536c6p-13, 0x1.2272'4cb7'eb269p-8, 0x1.ba4b'99d1'799abp-5,
        0x1.ef8e'b9ad'499bap-3, 0x1.9884'a307'594fbp-2,
    };
    std::span<double const> const weights = wide ? std::span<double const>(WIDE) : std::span<double const>(NARROW);
    int32_t const radius = static_cast<int32_t>(weights.size()) - 1;
    std::vector<double> input(values.begin(), values.end()), output(COUNT);
    for (uint32_t axis = 0U; axis < 2U; ++axis)
    {
        for (int32_t y = 0; y < static_cast<int32_t>(N); ++y)
        {
            checkCancellation(cancellation);
            for (int32_t x = 0; x < static_cast<int32_t>(N); ++x)
            {
                auto const at = [&](int32_t offset) {
                    int32_t const sx = axis == 1 ? (x + offset + N) % N : x;
                    int32_t const sy = axis == 0 ? (y + offset + N) % N : y;
                    return input[sy * N + sx];
                };
                double sum = at(0) * weights[radius];
                for (int32_t offset = -radius; offset < 0; ++offset) {
                    sum = std::fma(at(offset) + at(-offset), weights[offset + radius], sum);
                }
                output[y * N + x] = sum;
            }
        }
        input.swap(output);
    }
    return input;
}

std::vector<uint8_t> provisionalOcean(std::span<double const> bed, std::stop_token cancellation)
{
    std::vector<int32_t> labels(COUNT, -1);
    std::vector<uint32_t> component;
    std::vector<uint32_t> largest;
    for (uint32_t index = 0; index < COUNT; ++index)
    {
        if (index % N == 0) {
            checkCancellation(cancellation);
        }
        if (bed[index] >= 0 || labels[index] >= 0) {
            continue;
        }
        component.clear();
        component.push_back(index);
        labels[index] = static_cast<int32_t>(index);
        for (size_t next = 0; next < component.size(); ++next)
        {
            auto const at = component[next];
            auto const x = at % N;
            auto const y = at / N;
            std::array<uint32_t, 4U> const neighbours{
                y * N + (x + N - 1U) % N, y * N + (x + 1U) % N,
                ((y + N - 1U) % N) * N + x, ((y + 1U) % N) * N + x,
            };
            for (auto neighbour : neighbours)
            {
                if (bed[neighbour] < 0 && labels[neighbour] < 0)
                {
                    labels[neighbour] = static_cast<int32_t>(index);
                    component.push_back(neighbour);
                }
            }
        }
        if (component.size() > largest.size()) {
            largest = component;
        }
    }
    std::vector<uint8_t> ocean(COUNT, 0U);
    for (auto index : largest) {
        ocean[index] = 1U;
    }
    return ocean;
}

int32_t textureSeed(uint64_t seed)
{
    std::string const material = "structural-worlds-dendry-v1/" + std::to_string(seed);
    auto const digest = sha256Digest(std::span(
        reinterpret_cast<uint8_t const*>(material.data()), material.size()
    ));
    uint32_t value = 0U;
    for (uint32_t byte = 0U; byte < 4U; ++byte) {
        value |= uint32_t{digest[byte]} << (8U * byte);
    }
    return static_cast<int32_t>(value & 0x7fff'ffffU);
}
} // namespace

V9TerrainResult prepareV9Terrain(
    uint64_t user_seed, V9PlatecSourceResult const& source,
    std::stop_token cancellation, V9TerrainProgressCallback progress
)
{
    auto const signal = [&](V9TerrainStage stage) {
        checkCancellation(cancellation);
        if (progress) {
            progress(stage);
        }
        checkCancellation(cancellation);
    };
    signal(V9TerrainStage::SourceMapping);
    if (source.height_raw.size() != COUNT || source.current_plate_id.size() != COUNT) {
        throw std::invalid_argument("v9 terrain requires registered 256-square Platec fields");
    }
    V9TerrainResult result;
    result.dendry_seed = textureSeed(user_seed);
    result.macro_bed_centres_m.resize(COUNT);
    std::vector<double> base(COUNT), slope(COUNT);
    for (size_t i = 0; i < COUNT; ++i)
    {
        double const raw = source.height_raw[i];
        if (!std::isfinite(raw)) {
            throw std::invalid_argument("nonfinite v9 Platec height");
        }
        float const elevation = static_cast<float>(raw >= 1.0
            ? (raw - 1.0) * METRES_PER_RAW_UNIT : -600.0 * std::pow((1.0 - raw) / .9, 1.5));
        result.macro_bed_centres_m[i] = elevation;
        double const uplift = std::max(double{elevation}, 0.0);
        double const continentality = raw >= 1.0 ? 1.0 : 0.0;
        base[i] = (80.0 + (220.0 * continentality) / 2.0) + .08 * uplift;
        double const potential = std::clamp(uplift / 1600.0, 0.0, 1.0);
        slope[i] = .018 + ((.8 * std::pow(potential, .65))
            * std::pow(.5, .7)) * (.65 + .7 * .5);
    }
    signal(V9TerrainStage::StructuralPreparation);
    base = gaussianControls(base, true, cancellation);
    slope = gaussianControls(slope, false, cancellation);
    result.control_layers.resize(2U * COUNT);
    result.structural_layers.resize(3U * COUNT, 0.0);
    for (size_t i = 0; i < COUNT; ++i)
    {
        result.control_layers[i] = base[i] / 2000.0;
        result.control_layers[COUNT + i] = slope[i];
        result.structural_layers[i] = result.macro_bed_centres_m[i];
        result.structural_layers[2U * COUNT + i] = slope[i];
    }
    signal(V9TerrainStage::CanonicalVertices);
    result.canonical_macro_vertices_m.resize(COUNT);
    for (uint32_t y = 0; y < N; ++y)
    {
        checkCancellation(cancellation);
        for (uint32_t x = 0; x < N; ++x) {
            result.canonical_macro_vertices_m[y * N + x] = cubicPeriodic(
                static_cast<double>(x) - .5, static_cast<double>(y) - .5, N,
                [&](uint32_t cx, uint32_t cy) { return result.structural_layers[cy * N + cx]; }
            );
        }
    }
    signal(V9TerrainStage::ProvisionalRouting);
    auto const ocean = provisionalOcean(result.canonical_macro_vertices_m, cancellation);
    signal(V9TerrainStage::TerrainEvolution);
    result.final_bed_vertices_m = evolveV9Terrain(result.canonical_macro_vertices_m, ocean, cancellation);
    signal(V9TerrainStage::Complete);
    return result;
}

struct V9StructuralSurface::Impl final
{
    explicit Impl(V9TerrainResult const& terrain)
        : surface(terrain.control_layers, terrain.structural_layers, terrain.dendry_seed) {}
    SeededWorldSurface surface;
};

V9StructuralSurface::V9StructuralSurface(V9TerrainResult const& terrain)
    : m_impl(std::make_unique<Impl>(terrain)) {}
V9StructuralSurface::~V9StructuralSurface() = default;

std::array<double, 13U> V9StructuralSurface::evaluate(double x_m, double y_m, uint32_t lod) const
{
    return m_impl->surface.evaluate(x_m, y_m, lod);
}
} // namespace generation::detail
