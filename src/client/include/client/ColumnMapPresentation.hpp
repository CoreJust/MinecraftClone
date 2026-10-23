#pragma once

#include <shared/world/SparseWorld.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace client
{

struct WorldWindow final
{
    int64_t origin_x = 0;
    int64_t origin_y = 0;
    uint16_t width = 0U;
    uint16_t height = 0U;

    [[nodiscard]]
    bool operator==(WorldWindow const&) const noexcept = default;
};

using ColumnMapRgba8 = std::array<uint8_t, 4>;

struct ColumnMapPalette final
{
    std::array<ColumnMapRgba8, 256> colors{};
};

enum class ColumnCoverage : uint8_t
{
    Unknown,
    Ready,
};

struct ColumnMapPlanes final
{
    std::vector<ColumnMapRgba8> rgba;
    std::vector<uint8_t> material_ids;
    std::vector<ColumnCoverage> coverage;
    std::vector<int16_t> highest_solid_z;
};

struct ColumnMapUpdate final
{
    uint32_t scanned_columns = 0U;
    uint32_t ready_columns = 0U;
    uint32_t unknown_columns = 0U;
};

class ColumnMapPresentation final
{
public:
    static constexpr uint16_t MAX_TILE_SIDE = 256U;
    static constexpr uint32_t MAX_COLUMNS_PER_UPDATE = 256U;

    explicit ColumnMapPresentation(ColumnMapPalette palette) noexcept;

    [[nodiscard]]
    bool setWindow(WorldWindow window);

    [[nodiscard]]
    ColumnMapUpdate update(shared::SparseWorld const& world, uint32_t max_columns);

    [[nodiscard]]
    WorldWindow const& window() const noexcept;

    [[nodiscard]]
    ColumnMapPlanes const& planes() const noexcept;

    [[nodiscard]]
    uint32_t nextColumnIndex() const noexcept;

private:
    [[nodiscard]]
    ColumnCoverage sampleColumn(shared::SparseWorld const& world, uint32_t index);

private:
    ColumnMapPalette m_palette;
    WorldWindow m_window;
    ColumnMapPlanes m_planes;
    uint32_t m_next_column_index = 0U;
};

} // namespace client
