#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <stdexcept>

namespace client::detail {

template <typename Recreate>
[[nodiscard]] bool tryGrowStoneFaceCapacity(
    uint32_t& capacity,
    uint32_t const required_capacity,
    uint32_t const maximum_capacity,
    std::chrono::steady_clock::time_point const deadline,
    Recreate&& recreate
)
{
    if (capacity == 0U || maximum_capacity == 0U || capacity > maximum_capacity
        || required_capacity > maximum_capacity) {
        throw std::invalid_argument("invalid stone face capacity request");
    }
    if (required_capacity <= capacity) {
        return true;
    }

    uint32_t const previous_capacity = capacity;
    uint32_t next_capacity = capacity;
    while (next_capacity < required_capacity) {
        if (next_capacity > maximum_capacity / 2U) {
            next_capacity = maximum_capacity;
            break;
        }
        next_capacity *= 2U;
    }
    capacity = next_capacity;
    try {
        std::invoke(recreate, deadline);
    } catch (...) {
        capacity = previous_capacity;
        std::invoke(recreate, deadline);
        return false;
    }
    return true;
}

} // namespace client::detail
