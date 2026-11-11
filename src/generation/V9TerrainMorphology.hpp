#pragma once

#include <cstdint>
#include <span>
#include <stop_token>
#include <vector>

namespace generation::detail {

// Canonical 256-square bed in metres, with 256 m vertex spacing and a
// provisional ocean mask in row-major order. Missing ocean shores throw
// runtime_error; cancellation throws GenerationCancelled.
[[nodiscard]]
std::vector<double> evolveV9Terrain(
    std::span<double const> bed,
    std::span<uint8_t const> ocean,
    std::stop_token cancellation = {}
);

} // namespace generation::detail
