#include <shared/world/HeightTileInterest.hpp>

#include <shared/world/SparseWorld.hpp>
#include <shared/world/World.hpp>

#include <core/common/Assert.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

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

int32_t rowHalfWidth(int32_t const offset, int32_t const radius) noexcept
{
    if (offset < -radius || offset > radius) {
        return -1;
    }
    return static_cast<int32_t>(std::sqrt(static_cast<double>(radius * radius - offset * offset)));
}

void appendDifference(
    std::vector<HeightTileKey>& keys,
    HeightTileKey const center,
    int32_t const center_dx,
    int32_t const center_dy,
    int32_t const radius
)
{
    for (int32_t y = -radius; y <= radius; ++y) {
        int32_t const half_width = rowHalfWidth(y, radius);
        int32_t const other_half_width = rowHalfWidth(y + center_dy, radius);
        int32_t const first_end = other_half_width < 0
            ? half_width
            : std::min(half_width, -center_dx - other_half_width - 1);
        for (int32_t x = -half_width; x <= first_end; ++x) {
            keys.push_back(normalizeHeightTileKey({ .x = center.x + x, .y = center.y + y }));
        }
        if (other_half_width < 0) {
            continue;
        }
        int32_t const second_begin = std::max(-half_width, -center_dx + other_half_width + 1);
        for (int32_t x = std::max(second_begin, first_end + 1); x <= half_width; ++x) {
            keys.push_back(normalizeHeightTileKey({ .x = center.x + x, .y = center.y + y }));
        }
    }
}

struct RankedHeightTileOffset final {
    HeightTileKey offset;
    double priority;
    int64_t distance_squared;
    double negative_ahead;
};

HeightTileInterestOrders buildHeightTileOffsetOrders(uint32_t const radius)
{
    int32_t const residency_radius = static_cast<int32_t>(radius);
    constexpr double PI = 3.141'592'653'589'793'238'46;
    HeightTileInterestOrders result{.radius = radius};
    for (uint32_t sector = 0U; sector < result.orders.size(); ++sector) {
        double const angle = static_cast<double>(sector) * 2.0 * PI / static_cast<double>(result.orders.size());
        HeightTileHeading const heading{
            .x = static_cast<int8_t>(std::round(std::cos(angle) * 127.0)),
            .y = static_cast<int8_t>(std::round(std::sin(angle) * 127.0)),
        };
        double const heading_length = std::hypot(heading.x, heading.y);
        double const forward_x = static_cast<double>(heading.x) / heading_length;
        double const forward_y = static_cast<double>(heading.y) / heading_length;
        std::vector<RankedHeightTileOffset> ranked;
        ranked.reserve(heightTileInterestCount(radius));
        for (int32_t y = -residency_radius; y <= residency_radius; ++y) {
            for (int32_t x = -residency_radius; x <= residency_radius; ++x) {
                int64_t const distance_squared = static_cast<int64_t>(x) * x + static_cast<int64_t>(y) * y;
                if (distance_squared > residency_radius * residency_radius) {
                    continue;
                }
                HeightTileKey const offset{.x = x, .y = y};
                ranked.push_back({
                    .offset = offset,
                    .priority = heightTileInterestPriority({.x = 0, .y = 0}, heading.x, heading.y, offset),
                    .distance_squared = distance_squared,
                    .negative_ahead = -(x * forward_x + y * forward_y),
                });
            }
        }
        std::ranges::sort(ranked, [](RankedHeightTileOffset const& first, RankedHeightTileOffset const& second) {
            return std::tie(
                first.priority,
                first.distance_squared,
                first.negative_ahead,
                first.offset.y,
                first.offset.x
            ) < std::tie(
                second.priority,
                second.distance_squared,
                second.negative_ahead,
                second.offset.y,
                second.offset.x
            );
        });
        HeightTileInterest& order = result.orders[sector];
        order.heading_x = heading.x;
        order.heading_y = heading.y;
        order.keys.reserve(ranked.size());
        for (RankedHeightTileOffset const& entry : ranked) {
            order.keys.push_back(entry.offset);
        }
    }
    return result;
}

} // namespace

std::shared_ptr<HeightTileInterestOrders const> prepareHeightTileInterestOrders(uint32_t const radius)
{
    if (!isValidHeightTileInterestRadius(radius)) {
        throw std::invalid_argument{"render distance must be an integer chunk radius from 1 to 256"};
    }
    static std::mutex cache_mutex;
    static std::array<std::weak_ptr<HeightTileInterestOrders const>, MAX_HEIGHT_TILE_INTEREST_RADIUS + 1U> cache;
    static std::shared_ptr<HeightTileInterestOrders const> default_orders;
    std::lock_guard const lock{cache_mutex};
    if (auto const existing = cache[radius].lock()) {
        return existing;
    }
    auto const orders = std::make_shared<HeightTileInterestOrders const>(buildHeightTileOffsetOrders(radius));
    cache[radius] = orders;
    if (radius == HEIGHT_TILE_INTEREST_RADIUS) {
        default_orders = orders;
    }
    return orders;
}

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

