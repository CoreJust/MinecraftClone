#pragma once

#include <vulkan/vulkan_core.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <unordered_map>

namespace shared {

enum class Block : uint8_t;
struct ChunkCoordinate;
struct HeightTileSurfaceQuad;
struct MeshFace;

} // namespace shared

namespace client {

// std430 FaceInstance packs material_id into extents.z (byte offset 24).
// The direct, indexed-instanced, and indexed-block vertex paths share this layout.
struct alignas(16) StoneFaceInstance final {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    uint32_t direction = 0U;
    uint32_t u_extent = 1U;
    uint32_t v_extent = 1U;
    uint32_t material_id = 1U;
    uint32_t reserved_1 = 0U;
    // Three upper-hemisphere octahedral snorm16x2 vertex normals and a validity word.
    // Voxel faces and vertical/downward terrain skirts keep this payload zero.
    std::array<uint32_t, 4> terrain_normals{};

    static constexpr uint32_t TERRAIN_TRIANGLE = 6U;
    static constexpr uint32_t TERRAIN_HEIGHT_SCALE = 32U;
    static constexpr uint32_t TERRAIN_TRIANGLE_XY_Q5 = 1U << 6U;
    // reserved_1 uses this bit for side quads only; triangles encode heights there.
    static constexpr uint32_t VOXEL_GRASS_BANK = 1U;

    // The low three direction bits identify the face; bits 3..5 carry each triangle Z bit 16.
    [[nodiscard]] constexpr uint32_t faceDirection() const noexcept { return direction & 7U; }

    // Triangle Z values are fixed point at 1/32 block, including the 2048-block world top.
    [[nodiscard]] constexpr std::array<uint32_t, 3> terrainTriangleHeights() const noexcept
    {
        return {
            (static_cast<uint32_t>(z) & 0xffffU) | (((direction >> 3U) & 1U) << 16U),
            (static_cast<uint32_t>(z) >> 16U) | (((direction >> 4U) & 1U) << 16U),
            (reserved_1 & 0xffffU) | (((direction >> 5U) & 1U) << 16U),
        };
    }

    // XY coordinates are blocks; Z coordinates are unsigned fixed point at 1/32 block.
    [[nodiscard]]
    static StoneFaceInstance fromTerrainTriangle(
        std::array<std::array<int32_t, 3>, 3> const& vertices, shared::Block material);

    // Temperatures use signed fixed point at 1/32 degree Celsius.
    [[nodiscard]]
    static StoneFaceInstance fromTerrainSurfaceTriangle(
        std::array<std::array<int32_t, 3>, 3> const& vertices,
        std::array<int32_t, 3> const& temperatures
    );

    // Three categorical surface materials use the original triangle material fields.
    [[nodiscard]]
    static StoneFaceInstance fromTerrainMaterialTriangle(
        std::array<std::array<int32_t, 3>, 3> const& vertices,
        std::array<shared::Block, 3> const& materials
    );

    // Metric XYZ is rounded to 1/32 block. XY origin is signed 32-bit Q5 and
    // offsets are signed 16-bit Q5; Z remains in the 0..65536 Q5 domain.
    // Non-finite or unrepresentable coordinates and invalid materials are rejected.
    [[nodiscard]]
    static StoneFaceInstance fromMetricTerrainMaterialTriangle(
        std::array<std::array<double, 3>, 3> const& vertices,
        std::array<shared::Block, 3> const& materials
    );

    constexpr bool operator==(StoneFaceInstance const&) const noexcept = default;

    [[nodiscard]]
    static StoneFaceInstance fromChunkFace(shared::ChunkCoordinate coordinate, shared::MeshFace const& face);

    [[nodiscard]]
    static StoneFaceInstance fromHeightTileQuad(shared::HeightTileSurfaceQuad const& quad);
};

static_assert(sizeof(StoneFaceInstance) == 48U);
static_assert(offsetof(StoneFaceInstance, material_id) == 24U);
static_assert(offsetof(StoneFaceInstance, terrain_normals) == 32U);

class StoneIndirectDraws final {
public:
    static constexpr uint32_t QUADS_PER_INDEX_BLOCK = 4096U;
    static constexpr uint32_t INDEX_COUNT_PER_BLOCK = QUADS_PER_INDEX_BLOCK * 6U;
    static constexpr std::array<uint16_t, 6> QUAD_INDICES{ 0U, 1U, 2U, 0U, 2U, 3U };

    enum class QuadMode : uint8_t {
        SixVertex = 0U,
        IndexedInstanced = 1U,
        IndexedBlock = 2U,
    };

    class Buffer;
    using Factory = std::function<std::unique_ptr<Buffer>(uint32_t)>;

    struct Range final {
        uint32_t first_instance = 0U;
        uint32_t instance_count = 0U;
    };

    [[nodiscard]]
    static std::array<uint16_t, INDEX_COUNT_PER_BLOCK> const& quadIndexBlock() noexcept;

    [[nodiscard]]
    static VkDrawIndexedIndirectCommand indexedBlockCommand(Range range);

    struct MaximumDrawCount final {
        constexpr MaximumDrawCount(uint32_t const value) noexcept
            : value(value)
        {
        }

        uint32_t value;
    };

    class AllocationError final : public std::runtime_error {
    public:
        AllocationError(VkResult result, char const* operation);

        [[nodiscard]]
        VkResult result() const noexcept { return m_result; }

    private:
        VkResult m_result;
    };

    class Buffer {
    public:
        virtual ~Buffer() = default;

        [[nodiscard]]
        virtual uint32_t capacity() const noexcept = 0;
        [[nodiscard]]
        virtual std::span<VkDrawIndirectCommand> mappedCommands() = 0;
        [[nodiscard]]
        virtual std::span<VkDrawIndexedIndirectCommand> mappedIndexedCommands();
        virtual void record(VkCommandBuffer command, VkDeviceSize offset, uint32_t draw_count) const = 0;
    };

    struct Prepared final {
        Buffer const* buffer = nullptr;
        uint32_t logical_draw_count = 0U;
        uint32_t textured_command_count = 0U;
        uint32_t maximum_draw_count = 0U;
        bool indexed = false;

        void record(VkCommandBuffer command, uint32_t first_command, uint32_t command_count) const;
    };

    StoneIndirectDraws(
        uint32_t maximum_command_count,
        MaximumDrawCount maximum_draw_count,
        bool multi_draw_indirect_enabled,
        bool draw_indirect_first_instance_enabled,
        Factory factory,
        QuadMode quad_mode = QuadMode::SixVertex
    );

    // The caller has acquired this slot after its submission fence completed.
    [[nodiscard]]
    Prepared prepareAcquiredSlot(
        uint32_t slot_index,
        std::span<Range const> draw_ranges,
        std::span<Range const> solid_draw_ranges
    );
    // Submitted slots must be drained before their buffers are released.
    void clear() noexcept;

private:
    uint32_t m_maximum_command_count;
    uint32_t m_maximum_draw_count;
    Factory m_factory;
    QuadMode m_quad_mode = QuadMode::SixVertex;
    std::unordered_map<uint32_t, std::unique_ptr<Buffer>> m_buffers;
};

} // namespace client
