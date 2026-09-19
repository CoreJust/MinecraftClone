#pragma once

#include <shared/world/Chunk.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

namespace shared {

struct HeightTileCoordinate final {
    int32_t x;
    int32_t y;

    constexpr bool operator==(HeightTileCoordinate const&) const noexcept = default;
};

struct HeightTile final {
    static constexpr uint8_t SIDE_LENGTH = Chunk::SIDE_LENGTH;
    static constexpr uint32_t SAMPLE_COUNT = Chunk::FACE_BLOCK_COUNT;

    using Heights = std::array<uint16_t, SAMPLE_COUNT>;

    HeightTileCoordinate coordinate{};
    Heights heights{};

    [[nodiscard]]
    std::optional<uint16_t> heightAt(BlockCoordinate coordinate) const noexcept;
};

class TerrainGenerator final {
public:
    TerrainGenerator();
    ~TerrainGenerator();

    TerrainGenerator(TerrainGenerator const&) = delete;
    TerrainGenerator& operator=(TerrainGenerator const&) = delete;
    TerrainGenerator(TerrainGenerator&&) noexcept;
    TerrainGenerator& operator=(TerrainGenerator&&) noexcept;

    [[nodiscard]]
    uint16_t heightAt(int64_t x, int64_t y) const noexcept;

    [[nodiscard]]
    HeightTile generateHeightTile(HeightTileCoordinate coordinate) const;

    [[nodiscard]]
    Chunk generateChunk(ChunkCoordinate coordinate) const;

    [[nodiscard]]
    bool usingCoreLang() const noexcept;

private:
    struct ScriptState;
    std::shared_ptr<ScriptState> m_script;
};

} // namespace shared
