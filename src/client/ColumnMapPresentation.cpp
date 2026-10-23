#include <client/ColumnMapPresentation.hpp>

#include <shared/world/Chunk.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>

#include <algorithm>
#include <optional>
#include <utility>

namespace client
{

ColumnMapPresentation::ColumnMapPresentation(ColumnMapPalette palette) noexcept
    : m_palette{ palette }
{
}

bool ColumnMapPresentation::setWindow(WorldWindow const window)
{
    if (window.width == 0U
        || window.height == 0U
        || window.width > MAX_TILE_SIDE
        || window.height > MAX_TILE_SIDE) {
        return false;
    }

    uint32_t const column_count = static_cast<uint32_t>(window.width) * window.height;
    m_window = window;
    m_planes.rgba.assign(column_count, ColumnMapRgba8{ 0U, 0U, 0U, 0U });
    m_planes.material_ids.assign(column_count, static_cast<uint8_t>(shared::Block::Air));
    m_planes.coverage.assign(column_count, ColumnCoverage::Unknown);
    m_planes.highest_solid_z.assign(column_count, -1);
    m_next_column_index = 0U;
    return true;
}

ColumnMapUpdate ColumnMapPresentation::update(
    shared::SparseWorld const& world,
    uint32_t const max_columns
)
{
    ColumnMapUpdate result;
    uint32_t const column_count = static_cast<uint32_t>(m_planes.coverage.size());
    if (column_count == 0U || max_columns == 0U) {
        return result;
    }

    uint32_t const budget = std::min({ max_columns, MAX_COLUMNS_PER_UPDATE, column_count });
    for (uint32_t offset = 0U; offset < budget; ++offset) {
        static_cast<void>(sampleColumn(world, m_next_column_index));
        if (m_planes.coverage[m_next_column_index] == ColumnCoverage::Ready) {
            ++result.ready_columns;
        } else {
            ++result.unknown_columns;
        }
        ++result.scanned_columns;
        ++m_next_column_index;
        if (m_next_column_index == column_count) {
            m_next_column_index = 0U;
        }
    }
    return result;
}

WorldWindow const& ColumnMapPresentation::window() const noexcept
{
    return m_window;
}

ColumnMapPlanes const& ColumnMapPresentation::planes() const noexcept
{
    return m_planes;
}

uint32_t ColumnMapPresentation::nextColumnIndex() const noexcept
{
    return m_next_column_index;
}

ColumnCoverage ColumnMapPresentation::sampleColumn(
    shared::SparseWorld const& world,
    uint32_t const index
)
{
    uint32_t const x_offset = index % m_window.width;
    uint32_t const y_offset = index / m_window.width;
    uint32_t const world_x = (shared::WorldBounds::wrapHorizontal(m_window.origin_x) + x_offset)
        % shared::WorldExtent::WIDTH;
    uint32_t const world_y = (shared::WorldBounds::wrapHorizontal(m_window.origin_y) + y_offset)
        % shared::WorldExtent::WIDTH;
    if (std::optional<shared::VerifiedSurfaceColumn> const verified = world.verifiedSurfaceColumn(
            static_cast<int64_t>(world_x),
            static_cast<int64_t>(world_y)
        )) {
        uint8_t const material_id = static_cast<uint8_t>(verified->material_at_highest_solid);
        m_planes.rgba[index] = m_palette.colors[material_id];
        m_planes.material_ids[index] = material_id;
        m_planes.coverage[index] = ColumnCoverage::Ready;
        m_planes.highest_solid_z[index] = static_cast<int16_t>(verified->highest_solid_z);
        return ColumnCoverage::Ready;
    }

    for (int32_t z = static_cast<int32_t>(shared::WorldExtent::DEPTH) - 1; z >= 0; --z) {
        shared::SparseBlockQuery const query = world.queryBlock({
            .x = static_cast<int64_t>(world_x),
            .y = static_cast<int64_t>(world_y),
            .z = z,
        });
        if (query.state != shared::GenerationState::Materialized || !query.block) {
            m_planes.rgba[index] = { 0U, 0U, 0U, 0U };
            m_planes.material_ids[index] = static_cast<uint8_t>(shared::Block::Air);
            m_planes.coverage[index] = ColumnCoverage::Unknown;
            m_planes.highest_solid_z[index] = -1;
            return ColumnCoverage::Unknown;
        }
        if (*query.block != shared::Block::Air) {
            uint8_t const material_id = static_cast<uint8_t>(*query.block);
            m_planes.rgba[index] = m_palette.colors[material_id];
            m_planes.material_ids[index] = material_id;
            m_planes.coverage[index] = ColumnCoverage::Ready;
            m_planes.highest_solid_z[index] = static_cast<int16_t>(z);
            return ColumnCoverage::Ready;
        }
    }

    uint8_t const air_id = static_cast<uint8_t>(shared::Block::Air);
    m_planes.rgba[index] = m_palette.colors[air_id];
    m_planes.material_ids[index] = air_id;
    m_planes.coverage[index] = ColumnCoverage::Ready;
    m_planes.highest_solid_z[index] = -1;
    return ColumnCoverage::Ready;
}

} // namespace client
