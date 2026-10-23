#include <shared/world/Chunk.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace shared {

namespace {

constexpr int64_t WORLD_XY_WIDTH = 65'536;

static_assert(REGISTERED_WORLD_GENERATION_EXTENTS == TERRAIN_REGION_EXTENTS);

[[nodiscard]]
bool isRegisteredExtent(uint32_t const extent) noexcept
{
    return std::ranges::find(REGISTERED_WORLD_GENERATION_EXTENTS, extent)
        != REGISTERED_WORLD_GENERATION_EXTENTS.end();
}

} // namespace

std::optional<GenerationRegionKey> generationRegionForChunk(
    ChunkCoordinate const coordinate,
    uint32_t const extent,
    GenerationStage const stage,
    uint64_t const revision,
    uint64_t const seed
) noexcept
{
    if (!isRegisteredExtent(extent)
        || (stage != GenerationStage::Pregen && stage != GenerationStage::Refinement)) {
        return std::nullopt;
    }
    auto const canonicalOrigin = [extent](int32_t const chunk_coordinate) {
        int64_t const chunk_offset = static_cast<int64_t>(chunk_coordinate) * Chunk::SIDE_LENGTH;
        int64_t const wrapped = (chunk_offset % WORLD_XY_WIDTH + WORLD_XY_WIDTH) % WORLD_XY_WIDTH;
        return static_cast<int32_t>((wrapped / static_cast<int64_t>(extent)) * extent);
    };
    return GenerationRegionKey{
        .revision = revision,
        .seed = seed,
        .origin_x = canonicalOrigin(coordinate.x),
        .origin_y = canonicalOrigin(coordinate.y),
        .extent = extent,
        .stage = stage,
    };
}

bool isValidWorldGenerationPlan(WorldGenerationPlan const& plan) noexcept
{
    if (plan.refinement_extents.size() > REGISTERED_WORLD_GENERATION_EXTENTS.size()
        || plan.priority_order.size() > DEFAULT_GENERATION_PRIORITY_ORDER.size()) {
        return false;
    }
    std::array<bool, 3> seen_priorities{};
    for (GenerationPriorityDimension const dimension : plan.priority_order) {
        size_t const index = static_cast<size_t>(dimension);
        if (index >= seen_priorities.size() || seen_priorities[index]) {
            return false;
        }
        seen_priorities[index] = true;
    }

    if (plan.pre_generate && !plan.refinement_extents.empty()
        && plan.refinement_extents.front() == REGISTERED_WORLD_GENERATION_EXTENTS.front()) {
        return false;
    }
    uint32_t previous_extent = plan.pre_generate
        ? REGISTERED_WORLD_GENERATION_EXTENTS.front()
        : std::numeric_limits<uint32_t>::max();
    for (uint32_t const extent : plan.refinement_extents) {
        if (!isRegisteredExtent(extent) || extent >= previous_extent) {
            return false;
        }
        previous_extent = extent;
    }

    bool has_height_tile = false;
    bool has_materialize = false;
    for (size_t index = 0U; index < plan.chunk_stages.size(); ++index) {
        switch (plan.chunk_stages[index]) {
        case GenerationStage::HeightTile:
            if (has_height_tile) {
                return false;
            }
            has_height_tile = true;
            break;
        case GenerationStage::Materialize:
            if (has_materialize || index + 1U != plan.chunk_stages.size()) {
                return false;
            }
            has_materialize = true;
            break;
        default:
            return false;
        }
    }
    return plan.pre_generate || !plan.refinement_extents.empty() || !plan.chunk_stages.empty();
}

} // namespace shared