HeightTileInterestDelta heightTileInterestDelta(
    HeightTileKey const previous_center,
    HeightTileKey const next_center,
    uint32_t const radius
)
{
    ASSERT(isValidHeightTileInterestRadius(radius), "invalid height-tile interest radius");
    HeightTileInterestDelta result;
    HeightTileKey const normalized_previous = normalizeHeightTileKey(previous_center);
    HeightTileKey const normalized_next = normalizeHeightTileKey(next_center);
    int32_t const dx = shortestDisplacement(normalized_previous.x, normalized_next.x);
    int32_t const dy = shortestDisplacement(normalized_previous.y, normalized_next.y);
    if (dx == 0 && dy == 0) {
        return result;
    }
    appendDifference(result.additions, normalized_next, dx, dy, static_cast<int32_t>(radius));
    appendDifference(result.removals, normalized_previous, -dx, -dy, static_cast<int32_t>(radius));
    return result;
}

bool heightTileWithinInterest(
    HeightTileKey const center,
    HeightTileKey const key,
    uint32_t const radius
)
{
    ASSERT(isValidHeightTileInterestRadius(radius), "invalid height-tile interest radius");
    HeightTileKey const normalized_center = normalizeHeightTileKey(center);
    HeightTileKey const normalized_key = normalizeHeightTileKey(key);
    int32_t const dx = shortestDisplacement(normalized_center.x, normalized_key.x);
    int32_t const dy = shortestDisplacement(normalized_center.y, normalized_key.y);
    int64_t const distance_squared = static_cast<int64_t>(dx) * dx
        + static_cast<int64_t>(dy) * dy;
    int64_t const radius_squared = static_cast<int64_t>(radius) * radius;
    return distance_squared <= radius_squared;
}

HeightTileInterest makeHeightTileInterest(
    HeightTileKey const center,
    int8_t heading_x,
    int8_t heading_y,
    uint32_t const radius
)
{
    if (radius == HEIGHT_TILE_INTEREST_RADIUS) {
        static auto const default_orders = prepareHeightTileInterestOrders();
        return makeHeightTileInterest(center, heading_x, heading_y, *default_orders);
    }
    auto const orders = prepareHeightTileInterestOrders(radius);
    return makeHeightTileInterest(center, heading_x, heading_y, *orders);
}

HeightTileInterest makeHeightTileInterest(
    HeightTileKey const center,
    int8_t heading_x,
    int8_t heading_y,
    HeightTileInterestOrders const& orders
)
{
    HeightTileHeading const heading = canonicalHeightTileHeading(heading_x, heading_y);
    heading_x = heading.x;
    heading_y = heading.y;

    HeightTileInterest result{.heading_x = heading_x, .heading_y = heading_y};
    auto const order = std::ranges::find_if(orders.orders, [heading](HeightTileInterest const& order) {
        return order.heading_x == heading.x && order.heading_y == heading.y;
    });
    ASSERT(order != orders.orders.end(), "invalid height-tile interest orders");
    HeightTileInterest const& offsets = *order;
    result.keys.reserve(offsets.keys.size());
    for (HeightTileKey const offset : offsets.keys) {
        result.keys.push_back(normalizeHeightTileKey({.x = center.x + offset.x, .y = center.y + offset.y}));
    }
    return result;
}

std::vector<HeightTileKey> selectHeightTileRemovalCandidates(
    HeightTileKey const center,
    int8_t const heading_x,
    int8_t const heading_y,
    std::span<HeightTileKey const> const pending,
    std::span<HeightTileKey const> const inflight,
    uint32_t const maximum_count
) {
    struct HeightTileKeyHash final {
        [[nodiscard]]
        uint64_t operator()(HeightTileKey const key) const noexcept
        {
            return heightTileCoordinateHash(key.x, key.y);
        }
    };

    std::unordered_set<HeightTileKey, HeightTileKeyHash> const inflight_set{
        inflight.begin(), inflight.end()
    };
    std::vector<HeightTileKey> candidates;
    candidates.reserve(pending.size());
    for (HeightTileKey const key : pending) {
        if (!inflight_set.contains(key)) {
            candidates.push_back(key);
        }
    }
    auto const order = [center, heading_x, heading_y](HeightTileKey const first, HeightTileKey const second) {
        return std::tuple{
            heightTileInterestPriority(center, heading_x, heading_y, first),
            first.y,
            first.x,
        } > std::tuple{
            heightTileInterestPriority(center, heading_x, heading_y, second),
            second.y,
            second.x,
        };
    };
    if (candidates.size() > maximum_count) {
        std::ranges::partial_sort(candidates, candidates.begin() + maximum_count, order);
        candidates.resize(maximum_count);
    } else {
        std::ranges::sort(candidates, order);
    }
    return candidates;
}

} // namespace shared
