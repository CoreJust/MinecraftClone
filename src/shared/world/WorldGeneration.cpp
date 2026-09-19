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
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t HEIGHT_TILE_INDEX_MULTIPLIER = shared::HeightTile::SIDE_LENGTH;
constexpr double WORLD_CENTER = 32'768.0;
constexpr double CREST_SPACING = 64.0;
constexpr double BASE_HEIGHT = 6.0;
constexpr double CREST_AMPLITUDE = 800.0;
constexpr uint16_t MAXIMUM_HEIGHT = static_cast<uint16_t>(shared::WorldExtent::DEPTH);

[[nodiscard]]
uint32_t sampleIndex(shared::BlockCoordinate const coordinate) noexcept
{
    return static_cast<uint32_t>(coordinate.y) * HEIGHT_TILE_INDEX_MULTIPLIER + coordinate.x;
}

[[nodiscard]]
core::lang::Type floatType()
{
    return {.kind = core::lang::TypeKind::F64};
}

[[nodiscard]]
core::lang::Value floatValue(double const value)
{
    uint64_t const bits = std::bit_cast<uint64_t>(value);
    std::vector<uint8_t> bytes(sizeof(bits));
    for (uint64_t index = 0U; index < bytes.size(); ++index) {
        bytes[static_cast<std::vector<uint8_t>::size_type>(index)] = static_cast<uint8_t>(bits >> (index * 8U));
    }
    return {.type = floatType(), .bytes = std::move(bytes)};
}

[[nodiscard]]
std::optional<int32_t> signedValue(core::lang::Value const& value)
{
    if (value.type.kind != core::lang::TypeKind::I32 || value.bytes.size() != sizeof(uint32_t)) {
        return std::nullopt;
    }
    uint32_t bits = 0U;
    for (uint64_t index = 0U; index < value.bytes.size(); ++index) {
        bits |= static_cast<uint32_t>(value.bytes[static_cast<std::vector<uint8_t>::size_type>(index)])
            << (index * 8U);
    }
    return static_cast<int32_t>(bits);
}

[[nodiscard]]
uint16_t clampHeight(int32_t const height) noexcept
{
    return static_cast<uint16_t>(std::clamp(height, 0, static_cast<int32_t>(MAXIMUM_HEIGHT)));
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

    ScriptState()
    {
        auto const compiled = core::lang::compile(
            {.id = "s6_height_tile", .text = detail::S6_HEIGHT_TILE_SCRIPT},
            {}
        );
        if (!compiled) {
            return;
        }
        auto loaded = runtime.load(compiled->bytes);
        if (loaded) {
            program = std::move(*loaded);
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

uint16_t TerrainGenerator::nativeHeightAt(int64_t const x, int64_t const y) noexcept
{
    double const dx = static_cast<double>(x) - WORLD_CENTER;
    double const dy = static_cast<double>(y) - WORLD_CENTER;
    double const distance = std::sqrt(dx * dx + dy * dy);
    double const n = std::floor(distance / CREST_SPACING + 0.5);
    double const angle = std::numbers::pi_v<double> * distance / CREST_SPACING;
    double const height = std::floor(BASE_HEIGHT
        + CREST_AMPLITUDE / (n + 1.0) * std::cos(angle) * std::cos(angle));
    return clampHeight(static_cast<int32_t>(height));
}

uint16_t TerrainGenerator::evaluateHeight(int64_t const x, int64_t const y) const noexcept
{
    if (!m_script || !m_script->program) {
        return nativeHeightAt(x, y);
    }
    try {
        std::array<core::lang::Type, 1U> const parameter_types{floatType()};
        double const dx = static_cast<double>(x) - WORLD_CENTER;
        double const dy = static_cast<double>(y) - WORLD_CENTER;
        double const distance = std::sqrt(dx * dx + dy * dy);
        std::array<core::lang::Value, 1U> const arguments{floatValue(distance)};
        auto const outcome = m_script->runtime.execute(
            *m_script->program,
            "s6_height_tile",
            "height",
            parameter_types,
            arguments
        );
        if (!outcome || !std::holds_alternative<core::lang::Completed>(*outcome)) {
            return nativeHeightAt(x, y);
        }
        auto const value = signedValue(std::get<core::lang::Completed>(*outcome).value);
        return value ? clampHeight(*value) : nativeHeightAt(x, y);
    } catch (...) {
        return nativeHeightAt(x, y);
    }
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
            tile.heights[static_cast<uint32_t>(y) * HeightTile::SIDE_LENGTH + x] = evaluateHeight(world_x, world_y);
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
