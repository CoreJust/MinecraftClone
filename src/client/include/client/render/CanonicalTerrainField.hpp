#pragma once

#include <generation/Generation.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace client::render {

using CanonicalTerrainFieldValue = std::array<float, 4U>;

// V9 category is a discrete source-cell surface material; the other values
// retain the existing temperature/elevation samples used by R6 shading.
[[nodiscard]]
std::vector<CanonicalTerrainFieldValue> buildV9CanonicalTerrainFieldValues(
    generation::GlobalAtlas const& atlas
);

} // namespace client::render
