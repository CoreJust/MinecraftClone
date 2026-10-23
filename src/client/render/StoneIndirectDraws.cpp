#include <client/render/StoneIndirectDraws.hpp>

#include <shared/world/Chunk.hpp>
#include <shared/world/ChunkMesher.hpp>
#include <shared/world/HeightTileSurfaceMesher.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

namespace client {

StoneFaceInstance StoneFaceInstance::fromTerrainTriangle(
    std::array<std::array<int32_t, 3>, 3> const& vertices, shared::Block const material)
{
    if (!shared::isSolidBlock(material)) {
        throw std::invalid_argument("terrain triangle contains an invalid material");
    }
    auto const offset = [&vertices](uint32_t const vertex) {
        int64_t const dx = static_cast<int64_t>(vertices[vertex][0]) - vertices[0][0];
        int64_t const dy = static_cast<int64_t>(vertices[vertex][1]) - vertices[0][1];
        if (dx < -32'768 || dx > 32'767 || dy < -32'768 || dy > 32'767) {
            throw std::invalid_argument("terrain triangle offset exceeds signed 16-bit range");
        }
        return static_cast<uint32_t>(static_cast<uint16_t>(dx))
            | (static_cast<uint32_t>(static_cast<uint16_t>(dy)) << 16U);
    };
    for (auto const& vertex : vertices) {
        if (vertex[2] < 0 || vertex[2] > 65'535) {
            throw std::invalid_argument("terrain triangle height exceeds unsigned 16-bit range");
        }
    }
    uint32_t const heights = static_cast<uint32_t>(vertices[0][2]) | (static_cast<uint32_t>(vertices[1][2]) << 16U);
    return {
        .x = vertices[0][0],
        .y = vertices[0][1],
        .z = std::bit_cast<int32_t>(heights),
        .direction = TERRAIN_TRIANGLE,
        .u_extent = offset(1U),
        .v_extent = offset(2U),
        .material_id = static_cast<uint32_t>(material),
        .reserved_1 = static_cast<uint32_t>(vertices[2][2]),
    };
}

StoneFaceInstance StoneFaceInstance::fromTerrainSurfaceTriangle(
    std::array<std::array<int32_t, 3>, 3> const& vertices,
    std::array<int32_t, 3> const& temperatures
)
{
    for (int32_t const temperature : temperatures) {
        if (temperature < -16'384 || temperature > 16'383) {
            throw std::invalid_argument("terrain temperature exceeds signed 15-bit range");
        }
    }
    StoneFaceInstance face = fromTerrainTriangle(vertices, shared::Block::Grass);
    face.reserved_1 |= 0x8000'0000U | ((static_cast<uint32_t>(temperatures[0]) & 0x7fffU) << 16U);
    face.material_id = static_cast<uint32_t>(static_cast<uint16_t>(temperatures[1]))
        | (static_cast<uint32_t>(static_cast<uint16_t>(temperatures[2])) << 16U);
    return face;
}

namespace {

constexpr uint32_t STONE_FACE_VERTEX_COUNT = 6U;
constexpr uint32_t STONE_INDEX_COUNT_PER_QUAD = 6U;
constexpr uint32_t STONE_VERTEX_COUNT_PER_QUAD = 4U;

[[nodiscard]]
bool isValidHeightTileSurfaceDirection(uint32_t const direction) noexcept
{
    return direction <= static_cast<uint32_t>(shared::HeightTileSurfaceDirection::PositiveY)
        || direction == static_cast<uint32_t>(shared::HeightTileSurfaceDirection::PositiveZ);
}

[[nodiscard]] int32_t checkedVertexOffset(uint32_t const first_quad)
{
    constexpr uint32_t MAXIMUM_FIRST_QUAD = static_cast<uint32_t>(std::numeric_limits<int32_t>::max())
        / STONE_VERTEX_COUNT_PER_QUAD;
    if (first_quad > MAXIMUM_FIRST_QUAD) {
        throw std::invalid_argument("stone indexed block vertex offset exceeds the signed range");
    }
    return static_cast<int32_t>(first_quad * STONE_VERTEX_COUNT_PER_QUAD);
}

void validateIndexedBlockRange(StoneIndirectDraws::Range const range)
{
    constexpr uint64_t MAXIMUM_QUAD_END_EXCLUSIVE =
        (static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) + 1U) / STONE_VERTEX_COUNT_PER_QUAD;
    uint64_t const end_quad = static_cast<uint64_t>(range.first_instance) + range.instance_count;
    if (end_quad > MAXIMUM_QUAD_END_EXCLUSIVE) {
        throw std::invalid_argument("stone indexed block vertex range exceeds the signed range");
    }
    static_cast<void>(checkedVertexOffset(range.first_instance));
}

[[nodiscard]] uint32_t indexedBlockCommandCount(StoneIndirectDraws::Range const range)
{
    validateIndexedBlockRange(range);
    return range.instance_count == 0U
        ? 1U
        : 1U + (range.instance_count - 1U) / StoneIndirectDraws::QUADS_PER_INDEX_BLOCK;
}

[[nodiscard]] uint32_t packedCommandCount(
    std::span<StoneIndirectDraws::Range const> ranges,
    StoneIndirectDraws::QuadMode const quad_mode,
    uint32_t const maximum_command_count
)
{
    if (quad_mode != StoneIndirectDraws::QuadMode::IndexedBlock) {
        if (ranges.size() > maximum_command_count) {
            throw std::invalid_argument("visible stone ranges exceed the indirect command limit");
        }
        return static_cast<uint32_t>(ranges.size());
    }

    uint32_t command_count = 0U;
    for (StoneIndirectDraws::Range const range : ranges) {
        uint32_t const range_command_count = indexedBlockCommandCount(range);
        if (range_command_count > maximum_command_count - command_count) {
            throw std::invalid_argument("visible stone ranges exceed the indirect command limit");
        }
        command_count += range_command_count;
    }
    return command_count;
}

void appendIndexedBlockCommands(
    std::span<VkDrawIndexedIndirectCommand> commands,
    std::span<StoneIndirectDraws::Range const> ranges,
    uint32_t& command_index
)
{
    for (StoneIndirectDraws::Range const range : ranges) {
        uint32_t remaining_quads = range.instance_count;
        uint32_t first_quad = range.first_instance;
        if (remaining_quads == 0U) {
            commands[command_index++] = StoneIndirectDraws::indexedBlockCommand({ first_quad, 0U });
            continue;
        }
        while (remaining_quads != 0U) {
            uint32_t const quad_count = std::min(remaining_quads, StoneIndirectDraws::QUADS_PER_INDEX_BLOCK);
            commands[command_index++] = StoneIndirectDraws::indexedBlockCommand({ first_quad, quad_count });
            first_quad += quad_count;
            remaining_quads -= quad_count;
        }
    }
}

[[nodiscard]] bool isRecoverableAllocationResult(VkResult const result) noexcept
{
    return result == VK_ERROR_OUT_OF_HOST_MEMORY
        || result == VK_ERROR_OUT_OF_DEVICE_MEMORY
        || result == VK_ERROR_MEMORY_MAP_FAILED;
}

} // namespace

