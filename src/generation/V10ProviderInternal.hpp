#pragma once

#include "V10ClimatePreparation.hpp"
#include "V10FineTerrain.hpp"
#include "V10Foundation.hpp"
#include "V10PlatecSource.hpp"
#include "V10WaterPreparation.hpp"

#include <generation/Generation.hpp>
#include <generation/RiverBedField.hpp>

#include <cstddef>
#include <memory>
#include <mutex>

namespace generation::detail
{

struct V10FineTerrainCache final
{
    std::once_flag preparation;
    std::unique_ptr<V10FineTerrain const> terrain;
    size_t prepared_parent_queries = 0U;
    bool recording_preparation_queries = false;
};

struct V10ProviderState final
{
    V10PlatecSourceResult source;
    V10FoundationBasis foundation;
    std::vector<double> canonical_l0_m;
    V10ClimatePreparation climate;
    V10WaterPreparation water;
    std::vector<GenerationPlateDirection> plate_directions;
    std::shared_ptr<river_field_candidate::PreparedRiverField const> river_field;
    bool use_v10_fine_terrain = false;
    // Declared last so its field callback is destroyed before the borrowed foundation.
    std::unique_ptr<V10FineTerrainCache> fine_terrain_cache;
};

[[nodiscard]] std::shared_ptr<GlobalAtlas const> prepareV10GlobalAtlas(
    GenerationConfiguration const& configuration, std::stop_token cancellation, ProgressCallback progress
);
[[nodiscard]] bool isValidV10Snapshot(GenerationSnapshot const& snapshot) noexcept;
[[nodiscard]] bool isValidV10Atlas(GlobalAtlas const& atlas) noexcept;

} // namespace generation::detail
