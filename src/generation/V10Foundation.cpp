#include "V10Foundation.hpp"

#include "../../third_party/v10_perlin/perlin2.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

// The feature-transform recurrence and symmetric correlation order below are
// adapted from SciPy 1.18.1 ndimage/src/ni_morphology.c and ni_filters.c:
// https://github.com/scipy/scipy/tree/v1.18.1/scipy/ndimage/src
/* Copyright (C) 2003-2005 Peter J. Verveer
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 *
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace generation::detail
{
namespace
{
constexpr size_t SIDE = V10_FOUNDATION_SIDE;
constexpr size_t COUNT = SIDE * SIDE;
constexpr int32_t TILED_SIDE = static_cast<int32_t>(3U * SIDE);
constexpr double SPACING_M = 256.0;
// scipy.ndimage._filters._gaussian_kernel1d(3, 0, 12), binary64. Fixed
// coefficients preserve the frozen NumPy normalization, without runtime Python.
constexpr std::array<double, 25> GAUSSIAN = {
    4.461'134'277'264'88e-05,
    0.000'160'094'765'891'977'58,
    0.000'514'107'605'821'972'7,
    0.001'477'324'778'400'407,
    0.003'798'769'940'305'482'4,
    0.008'740'878'047'691'292,
    0.017'997'500'190'860'556,
    0.033'159'988'420'867'42,
    0.054'671'578'245'654'065,
    0.080'659'199'898'786'99,
    0.106'485'694'027'244'7,
    0.125'797'983'460'310'65,
    0.132'984'538'550'783'7,
    0.125'797'983'460'310'65,
    0.106'485'694'027'244'7,
    0.080'659'199'898'786'99,
    0.054'671'578'245'654'065,
    0.033'159'988'420'867'42,
    0.017'997'500'190'860'556,
    0.008'740'878'047'691'292,
    0.003'798'769'940'305'482'4,
    0.001'477'324'778'400'407,
    0.000'514'107'605'821'972'7,
    0.000'160'094'765'891'977'58,
    4.461'134'277'264'88e-05,
};

struct Feature final
{
    int32_t y = -1;
    int32_t x = -1;
};

size_t index(int32_t y, int32_t x)
{
    return static_cast<size_t>(y * TILED_SIDE + x);
}

size_t wrap(int32_t value)
{
    int32_t const side = static_cast<int32_t>(SIDE);
    return static_cast<size_t>((value % side + side) % side);
}

int32_t component(Feature feature, int32_t axis)
{
    return axis == 0 ? feature.y : feature.x;
}

// Maurer feature transform, specialized to two unit-spaced dimensions. The
// strict hull-pop and <= nearest comparisons preserve SciPy's tie selection.
void transformLine(std::vector<Feature>& features, int32_t axis, int32_t fixed)
{
    std::array<Feature, TILED_SIDE> candidates;
    std::array<int32_t, TILED_SIDE> hull;
    for (int32_t i = 0; i < TILED_SIDE; ++i)
    {
        candidates[static_cast<size_t>(i)] = features[axis == 0 ? index(i, fixed) : index(fixed, i)];
    }
    int32_t last = -1;
    for (int32_t i = 0; i < TILED_SIDE; ++i)
    {
        Feature const right = candidates[static_cast<size_t>(i)];
        if (right.y < 0)
        {
            continue;
        }
        double const right_axis = component(right, axis);
        double const right_other = component(right, 1 - axis) - fixed;
        double const right_squared = right_other * right_other;
        while (last >= 1)
        {
            Feature const middle = candidates[static_cast<size_t>(hull[static_cast<size_t>(last)])];
            Feature const left = candidates[static_cast<size_t>(hull[static_cast<size_t>(last - 1)])];
            double const a = component(middle, axis) - component(left, axis);
            double const b = right_axis - component(middle, axis);
            double const c = a + b;
            double const left_other = component(left, 1 - axis) - fixed;
            double const middle_other = component(middle, 1 - axis) - fixed;
            if (c * middle_other * middle_other - b * left_other * left_other
                - a * right_squared - a * b * c <= 0.0)
            {
                break;
            }
            --last;
        }
        hull[static_cast<size_t>(++last)] = i;
    }
    if (last < 0)
    {
        return;
    }
    int32_t selected = 0;
    auto distance_squared = [&](int32_t candidate, int32_t coordinate) {
        Feature const feature = candidates[static_cast<size_t>(hull[static_cast<size_t>(candidate)])];
        double const along = component(feature, axis) - coordinate;
        double const across = component(feature, 1 - axis) - fixed;
        return along * along + across * across;
    };
    for (int32_t i = 0; i < TILED_SIDE; ++i)
    {
        double distance = distance_squared(selected, i);
        while (selected < last)
        {
            double const next = distance_squared(selected + 1, i);
            if (distance <= next)
            {
                break;
            }
            distance = next;
            ++selected;
        }
        features[axis == 0 ? index(i, fixed) : index(fixed, i)]
            = candidates[static_cast<size_t>(hull[static_cast<size_t>(selected)])];
    }
}

std::vector<Feature> nearestValid(std::span<uint8_t const> valid)
{
    if (std::none_of(valid.begin(), valid.end(), [](uint8_t value) { return value != 0U; }))
    {
        throw std::invalid_argument("V10 foundation requires normalized support");
    }
    std::vector<Feature> features(static_cast<size_t>(TILED_SIDE * TILED_SIDE));
    for (int32_t y = 0; y < TILED_SIDE; ++y)
    {
        for (int32_t x = 0; x < TILED_SIDE; ++x)
        {
            if (valid[wrap(y) * SIDE + wrap(x)] != 0U)
            {
                features[index(y, x)] = {y, x};
            }
        }
    }
    for (int32_t x = 0; x < TILED_SIDE; ++x)
    {
        transformLine(features, 0, x);
    }
    for (int32_t y = 0; y < TILED_SIDE; ++y)
    {
        transformLine(features, 1, y);
    }
    return features;
}

void fillUnsupported(std::vector<double>& values, std::span<uint8_t const> valid)
{
    if (std::all_of(valid.begin(), valid.end(), [](uint8_t value) { return value != 0U; }))
    {
        return;
    }
    auto const nearest = nearestValid(valid);
    auto const original = values;
    for (size_t y = 0U; y < SIDE; ++y)
    {
        for (size_t x = 0U; x < SIDE; ++x)
        {
            Feature const feature = nearest[index(static_cast<int32_t>(y + SIDE), static_cast<int32_t>(x + SIDE))];
            values[y * SIDE + x] = original[wrap(feature.y) * SIDE + wrap(feature.x)];
        }
    }
}

std::vector<double> gaussian(std::vector<double> input)
{
    std::vector<double> output(COUNT);
    for (int32_t axis = 0; axis < 2; ++axis)
    {
        for (int32_t y = 0; y < static_cast<int32_t>(SIDE); ++y)
        {
            for (int32_t x = 0; x < static_cast<int32_t>(SIDE); ++x)
            {
                auto at = [&](int32_t offset) {
                    return input[wrap(y + (axis == 0 ? offset : 0)) * SIDE
                        + wrap(x + (axis == 1 ? offset : 0))];
                };
                double value = at(0) * GAUSSIAN[12U];
                for (int32_t offset = -12; offset < 0; ++offset)
                {
                    value += (at(offset) + at(-offset)) * GAUSSIAN[static_cast<size_t>(offset + 12)];
                }
                output[static_cast<size_t>(y) * SIDE + static_cast<size_t>(x)] = value;
            }
        }
        input.swap(output);
    }
    return input;
}

std::vector<double> normalized(std::span<uint8_t const> mask, std::span<double const> magnitude)
{
    std::vector<double> support(COUNT), numerator(COUNT);
    for (size_t i = 0U; i < COUNT; ++i)
    {
        support[i] = mask[i] != 0U ? 1.0 : 0.0;
        numerator[i] = mask[i] != 0U ? magnitude[i] : 0.0;
    }
    support = gaussian(std::move(support));
    numerator = gaussian(std::move(numerator));
    std::vector<uint8_t> valid(COUNT);
    for (size_t i = 0U; i < COUNT; ++i)
    {
        valid[i] = support[i] >= 1e-8 ? 1U : 0U;
        numerator[i] = valid[i] != 0U ? numerator[i] / support[i] : 0.0;
    }
    fillUnsupported(numerator, valid);
    return numerator;
}

double repeat(double value, double period = V10_FOUNDATION_PERIOD_M)
{
    double result = std::fmod(value, period);
    if (result < 0.0)
    {
        result += period;
    }
    return result;
}

double smoothstep(double input)
{
    double const t = std::clamp(input, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double noise(double x, double y, int32_t repeat_x, int32_t repeat_y, int32_t base)
{
    // The reviewed Python C wrapper rounds positive-remainder arguments to
    // binary32. The unchanged vendored kernel returns binary32, widened here.
    return static_cast<double>(platec_noise2(
        static_cast<float>(repeat(x, repeat_x)), static_cast<float>(repeat(y, repeat_y)),
        static_cast<float>(repeat_x), static_cast<float>(repeat_y), base
    ));
}

} // namespace

V10FoundationBasis prepareV10Foundation(uint32_t seed, std::span<float const> raw_height)
{
    if (seed != 42U)
    {
        throw std::invalid_argument("V10 foundation requires seed 42");
    }
    if (raw_height.size() != COUNT)
    {
        throw std::invalid_argument("V10 foundation requires 256x256 raw heights");
    }
    std::vector<double> source(COUNT), magnitude(COUNT);
    std::vector<uint8_t> land(COUNT), wet(COUNT);
    for (size_t i = 0U; i < COUNT; ++i)
    {
        double const raw = raw_height[i];
        if (!std::isfinite(raw) || raw <= 0.0)
        {
            throw std::invalid_argument("V10 foundation requires finite positive raw heights");
        }
        double const u = std::log(raw);
        source[i] = u * (70.0 + 60.0 * std::abs(u));
        magnitude[i] = std::abs(source[i]);
        land[i] = source[i] > 0.0 ? 1U : 0U;
        wet[i] = land[i] == 0U ? 1U : 0U;
    }
    if (std::none_of(land.begin(), land.end(), [](uint8_t v) { return v != 0U; })
        || std::none_of(wet.begin(), wet.end(), [](uint8_t v) { return v != 0U; }))
    {
        throw std::invalid_argument("V10 foundation requires land and wet support");
    }
    V10FoundationBasis basis;
    basis.land_magnitude_m = normalized(land, source);
    basis.ocean_magnitude_m = normalized(wet, magnitude);
    basis.regional_relief_m.resize(COUNT);
    std::vector<uint8_t> valid(COUNT);
    for (int32_t y = 0; y < static_cast<int32_t>(SIDE); ++y)
    {
        for (int32_t x = 0; x < static_cast<int32_t>(SIDE); ++x)
        {
            double low = std::numeric_limits<double>::infinity();
            double high = -std::numeric_limits<double>::infinity();
            for (int32_t dy = -4; dy <= 4; ++dy)
            {
                for (int32_t dx = -4; dx <= 4; ++dx)
                {
                    size_t const neighbour = wrap(y + dy) * SIDE + wrap(x + dx);
                    if (land[neighbour] != 0U)
                    {
                        low = std::min(low, basis.land_magnitude_m[neighbour]);
                        high = std::max(high, basis.land_magnitude_m[neighbour]);
                    }
                }
            }
            size_t const i = static_cast<size_t>(y) * SIDE + static_cast<size_t>(x);
            valid[i] = std::isfinite(low) && std::isfinite(high) ? 1U : 0U;
            basis.regional_relief_m[i] = valid[i] != 0U ? high - low : 0.0;
        }
    }
    fillUnsupported(basis.regional_relief_m, valid);
    auto const nearest_wet = nearestValid(wet);
    auto const nearest_land = nearestValid(land);
    basis.coast_distance_m.resize(COUNT);
    for (int32_t y = 0; y < static_cast<int32_t>(SIDE); ++y)
    {
        for (int32_t x = 0; x < static_cast<int32_t>(SIDE); ++x)
        {
            size_t const i = static_cast<size_t>(y) * SIDE + static_cast<size_t>(x);
            bool const is_land = land[i] != 0U;
            Feature const feature = (is_land ? nearest_wet : nearest_land)[
                index(y + static_cast<int32_t>(SIDE), x + static_cast<int32_t>(SIDE))
            ];
            double const dy = feature.y - (y + static_cast<int32_t>(SIDE));
            double const dx = feature.x - (x + static_cast<int32_t>(SIDE));
            double const distance = (std::sqrt(dy * dy + dx * dx) - 0.5) * SPACING_M;
            basis.coast_distance_m[i] = is_land ? distance : -distance;
        }
    }
    return basis;
}

std::array<double, 4> sampleV10FoundationBasis(V10FoundationBasis const& basis, double x_m, double y_m)
{
    if (!std::isfinite(x_m) || !std::isfinite(y_m))
    {
        throw std::invalid_argument("V10 foundation requires finite XY coordinates");
    }
    std::array<std::vector<double> const*, 4> const fields = {
        &basis.land_magnitude_m, &basis.ocean_magnitude_m,
        &basis.coast_distance_m, &basis.regional_relief_m,
    };
    double const gx = repeat(x_m) / SPACING_M;
    double const gy = repeat(y_m) / SPACING_M;
    auto const x0 = static_cast<size_t>(std::floor(gx)) % SIDE;
    auto const y0 = static_cast<size_t>(std::floor(gy)) % SIDE;
    size_t const x1 = (x0 + 1U) % SIDE;
    size_t const y1 = (y0 + 1U) % SIDE;
    double const tx = gx - std::floor(gx);
    double const ty = gy - std::floor(gy);
    std::array<double, 4> result;
    for (size_t field = 0U; field < fields.size(); ++field)
    {
        auto const& values = *fields[field];
        if (values.size() != COUNT)
        {
            throw std::invalid_argument("V10 foundation basis requires 256x256 fields");
        }
        double value = values[y0 * SIDE + x0] * (1.0 - ty) * (1.0 - tx);
        value += values[y0 * SIDE + x1] * (1.0 - ty) * tx;
        value += values[y1 * SIDE + x0] * ty * (1.0 - tx);
        value += values[y1 * SIDE + x1] * ty * tx;
        if (!std::isfinite(value))
        {
            throw std::invalid_argument("V10 foundation requires finite basis samples");
        }
        result[field] = value;
    }
    return result;
}

double sampleV10FoundationHeight(V10FoundationBasis const& basis, double x_m, double y_m)
{
    if (!std::isfinite(x_m) || !std::isfinite(y_m))
    {
        throw std::invalid_argument("V10 foundation requires finite XY coordinates");
    }
    // Query equations adapted from the reviewed native platec_v10.cpp. Only
    // basis ownership changed: all samples use native raw-derived fields.
    double const x = repeat(x_m);
    double const y = repeat(y_m);
    double const wx = x + 400.0 * noise(x / 2048.0, y / 2048.0, 32, 32, 42);
    double const wy = y + 400.0 * noise(x / 2048.0 + 11.0, y / 2048.0 - 7.0, 32, 32, 43);
    auto const fields = sampleV10FoundationBasis(basis, wx, wy);
    double const land = fields[0U];
    double const ocean = fields[1U];
    double const coast = fields[2U];
    double const broad = (coast >= 0.0 ? land : ocean) * std::tanh(coast / 384.0);
    double signal = 0.0;
    double carry = 1.0;
    double amplitude = 1.0;
    double normalizer = 0.0;
    for (int32_t octave = 0; octave < 5; ++octave)
    {
        int32_t const f = 24 * (1 << octave);
        int32_t const g = 8 * (1 << octave);
        double const u = f * (2.0 * wx + wy) / V10_FOUNDATION_PERIOD_M;
        double const v = g * (-wx + 2.0 * wy) / V10_FOUNDATION_PERIOD_M;
        double const n = noise(u, v, f, g, 42 + octave);
        double const ridge_base = std::clamp(1.0 - std::abs(2.0 * n), 0.0, 1.0);
        double const ridge = ridge_base * ridge_base * carry;
        signal += amplitude * ridge;
        normalizer += amplitude;
        carry = std::clamp(2.0 * ridge, 0.0, 1.0);
        amplitude *= 0.52;
    }
    signal /= normalizer;
    double const relief_amplitude = std::min(520.0, 0.75 * std::max(fields[3U] - 40.0, 0.0))
        * smoothstep(std::max(coast, 0.0) / 256.0);
    double const initial = broad + relief_amplitude * (signal - 0.25);
    return initial > 0.0 ? initial * (1.0 + 1.4 * smoothstep(initial / 240.0)) : initial;
}

} // namespace generation::detail
