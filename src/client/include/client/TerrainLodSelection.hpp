#pragma once

#include <shared/world/TerrainLod.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace client {

struct TerrainLodRegion final {
    shared::TerrainLodLevel level = shared::TerrainLodLevel::L0;
    uint32_t x = 0U;
    uint32_t y = 0U;
    bool operator==(TerrainLodRegion const&) const = default;
};

enum class TerrainLodReadiness : uint8_t { Source, Mesh, Uploaded };

[[nodiscard]]
TerrainLodRegion normalizeTerrainLodRegion(TerrainLodRegion region, uint32_t root_extent = 256U);
[[nodiscard]]
uint32_t terrainLodRegionExtent(shared::TerrainLodLevel level, uint32_t root_extent = 256U);
[[nodiscard]]
std::array<TerrainLodRegion, 16> terrainLodChildren(TerrainLodRegion parent, uint32_t root_extent = 256U);

class TerrainLodSelection final {
public:
    explicit TerrainLodSelection(uint64_t generation = 0U, uint32_t root_extent = 256U);
    void reset(uint64_t generation) noexcept;
    [[nodiscard]]
    bool markReady(TerrainLodRegion region, uint64_t generation, TerrainLodReadiness readiness);
    [[nodiscard]]
    bool activateRoot(TerrainLodRegion root, uint64_t generation);
    [[nodiscard]]
    bool activateActualChunk(TerrainLodRegion chunk, uint64_t generation);
    [[nodiscard]]
    bool refine(TerrainLodRegion parent, uint64_t generation);
    [[nodiscard]]
    bool coarsen(TerrainLodRegion parent, uint64_t generation);
    [[nodiscard]]
    bool uploaded(TerrainLodRegion region) const;
    [[nodiscard]]
    bool active(TerrainLodRegion region) const;
    [[nodiscard]]
    std::span<TerrainLodRegion const> leaves() const noexcept;
    [[nodiscard]]
    uint64_t generation() const noexcept;

private:
    struct RegionHash final {
        uint64_t operator()(TerrainLodRegion region) const noexcept;
    };
    uint64_t m_generation;
    uint32_t m_root_extent;
    std::unordered_map<TerrainLodRegion, TerrainLodReadiness, RegionHash> m_ready;
    std::vector<TerrainLodRegion> m_leaves;
};

} // namespace client