StoneFaceInstance StoneFaceInstance::fromChunkFace(
    shared::ChunkCoordinate const coordinate,
    shared::MeshFace const& face
)
{
    uint32_t const direction = static_cast<uint32_t>(face.direction);
    if (!shared::isSolidBlock(face.material) || direction >= 6U || !shared::Chunk::isValid(face.local_origin)) {
        throw std::invalid_argument("chunk mesh contains an invalid material face");
    }
    return {
        .x = coordinate.x * static_cast<int32_t>(shared::Chunk::SIDE_LENGTH) + face.local_origin.x,
        .y = coordinate.y * static_cast<int32_t>(shared::Chunk::SIDE_LENGTH) + face.local_origin.y,
        .z = coordinate.z * static_cast<int32_t>(shared::Chunk::SIDE_LENGTH) + face.local_origin.z,
        .direction = direction,
        .material_id = static_cast<uint32_t>(static_cast<uint8_t>(face.material)),
    };
}

StoneFaceInstance StoneFaceInstance::fromHeightTileQuad(shared::HeightTileSurfaceQuad const& quad)
{
    uint32_t const direction = static_cast<uint32_t>(quad.direction);
    if (!shared::isSolidBlock(quad.material) || !isValidHeightTileSurfaceDirection(direction)
        || quad.u_extent == 0U || quad.v_extent == 0U) {
        throw std::invalid_argument("height tile mesh contains an invalid material quad");
    }
    return {
        .x = quad.x,
        .y = quad.y,
        .z = quad.z,
        .direction = direction,
        .u_extent = quad.u_extent,
        .v_extent = quad.v_extent,
        .material_id = static_cast<uint32_t>(static_cast<uint8_t>(quad.material)),
    };
}

