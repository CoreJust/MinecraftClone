#include <shared/world/WorldGeneration.hpp>

#include <shared/HeightTileScript.hpp>
#include <shared/world/SparseWorld.hpp>

#include <core/lang/CoreLang.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

constexpr uint32_t HEIGHT_TILE_INDEX_MULTIPLIER = shared::HeightTile::SIDE_LENGTH;
constexpr double WORLD_CENTER = 32'768.0;
constexpr uint16_t MAXIMUM_HEIGHT = static_cast<uint16_t>(shared::WorldExtent::DEPTH);

[[nodiscard]]
uint16_t kernelHeightAt(
    int64_t const x,
    int64_t const y,
    double const base,
    double const spacing,
    double const amplitude
) noexcept {
    double const dx = static_cast<double>(x) - WORLD_CENTER;
    double const dy = static_cast<double>(y) - WORLD_CENTER;
    double const distance = std::sqrt(dx * dx + dy * dy);
    double const n = std::floor(distance / spacing + 0.5);
    double const angle = std::numbers::pi_v<double> * distance / spacing;
    double const height = std::floor(base + amplitude / (n + 1.0) * std::cos(angle) * std::cos(angle));
    return static_cast<uint16_t>(std::clamp(height, 0.0, static_cast<double>(MAXIMUM_HEIGHT)));
}

[[nodiscard]]
uint32_t sampleIndex(shared::BlockCoordinate const coordinate) noexcept
{
    return static_cast<uint32_t>(coordinate.y) * HEIGHT_TILE_INDEX_MULTIPLIER + coordinate.x;
}

} // namespace

namespace shared {

std::optional<uint16_t> HeightTile::heightAt(BlockCoordinate const coordinate) const noexcept
{
    if (!Chunk::isValid(coordinate)) {
        return std::nullopt;
    }
    return heights[sampleIndex(coordinate)];
}

struct TerrainGenerator::ScriptState final {
    core::lang::Runtime runtime;
    std::optional<core::lang::Program> program;
    double base{0.0};
    double spacing{0.0};
    double amplitude{0.0};

    [[nodiscard]]
    double parameter(std::string_view const name)
    {
        auto const outcome = runtime.execute(*program, "s6_height_tile", name, {}, {});
        if (!outcome || !std::holds_alternative<core::lang::Completed>(*outcome)) {
            throw std::runtime_error{"CoreLang terrain parameter failed"};
        }
        core::lang::Value const& value = std::get<core::lang::Completed>(*outcome).value;
        if (value.type.kind != core::lang::TypeKind::F64 || value.bytes.size() != sizeof(double)) {
            throw std::runtime_error{"CoreLang terrain parameter has invalid type"};
        }
        uint64_t bits = 0U;
        for (uint32_t index = 0U; index < sizeof(double); ++index) {
            bits |= static_cast<uint64_t>(value.bytes[index]) << (index * 8U);
        }
        return std::bit_cast<double>(bits);
    }

    ScriptState()
    {
        auto const compiled = core::lang::compile(
            {.id = "s6_height_tile", .text = detail::S6_HEIGHT_TILE_SCRIPT},
            {}
        );
        if (!compiled) {
            throw std::runtime_error{"CoreLang terrain script failed to compile"};
        }
        auto loaded = runtime.load(compiled->bytes);
        if (!loaded) {
            throw std::runtime_error{"CoreLang terrain script failed to load"};
        }
        program = std::move(*loaded);
        base = parameter("base_height");
        spacing = parameter("crest_spacing");
        amplitude = parameter("crest_amplitude");
        if (!std::isfinite(base) || !std::isfinite(spacing) || !std::isfinite(amplitude)
            || base < 0.0 || spacing <= 0.0 || amplitude < 0.0) {
            throw std::runtime_error{"CoreLang terrain parameters are invalid"};
        }
    }
};

TerrainGenerator::TerrainGenerator()
    : m_script(std::make_shared<ScriptState>())
{
}

TerrainGenerator::~TerrainGenerator() = default;
TerrainGenerator::TerrainGenerator(TerrainGenerator&&) noexcept = default;
TerrainGenerator& TerrainGenerator::operator=(TerrainGenerator&&) noexcept = default;

uint16_t TerrainGenerator::heightAt(int64_t const x, int64_t const y) const noexcept
{
    return kernelHeightAt(x, y, m_script->base, m_script->spacing, m_script->amplitude);
}

HeightTile TerrainGenerator::generateHeightTile(HeightTileCoordinate coordinate) const
{
    constexpr int32_t TILE_COUNT = static_cast<int32_t>(WorldExtent::WIDTH / HeightTile::SIDE_LENGTH);
    coordinate.x = static_cast<int32_t>((coordinate.x % TILE_COUNT + TILE_COUNT) % TILE_COUNT);
    coordinate.y = static_cast<int32_t>((coordinate.y % TILE_COUNT + TILE_COUNT) % TILE_COUNT);
    HeightTile tile{.coordinate = coordinate};
    for (uint8_t y = 0U; y < HeightTile::SIDE_LENGTH; ++y) {
        for (uint8_t x = 0U; x < HeightTile::SIDE_LENGTH; ++x) {
            int64_t const world_x = static_cast<int64_t>(coordinate.x) * HeightTile::SIDE_LENGTH + x;
            int64_t const world_y = static_cast<int64_t>(coordinate.y) * HeightTile::SIDE_LENGTH + y;
            tile.heights[static_cast<uint32_t>(y) * HeightTile::SIDE_LENGTH + x] = heightAt(world_x, world_y);
        }
    }
    return tile;
}

Chunk TerrainGenerator::generateChunk(ChunkCoordinate const coordinate) const
{
    if (!WorldBounds::isValidChunk(coordinate)) {
        throw std::invalid_argument{"chunk coordinate is outside the sparse world"};
    }
    HeightTile const tile = generateHeightTile({.x = coordinate.x, .y = coordinate.y});
    Chunk::Blocks blocks;
    blocks.fill(Block::Air);
    for (uint8_t z = 0U; z < Chunk::SIDE_LENGTH; ++z) {
        uint32_t const world_z = static_cast<uint32_t>(coordinate.z) * Chunk::SIDE_LENGTH + z;
        for (uint8_t y = 0U; y < Chunk::SIDE_LENGTH; ++y) {
            for (uint8_t x = 0U; x < Chunk::SIDE_LENGTH; ++x) {
                uint16_t const height = tile.heights[static_cast<uint32_t>(y) * Chunk::SIDE_LENGTH + x];
                if (world_z < height) {
                    blocks[static_cast<uint32_t>(z) * Chunk::FACE_BLOCK_COUNT
                        + static_cast<uint32_t>(y) * Chunk::SIDE_LENGTH + x] = Block::Stone;
                }
            }
        }
    }
    return Chunk{coordinate, std::move(blocks)};
}

bool TerrainGenerator::usingCoreLang() const noexcept
{
    return m_script && m_script->program.has_value();
}

} // namespace shared
