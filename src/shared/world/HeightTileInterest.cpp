#include <shared/world/HeightTileInterest.hpp>

#include <shared/world/SparseWorld.hpp>
#include <shared/world/World.hpp>

#include <algorithm>
#include <cmath>
#include <tuple>

namespace shared {

namespace {

constexpr int32_t HEIGHT_TILE_EXTENT = static_cast<int32_t>(
    WorldExtent::WIDTH / HEIGHT_TILE_SIDE_LENGTH
);

int32_t shortestDisplacement(int32_t const from, int32_t const to) noexcept
{
    int32_t displacement = to - from;
    if (displacement > HEIGHT_TILE_EXTENT / 2) {
        displacement -= HEIGHT_TILE_EXTENT;
    } else if (displacement < -HEIGHT_TILE_EXTENT / 2) {
        displacement += HEIGHT_TILE_EXTENT;
    }
    return displacement;
}

} // namespace

HeightTileGenerationBand heightTileGenerationBand(
    HeightTileKey const center,
    int8_t const heading_x,
    int8_t const heading_y,
    HeightTileKey const key
) noexcept
{
    HeightTileHeading const heading = canonicalHeightTileHeading(heading_x, heading_y);
    double const heading_length = std::hypot(heading.x, heading.y);
    double const forward_x = static_cast<double>(heading.x) / heading_length;
    double const forward_y = static_cast<double>(heading.y) / heading_length;
    double const x = shortestDisplacement(center.x, key.x);
    double const y = shortestDisplacement(center.y, key.y);
    double const parallel = x * forward_x + y * forward_y;
    double const perpendicular = -x * forward_y + y * forward_x;
    double const distance_squared = x * x + y * y;
    if (distance_squared <= 9.0) {
        return HeightTileGenerationBand::Immediate;
    }
    if (distance_squared <= 64.0) {
        return HeightTileGenerationBand::Near;
    }
    double const longitudinal_radius = parallel >= 0.0 ? 45.0 : 24.0;
    double const longitudinal = parallel / longitudinal_radius;
    double const lateral = perpendicular / 24.0;
    double const ellipse_distance = longitudinal * longitudinal + lateral * lateral;
    if (ellipse_distance <= 1.0) {
        return HeightTileGenerationBand::Directional;
    }
    return HeightTileGenerationBand::Background;
}

double heightTileInterestPriority(
    HeightTileKey const center,
    int8_t const heading_x,
    int8_t const heading_y,
    HeightTileKey const key
) noexcept
{
    HeightTileGenerationBand const band = heightTileGenerationBand(
        center, heading_x, heading_y, key
    );
    double const x = shortestDisplacement(center.x, key.x);
    double const y = shortestDisplacement(center.y, key.y);
    double const distance_squared = x * x + y * y;
    HeightTileHeading const heading = canonicalHeightTileHeading(heading_x, heading_y);
    double const heading_length = std::hypot(heading.x, heading.y);
    double const forward_x = static_cast<double>(heading.x) / heading_length;
    double const forward_y = static_cast<double>(heading.y) / heading_length;
    double const parallel = x * forward_x + y * forward_y;
    double const perpendicular = -x * forward_y + y * forward_x;
    double within_band = distance_squared / (45.0 * 45.0);
    if (band == HeightTileGenerationBand::Directional) {
        double const longitudinal_radius = parallel >= 0.0 ? 45.0 : 24.0;
        within_band = (parallel / longitudinal_radius) * (parallel / longitudinal_radius)
            + (perpendicular / 24.0) * (perpendicular / 24.0);
    }
    return static_cast<double>(band) + within_band - parallel / 100'000.0;
}

HeightTileHeading canonicalHeightTileHeading(int8_t heading_x, int8_t heading_y) noexcept
{
    double const heading_length = std::hypot(heading_x, heading_y);
    if (heading_length < 1.0) {
        heading_x = 0;
        heading_y = 127;
    }
    constexpr double PI = 3.141'592'653'589'793'238'46;
    constexpr double HEADING_SECTORS = 16.0;
    double const angle = std::atan2(heading_y, heading_x);
    double const quantized_angle = std::round(angle * HEADING_SECTORS / (2.0 * PI))
        * (2.0 * PI / HEADING_SECTORS);
    double const forward_x = std::cos(quantized_angle);
    double const forward_y = std::sin(quantized_angle);
    return {
        .x = static_cast<int8_t>(std::round(forward_x * 127.0)),
        .y = static_cast<int8_t>(std::round(forward_y * 127.0)),
    };
}

HeightTileInterest makeHeightTileInterest(
    HeightTileKey const center,
    int8_t heading_x,
    int8_t heading_y
)
{
    constexpr int32_t RESIDENCY_RADIUS = 45;
    HeightTileHeading const heading = canonicalHeightTileHeading(heading_x, heading_y);
    heading_x = heading.x;
    heading_y = heading.y;
    double const heading_length = std::hypot(heading_x, heading_y);
    double const forward_x = static_cast<double>(heading_x) / heading_length;
    double const forward_y = static_cast<double>(heading_y) / heading_length;

    HeightTileInterest result{.heading_x = heading_x, .heading_y = heading_y};
    result.keys.reserve(HEIGHT_TILE_INTEREST_COUNT);
    for (int32_t y = -RESIDENCY_RADIUS; y <= RESIDENCY_RADIUS; ++y) {
        for (int32_t x = -RESIDENCY_RADIUS; x <= RESIDENCY_RADIUS; ++x) {
            if (x * x + y * y > RESIDENCY_RADIUS * RESIDENCY_RADIUS) {
                continue;
            }
            result.keys.push_back(normalizeHeightTileKey({.x = center.x + x, .y = center.y + y}));
        }
    }
    std::ranges::sort(result.keys, [center, heading_x, heading_y, forward_x, forward_y](
        HeightTileKey const first,
        HeightTileKey const second
    ) {
        auto const priority = [center, heading_x, heading_y, forward_x, forward_y](HeightTileKey const key) {
            int64_t const x = shortestDisplacement(center.x, key.x);
            int64_t const y = shortestDisplacement(center.y, key.y);
            int64_t const distance = x * x + y * y;
            double const ahead = x * forward_x + y * forward_y;
            return std::tuple{
                heightTileInterestPriority(center, heading_x, heading_y, key),
                distance,
                -ahead,
                key.y,
                key.x,
            };
        };
        return priority(first) < priority(second);
    });
    return result;
}

} // namespace shared
