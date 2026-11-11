#ifndef CUBIC_PERIODIC_H
#define CUBIC_PERIODIC_H

#include <array>
#include <cmath>
#include <cstdint>

inline std::array<double, 4> cubicWeights(const double t)
{
    const double t2 = t * t;
    const double t3 = t2 * t;
    return {-.5 * t + t2 - .5 * t3, 1 - 2.5 * t2 + 1.5 * t3,
        .5 * t + 2 * t2 - 1.5 * t3, -.5 * t2 + .5 * t3};
}

template <typename Sample>
double cubicPeriodic(const double x, const double y, const uint32_t period, Sample sample)
{
    const auto ix = static_cast<int64_t>(std::floor(x));
    const auto iy = static_cast<int64_t>(std::floor(y));
    const auto wx = cubicWeights(x - ix);
    const auto wy = cubicWeights(y - iy);
    double result = 0;
    for (uint32_t j = 0; j < 4; ++j) {
        double row = 0;
        for (uint32_t i = 0; i < 4; ++i) {
            const auto cx = static_cast<uint32_t>((ix + i - 1 + period) % period);
            const auto cy = static_cast<uint32_t>((iy + j - 1 + period) % period);
            row += wx[i] * sample(cx, cy);
        }
        result += wy[j] * row;
    }
    return result;
}
#endif
