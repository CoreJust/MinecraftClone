#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace client {

template <typename Obstructed>
[[nodiscard]] double maximumUnobstructedCameraDistance(
    double const maximum_distance,
    uint32_t const sample_count,
    uint32_t const binary_steps,
    double const margin,
    Obstructed const& obstructed
) noexcept
{
    if (!std::isfinite(maximum_distance) || maximum_distance <= 0.0 || sample_count == 0U) {
        return 0.0;
    }
    if (obstructed(0.0)) {
        return 0.0;
    }
    double clear_distance = 0.0;
    double blocked_distance = maximum_distance;
    bool found_obstruction = false;
    for (uint32_t sample = 1U; sample <= sample_count; ++sample) {
        double const distance = maximum_distance
            * static_cast<double>(sample)
            / static_cast<double>(sample_count);
        if (obstructed(distance)) {
            blocked_distance = distance;
            found_obstruction = true;
            break;
        }
        clear_distance = distance;
    }
    if (!found_obstruction) {
        return maximum_distance;
    }
    for (uint32_t step = 0U; step < binary_steps; ++step) {
        double const midpoint = (clear_distance + blocked_distance) * 0.5;
        if (obstructed(midpoint)) {
            blocked_distance = midpoint;
        } else {
            clear_distance = midpoint;
        }
    }
    return std::max(0.0, clear_distance - margin);
}

} // namespace client
