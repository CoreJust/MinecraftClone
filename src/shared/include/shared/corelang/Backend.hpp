#pragma once

#include <shared/corelang/ExecutionTypes.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>
#include <generation/Generation.hpp>

#include <expected>
#include <stop_token>
#include <string_view>

namespace shared::corelang {

struct TerrainPlan final {
    generation::GenerationConfiguration configuration;
    WorldGenerationPlan world_generation_plan;
    BackendReceipt receipt;
};

[[nodiscard]]
std::string_view terrainPlanSource() noexcept;

[[nodiscard]]
std::expected<TerrainPlan, BackendError>
prepareTerrainPlan(generation::GenerationConfiguration configuration,
                   Backend backend,
                   std::stop_token const& cancellation = {});

[[nodiscard]]
std::expected<TerrainPlan, BackendError>
prepareTerrainPlan(generation::GenerationConfiguration configuration,
                   Backend backend,
                   std::string_view manifest_source,
                   std::stop_token const& cancellation = {});

} // namespace shared::corelang