StoneIndirectDraws::AllocationError::AllocationError(VkResult const result, char const* const operation)
    : std::runtime_error(std::string{ operation } + " failed with Vulkan result " + std::to_string(result))
    , m_result(result)
{}

std::array<uint16_t, StoneIndirectDraws::INDEX_COUNT_PER_BLOCK> const& StoneIndirectDraws::quadIndexBlock() noexcept
{
    static std::array<uint16_t, INDEX_COUNT_PER_BLOCK> indices{};
    static bool const initialized = []() noexcept {
        for (uint32_t quad_index = 0U; quad_index < QUADS_PER_INDEX_BLOCK; ++quad_index) {
            uint32_t const index_offset = quad_index * STONE_INDEX_COUNT_PER_QUAD;
            uint16_t const vertex_offset = static_cast<uint16_t>(quad_index * STONE_VERTEX_COUNT_PER_QUAD);
            indices[index_offset] = vertex_offset;
            indices[index_offset + 1U] = static_cast<uint16_t>(vertex_offset + 1U);
            indices[index_offset + 2U] = static_cast<uint16_t>(vertex_offset + 2U);
            indices[index_offset + 3U] = vertex_offset;
            indices[index_offset + 4U] = static_cast<uint16_t>(vertex_offset + 2U);
            indices[index_offset + 5U] = static_cast<uint16_t>(vertex_offset + 3U);
        }
        return true;
    }();
    static_cast<void>(initialized);
    return indices;
}

VkDrawIndexedIndirectCommand StoneIndirectDraws::indexedBlockCommand(Range const range)
{
    if (range.instance_count > QUADS_PER_INDEX_BLOCK) {
        throw std::invalid_argument("stone indexed block command exceeds one index block");
    }
    validateIndexedBlockRange(range);
    return {
        .indexCount = range.instance_count * STONE_INDEX_COUNT_PER_QUAD,
        .instanceCount = 1U,
        .firstIndex = 0U,
        .vertexOffset = checkedVertexOffset(range.first_instance),
        .firstInstance = 0U,
    };
}

std::span<VkDrawIndexedIndirectCommand> StoneIndirectDraws::Buffer::mappedIndexedCommands()
{
    throw std::logic_error("stone indirect buffer does not support indexed commands");
}

void StoneIndirectDraws::Prepared::record(
    VkCommandBuffer const command,
    uint32_t first_command,
    uint32_t command_count
) const {
    if (buffer == nullptr || maximum_draw_count == 0U || first_command > logical_draw_count
        || command_count > logical_draw_count - first_command) {
        throw std::invalid_argument("stone indirect draw range is invalid");
    }
    while (command_count != 0U) {
        uint32_t const batch_count = std::min(command_count, maximum_draw_count);
        buffer->record(
            command,
            static_cast<VkDeviceSize>(first_command)
                * (indexed ? sizeof(VkDrawIndexedIndirectCommand) : sizeof(VkDrawIndirectCommand)),
            batch_count
        );
        first_command += batch_count;
        command_count -= batch_count;
    }
}

StoneIndirectDraws::StoneIndirectDraws(
    uint32_t const maximum_command_count,
    MaximumDrawCount const maximum_draw_count,
    bool const multi_draw_indirect_enabled,
    bool const draw_indirect_first_instance_enabled,
    Factory factory,
    QuadMode const quad_mode
)
    : m_maximum_command_count(maximum_command_count)
    , m_maximum_draw_count(multi_draw_indirect_enabled && draw_indirect_first_instance_enabled
        ? maximum_draw_count.value : 0U)
    , m_factory(std::move(factory))
    , m_quad_mode(quad_mode)
{
    if (m_maximum_command_count == 0U || !m_factory) {
        throw std::invalid_argument("stone indirect configuration is invalid");
    }
}

