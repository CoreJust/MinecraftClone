#pragma once

#include "V10Climate.hpp"
#include "V9Climate.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <stop_token>

namespace generation::detail
{

struct V10ClimatePreparation final
{
    // Fresh solver output retained for the owning water producer's coupling.
    V9ClimateResult climate;
    V10ClimateForcing forcing;
};

// Frozen seed-42 V10 producer: actual canonical 256-square L0 vertices in
// one-metre blocks. Only the climate input is multiplied by ten; geometry is not.
// Reuses the v7 eight-phase/180-day/three-year native solver, not V21 state.
[[nodiscard]]
std::expected<V10ClimatePreparation, V9ClimateFailure> prepareV10Climate(
    std::span<double const> canonical_l0_m, uint64_t seed,
    std::stop_token cancellation = {}, V9ClimateProgressCallback progress = {}
);

} // namespace generation::detail
