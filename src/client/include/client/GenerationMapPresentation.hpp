#pragma once

#include <generation/Generation.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace client
{

using GenerationMapRgba8 = std::array<uint8_t, 4U>;

struct GenerationMapImage final
{
    uint32_t width = 0U;
    uint32_t height = 0U;
    std::vector<GenerationMapRgba8> rgba;
};

// Keeps one elevation palette across stages and blends in available hydrology
// or climate fields. Climate-bearing drainage stages show canonical cold-land
// snow and below-freezing ocean tint from actual climate. Relief is shaded from
// elevation; the result is diagnostic, not authoritative material or physical ice data.
[[nodiscard]]
std::optional<GenerationMapImage> rasterizeGenerationMap(
    generation::GenerationSnapshot const& snapshot
);

} // namespace client