StoneIndirectDraws::Prepared StoneIndirectDraws::prepareAcquiredSlot(
    uint32_t const slot_index,
    std::span<Range const> const draw_ranges,
    std::span<Range const> const solid_draw_ranges
)
{
    uint32_t const textured_count = packedCommandCount(draw_ranges, m_quad_mode, m_maximum_command_count);
    uint32_t const solid_count = packedCommandCount(
        solid_draw_ranges,
        m_quad_mode,
        m_maximum_command_count - textured_count
    );
    if (solid_count > m_maximum_command_count - textured_count) {
        throw std::invalid_argument("visible stone ranges exceed the indirect command limit");
    }
    uint32_t const command_count = textured_count + solid_count;
    Prepared prepared{
        .logical_draw_count = command_count,
        .textured_command_count = textured_count,
        .maximum_draw_count = m_maximum_draw_count,
        .indexed = m_quad_mode != QuadMode::SixVertex,
    };
    if (m_maximum_draw_count == 0U || command_count == 0U) {
        return prepared;
    }
    auto entry = m_buffers.find(slot_index);
    if (entry == m_buffers.end() || entry->second->capacity() < command_count) {
        uint32_t capacity = 1U;
        while (capacity < command_count) {
            capacity = capacity > m_maximum_command_count / 2U ? m_maximum_command_count : capacity * 2U;
        }
        try {
            std::unique_ptr<Buffer> replacement = m_factory(capacity);
            if (!replacement || replacement->capacity() < command_count) {
                throw std::logic_error("stone indirect factory returned insufficient capacity");
            }
            if (entry == m_buffers.end()) {
                entry = m_buffers.emplace(slot_index, std::move(replacement)).first;
            } else {
                entry->second = std::move(replacement);
            }
        } catch (AllocationError const& error) {
            if (!isRecoverableAllocationResult(error.result())) {
                throw;
            }
            return prepared;
        } catch (std::bad_alloc const&) {
            return prepared;
        }
    }
    if (m_quad_mode != QuadMode::SixVertex) {
        std::span<VkDrawIndexedIndirectCommand> const commands = entry->second->mappedIndexedCommands();
        if (commands.size() < command_count) {
            throw std::logic_error("stone indexed indirect buffer has insufficient mapped capacity");
        }
        uint32_t command_index = 0U;
        if (m_quad_mode == QuadMode::IndexedBlock) {
            appendIndexedBlockCommands(commands, draw_ranges, command_index);
            appendIndexedBlockCommands(commands, solid_draw_ranges, command_index);
            prepared.buffer = entry->second.get();
            return prepared;
        }
        for (std::span<Range const> const ranges : { draw_ranges, solid_draw_ranges }) {
            for (Range const range : ranges) {
                commands[command_index++] = {
                    .indexCount = STONE_FACE_VERTEX_COUNT,
                    .instanceCount = range.instance_count,
                    .firstIndex = 0U,
                    .vertexOffset = 0,
                    .firstInstance = range.first_instance,
                };
            }
        }
        prepared.buffer = entry->second.get();
        return prepared;
    }
    std::span<VkDrawIndirectCommand> const commands = entry->second->mappedCommands();
    if (commands.size() < command_count) {
        throw std::logic_error("stone indirect buffer has insufficient mapped capacity");
    }
    uint32_t command_index = 0U;
    for (std::span<Range const> const ranges : { draw_ranges, solid_draw_ranges }) {
        for (Range const range : ranges) {
            commands[command_index++] = {
                .vertexCount = STONE_FACE_VERTEX_COUNT,
                .instanceCount = range.instance_count,
                .firstVertex = 0U,
                .firstInstance = range.first_instance,
            };
        }
    }
    prepared.buffer = entry->second.get();
    return prepared;
}

void StoneIndirectDraws::clear() noexcept
{
    m_buffers.clear();
}

} // namespace client
