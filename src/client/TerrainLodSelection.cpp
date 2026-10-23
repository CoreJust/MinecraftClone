#include <client/TerrainLodSelection.hpp>

#include <shared/world/TerrainLod.hpp>
#include <generation/Generation.hpp>

#include <algorithm>
#include <stdexcept>

namespace client {

namespace {

uint32_t regionsPerAxis(shared::TerrainLodLevel const level, uint32_t const root_extent)
{
    uint32_t const index = static_cast<uint32_t>(level);
    if (index > static_cast<uint32_t>(shared::TerrainLodLevel::L3)) {
        throw std::invalid_argument("terrain LOD level is invalid");
    }
    if (root_extent != 128U && root_extent != 256U) {
        throw std::invalid_argument("terrain LOD root extent must match a 256 or 512 atlas");
    }
    return generation::WORLD_SIZE_BLOCKS / (level == shared::TerrainLodLevel::L3
        ? 16U : root_extent / (1U << (index * 2U)));
}

bool containsRegion(
    TerrainLodRegion const parent, TerrainLodRegion const child, uint32_t const root_extent
)
{
    if (parent.level > child.level
        || terrainLodRegionExtent(parent.level, root_extent) < terrainLodRegionExtent(child.level, root_extent)) {
        return false;
    }
    uint32_t const scale = regionsPerAxis(child.level, root_extent) / regionsPerAxis(parent.level, root_extent);
    return child.x / scale == parent.x && child.y / scale == parent.y;
}

} // namespace

TerrainLodRegion normalizeTerrainLodRegion(TerrainLodRegion region, uint32_t const root_extent)
{
    uint32_t const count = regionsPerAxis(region.level, root_extent);
    region.x %= count;
    region.y %= count;
    return region;
}

uint32_t terrainLodRegionExtent(shared::TerrainLodLevel const level, uint32_t const root_extent)
{
    return generation::WORLD_SIZE_BLOCKS / regionsPerAxis(level, root_extent);
}

std::array<TerrainLodRegion, 16> terrainLodChildren(TerrainLodRegion parent, uint32_t const root_extent)
{
    parent = normalizeTerrainLodRegion(parent, root_extent);
    if (parent.level >= shared::TerrainLodLevel::L2) {
        throw std::invalid_argument("L2 regions transition to actual chunk footprints");
    }
    auto const level = static_cast<shared::TerrainLodLevel>(static_cast<uint32_t>(parent.level) + 1U);
    std::array<TerrainLodRegion, 16> children;
    for (uint32_t y = 0U; y < 4U; ++y) {
        for (uint32_t x = 0U; x < 4U; ++x) {
            children[y * 4U + x] = { .level = level, .x = parent.x * 4U + x, .y = parent.y * 4U + y };
        }
    }
    return children;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
TerrainLodSelection::TerrainLodSelection(uint64_t const generation, uint32_t const root_extent)
    : m_generation(generation)
    , m_root_extent(root_extent)
{
    static_cast<void>(regionsPerAxis(shared::TerrainLodLevel::L0, root_extent));
}

void TerrainLodSelection::reset(uint64_t const generation) noexcept
{
    m_generation = generation;
    m_ready.clear();
    m_leaves.clear();
}

bool TerrainLodSelection::markReady(
    TerrainLodRegion region,
    uint64_t const generation,
    TerrainLodReadiness const readiness
)
{
    if (generation != m_generation) {
        return false;
    }
    region = normalizeTerrainLodRegion(region, m_root_extent);
    auto const found = m_ready.find(region);
    if (found != m_ready.end() && found->second > readiness) {
        return false;
    }
    m_ready.insert_or_assign(region, readiness);
    return true;
}

bool TerrainLodSelection::activateRoot(TerrainLodRegion root, uint64_t const generation)
{
    root = normalizeTerrainLodRegion(root, m_root_extent);
    if (generation != m_generation || root.level != shared::TerrainLodLevel::L0
        || active(root) || !uploaded(root)
        || std::ranges::any_of(m_leaves, [this, root](TerrainLodRegion const leaf) {
            return containsRegion(root, leaf, m_root_extent);
        })) {
        return false;
    }
    m_leaves.push_back(root);
    return true;
}

bool TerrainLodSelection::activateActualChunk(TerrainLodRegion chunk, uint64_t const generation)
{
    chunk = normalizeTerrainLodRegion(chunk, m_root_extent);
    if (generation != m_generation || chunk.level != shared::TerrainLodLevel::L3 || !uploaded(chunk)) {
        return false;
    }
    uint32_t const cell_extent = terrainLodRegionExtent(shared::TerrainLodLevel::L2, m_root_extent);
    uint32_t const count = 16U / cell_extent;
    std::array<TerrainLodRegion, 4> cells;
    uint32_t cell_count = 0U;
    for (uint32_t y = 0U; y < count; ++y) {
        for (uint32_t x = 0U; x < count; ++x) {
            TerrainLodRegion const cell{
                .level = shared::TerrainLodLevel::L2,
                .x = chunk.x * count + x,
                .y = chunk.y * count + y,
            };
            if (!active(cell)) {
                return false;
            }
            cells[cell_count] = cell;
            ++cell_count;
        }
    }
    std::vector<TerrainLodRegion> replacement;
    replacement.reserve(m_leaves.size() + 1U);
    for (TerrainLodRegion const leaf : m_leaves) {
        if (std::find(cells.begin(), cells.begin() + cell_count, leaf) == cells.begin() + cell_count) {
            replacement.push_back(leaf);
        }
    }
    replacement.push_back(chunk);
    m_leaves.swap(replacement);
    return true;
}

bool TerrainLodSelection::refine(TerrainLodRegion parent, uint64_t const generation)
{
    parent = normalizeTerrainLodRegion(parent, m_root_extent);
    if (generation != m_generation || parent.level >= shared::TerrainLodLevel::L2 || !active(parent)) {
        return false;
    }
    auto const children = terrainLodChildren(parent, m_root_extent);
    if (!std::ranges::all_of(children, [this](TerrainLodRegion const child) { return uploaded(child); })) {
        return false;
    }
    std::vector<TerrainLodRegion> replacement;
    replacement.reserve(m_leaves.size() + children.size() - 1U);
    for (TerrainLodRegion const leaf : m_leaves) {
        if (leaf != parent) {
            replacement.push_back(leaf);
        }
    }
    replacement.insert(replacement.end(), children.begin(), children.end());
    m_leaves.swap(replacement);
    return true;
}

bool TerrainLodSelection::coarsen(TerrainLodRegion parent, uint64_t const generation)
{
    parent = normalizeTerrainLodRegion(parent, m_root_extent);
    if (generation != m_generation || !uploaded(parent) || active(parent) || m_leaves.empty()) {
        return false;
    }
    bool const has_descendants = std::ranges::any_of(m_leaves, [this, parent](TerrainLodRegion const leaf) {
        return containsRegion(parent, leaf, m_root_extent);
    });
    if (!has_descendants) {
        return false;
    }
    std::vector<TerrainLodRegion> replacement;
    replacement.reserve(m_leaves.size() + 1U);
    for (TerrainLodRegion const leaf : m_leaves) {
        if (!containsRegion(parent, leaf, m_root_extent)) {
            replacement.push_back(leaf);
        }
    }
    replacement.push_back(parent);
    m_leaves.swap(replacement);
    return true;
}

bool TerrainLodSelection::uploaded(TerrainLodRegion region) const
{
    region = normalizeTerrainLodRegion(region, m_root_extent);
    auto const found = m_ready.find(region);
    return found != m_ready.end() && found->second == TerrainLodReadiness::Uploaded;
}

bool TerrainLodSelection::active(TerrainLodRegion region) const
{
    region = normalizeTerrainLodRegion(region, m_root_extent);
    return std::ranges::find(m_leaves, region) != m_leaves.end();
}

std::span<TerrainLodRegion const> TerrainLodSelection::leaves() const noexcept
{
    return m_leaves;
}

uint64_t TerrainLodSelection::generation() const noexcept
{
    return m_generation;
}

uint64_t TerrainLodSelection::RegionHash::operator()(TerrainLodRegion const region) const noexcept
{
    return (static_cast<uint64_t>(region.level) << 32U) | (static_cast<uint64_t>(region.x) << 16U) | region.y;
}

} // namespace client
