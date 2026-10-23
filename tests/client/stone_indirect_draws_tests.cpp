#include <client/render/StoneIndirectDraws.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/ChunkMesher.hpp>
#include <shared/world/HeightTileSurfaceMesher.hpp>

#include <gtest/gtest.h>
#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Draws = client::StoneIndirectDraws;

struct Batch final {
    VkDeviceSize offset;
    uint32_t draw_count;
};

struct BufferState final {
    std::vector<VkDrawIndirectCommand> commands;
    std::vector<VkDrawIndexedIndirectCommand> indexed_commands;
    std::vector<Batch> batches;
    bool submitted = false;
    bool destroyed = false;
    bool touched_while_submitted = false;
};

class TestBuffer final : public Draws::Buffer {
public:
    explicit TestBuffer(std::shared_ptr<BufferState> state)
        : m_state(std::move(state))
    {}

    ~TestBuffer() override
    {
        m_state->destroyed = true;
        m_state->touched_while_submitted = m_state->touched_while_submitted || m_state->submitted;
    }

    [[nodiscard]] uint32_t capacity() const noexcept override
    {
        return static_cast<uint32_t>(std::max(m_state->commands.size(), m_state->indexed_commands.size()));
    }

    [[nodiscard]] std::span<VkDrawIndirectCommand> mappedCommands() override
    {
        m_state->touched_while_submitted = m_state->touched_while_submitted || m_state->submitted;
        return m_state->commands;
    }

    [[nodiscard]] std::span<VkDrawIndexedIndirectCommand> mappedIndexedCommands() override
    {
        m_state->touched_while_submitted = m_state->touched_while_submitted || m_state->submitted;
        return m_state->indexed_commands;
    }

    void record(VkCommandBuffer, VkDeviceSize const offset, uint32_t const draw_count) const override
    {
        m_state->batches.push_back({ offset, draw_count });
    }

private:
    std::shared_ptr<BufferState> m_state;
};

struct FactoryProbe final {
    std::vector<std::shared_ptr<BufferState>> created;
    std::optional<VkResult> failure;
    uint32_t attempts = 0U;
    bool fail_cpp_allocation = false;
    bool indexed = false;

    [[nodiscard]] std::unique_ptr<Draws::Buffer> create(uint32_t const capacity)
    {
        ++attempts;
        auto state = std::make_shared<BufferState>();
        if (indexed) {
            state->indexed_commands.resize(capacity);
        } else {
            state->commands.resize(capacity);
        }
        created.push_back(state);
        auto buffer = std::make_unique<TestBuffer>(state);
        if (fail_cpp_allocation) {
            throw std::bad_alloc{};
        }
        if (failure.has_value()) {
            throw Draws::AllocationError(*failure, "injected stone allocation");
        }
        return buffer;
    }
};

[[nodiscard]] Draws makeDraws(
    FactoryProbe& probe,
    uint32_t const maximum_command_count,
    uint32_t const maximum_draw_count = 4U,
    bool const multi_draw_indirect_enabled = true,
    bool const draw_indirect_first_instance_enabled = true,
    Draws::QuadMode const quad_mode = Draws::QuadMode::SixVertex
) {
    return {
        maximum_command_count,
        maximum_draw_count,
        multi_draw_indirect_enabled,
        draw_indirect_first_instance_enabled,
        [&probe](uint32_t const capacity) { return probe.create(capacity); },
        quad_mode,
    };
}

TEST(StoneIndirectDrawsTest, RequiresBothFeaturesWithoutAllocatingAndPreservesLogicalCounts)
{
    static constexpr uint32_t MAXIMUM_COMMAND_COUNT = 16U;
    static constexpr uint32_t MAXIMUM_DRAW_COUNT = 4U;
    static constexpr std::array<std::array<bool, 2>, 3> DISABLED_FEATURES{
        std::array<bool, 2>{ false, false },
        std::array<bool, 2>{ true, false },
        std::array<bool, 2>{ false, true },
    };
    std::array const ranges{ Draws::Range{ 10U, 3U }, Draws::Range{ 20U, 0U } };
    for (std::array<bool, 2> const features : DISABLED_FEATURES) {
        FactoryProbe probe;
        Draws draws = makeDraws(probe, MAXIMUM_COMMAND_COUNT, MAXIMUM_DRAW_COUNT, features[0], features[1]);
        Draws::Prepared const prepared = draws.prepareAcquiredSlot(7U, ranges, {});
        EXPECT_EQ(prepared.buffer, nullptr);
        EXPECT_EQ(prepared.logical_draw_count, ranges.size());
        EXPECT_EQ(probe.attempts, 0U);
    }
}

TEST(StoneIndirectDrawsTest, ZeroDeviceLimitAndEmptyRangesUseDirectFallbackWithoutAllocating)
{
    static constexpr uint32_t MAXIMUM_COMMAND_COUNT = 16U;
    std::array const ranges{ Draws::Range{ 10U, 3U } };
    FactoryProbe probe;
    Draws disabled = makeDraws(probe, MAXIMUM_COMMAND_COUNT, 0U);
    EXPECT_EQ(disabled.prepareAcquiredSlot(19U, ranges, {}).buffer, nullptr);
    EXPECT_EQ(disabled.prepareAcquiredSlot(19U, ranges, {}).logical_draw_count, 1U);
    Draws enabled = makeDraws(probe, MAXIMUM_COMMAND_COUNT);
    Draws::Prepared const empty = enabled.prepareAcquiredSlot(19U, {}, {});
    EXPECT_EQ(empty.buffer, nullptr);
    EXPECT_EQ(empty.logical_draw_count, 0U);
    EXPECT_EQ(probe.attempts, 0U);
}

TEST(StoneIndirectDrawsTest, PacksExactDrawCommandsAndBoundsBatchesForBothPipelineRanges)
{
    static constexpr uint32_t MAXIMUM_COMMAND_COUNT = 16U;
    static constexpr uint32_t MAXIMUM_DRAW_COUNT = 2U;
    static constexpr uint32_t VERTEX_COUNT = 6U;
    std::array const textured{ Draws::Range{ 10U, 3U }, Draws::Range{ 30U, 0U }, Draws::Range{ 50U, 7U } };
    std::array const solid{ Draws::Range{ 100U, 11U }, Draws::Range{ 200U, 13U } };
    FactoryProbe probe;
    Draws draws = makeDraws(probe, MAXIMUM_COMMAND_COUNT, MAXIMUM_DRAW_COUNT);
    Draws::Prepared const prepared = draws.prepareAcquiredSlot(91U, textured, solid);
    ASSERT_NE(prepared.buffer, nullptr);
    ASSERT_EQ(probe.created.size(), 1U);
    EXPECT_EQ(prepared.logical_draw_count, 5U);
    auto const& commands = probe.created[0]->commands;
    std::array const expected{ textured[0], textured[1], textured[2], solid[0], solid[1] };
    for (uint32_t index = 0U; index < expected.size(); ++index) {
        EXPECT_EQ(commands[index].vertexCount, VERTEX_COUNT);
        EXPECT_EQ(commands[index].instanceCount, expected[index].instance_count);
        EXPECT_EQ(commands[index].firstVertex, 0U);
        EXPECT_EQ(commands[index].firstInstance, expected[index].first_instance);
    }
    prepared.record(VK_NULL_HANDLE, 0U, 3U);
    prepared.record(VK_NULL_HANDLE, 3U, 2U);
    auto const& batches = probe.created[0]->batches;
    ASSERT_EQ(batches.size(), 3U);
    EXPECT_EQ(batches[0].offset, 0U);
    EXPECT_EQ(batches[0].draw_count, 2U);
    EXPECT_EQ(batches[1].offset, 2U * sizeof(VkDrawIndirectCommand));
    EXPECT_EQ(batches[1].draw_count, 1U);
    EXPECT_EQ(batches[2].offset, 3U * sizeof(VkDrawIndirectCommand));
    EXPECT_EQ(batches[2].draw_count, 2U);
    EXPECT_EQ(prepared.logical_draw_count, 5U);
}

TEST(StoneIndirectDrawsTest, QuadIndicesPreserveBothTriangleVerticesAndWindingForEveryFace)
{
    for (uint32_t direction_index = 0U; direction_index < 6U; ++direction_index) {
        shared::FaceDirection const direction = static_cast<shared::FaceDirection>(direction_index);
        auto const corners = shared::faceVertexOffsets(direction);
        std::array const expected{
            corners[0], corners[1], corners[2], corners[0], corners[2], corners[3],
        };
        for (uint32_t vertex = 0U; vertex < Draws::QUAD_INDICES.size(); ++vertex) {
            ASSERT_LT(Draws::QUAD_INDICES[vertex], corners.size());
            EXPECT_EQ(corners[Draws::QUAD_INDICES[vertex]], expected[vertex]);
        }
    }
}

TEST(StoneIndirectDrawsTest, PacksCanonicalMaterialsForChunkAndHeightTileVertexPaths)
{
    shared::MeshFace const chunk_face{
        .local_origin = { .x = 2U, .y = 3U, .z = 4U },
        .direction = shared::FaceDirection::PositiveZ,
        .material = shared::Block::Grass,
    };
    client::StoneFaceInstance const chunk_instance = client::StoneFaceInstance::fromChunkFace(
        { .x = 1, .y = 2, .z = 3 },
        chunk_face
    );
    EXPECT_EQ(chunk_instance.x, 18);
    EXPECT_EQ(chunk_instance.y, 35);
    EXPECT_EQ(chunk_instance.z, 52);
    EXPECT_EQ(chunk_instance.direction, static_cast<uint32_t>(shared::FaceDirection::PositiveZ));
    EXPECT_EQ(chunk_instance.material_id, static_cast<uint32_t>(shared::Block::Grass));

    shared::HeightTileSurfaceQuad const tile_quad{
        .x = 7,
        .y = 11,
        .z = 5,
        .direction = shared::HeightTileSurfaceDirection::NegativeX,
        .u_extent = 9U,
        .v_extent = 3U,
        .material = shared::Block::Ice,
    };
    client::StoneFaceInstance const tile_instance = client::StoneFaceInstance::fromHeightTileQuad(tile_quad);
    EXPECT_EQ(tile_instance.x, tile_quad.x);
    EXPECT_EQ(tile_instance.y, tile_quad.y);
    EXPECT_EQ(tile_instance.z, tile_quad.z);
    EXPECT_EQ(tile_instance.direction, static_cast<uint32_t>(tile_quad.direction));
    EXPECT_EQ(tile_instance.u_extent, tile_quad.u_extent);
    EXPECT_EQ(tile_instance.v_extent, tile_quad.v_extent);
    EXPECT_EQ(tile_instance.material_id, static_cast<uint32_t>(shared::Block::Ice));
    EXPECT_EQ(offsetof(client::StoneFaceInstance, material_id), 24U);
    EXPECT_EQ(sizeof(client::StoneFaceInstance), 32U);
}

TEST(StoneIndirectDrawsTest, RejectsAirAndUnknownMaterialIdsBeforePacking)
{
    shared::MeshFace air_face{
        .local_origin = { .x = 0U, .y = 0U, .z = 0U },
        .direction = shared::FaceDirection::PositiveZ,
        .material = shared::Block::Air,
    };
    EXPECT_THROW(static_cast<void>(client::StoneFaceInstance::fromChunkFace({}, air_face)), std::invalid_argument);

    shared::HeightTileSurfaceQuad invalid_quad{
        .direction = shared::HeightTileSurfaceDirection::PositiveZ,
        .material = static_cast<shared::Block>(255U),
    };
    EXPECT_THROW(
        static_cast<void>(client::StoneFaceInstance::fromHeightTileQuad(invalid_quad)),
        std::invalid_argument
    );

    invalid_quad.material = shared::Block::Grass;
    invalid_quad.direction = static_cast<shared::HeightTileSurfaceDirection>(4U);
    EXPECT_THROW(
        static_cast<void>(client::StoneFaceInstance::fromHeightTileQuad(invalid_quad)),
        std::invalid_argument
    );
}

TEST(StoneIndirectDrawsTest, GeneratesACompleteSixteenBitQuadIndexBlock)
{
    static constexpr uint32_t QUAD_COUNT = Draws::QUADS_PER_INDEX_BLOCK;
    static constexpr uint32_t INDICES_PER_QUAD = 6U;
    static constexpr uint32_t VERTICES_PER_QUAD = 4U;
    auto const& indices = Draws::quadIndexBlock();
    ASSERT_EQ(indices.size(), Draws::INDEX_COUNT_PER_BLOCK);
    for (uint32_t quad_index = 0U; quad_index < QUAD_COUNT; ++quad_index) {
        uint32_t const index_offset = quad_index * INDICES_PER_QUAD;
        uint16_t const vertex_offset = static_cast<uint16_t>(quad_index * VERTICES_PER_QUAD);
        EXPECT_EQ(indices[index_offset], vertex_offset);
        EXPECT_EQ(indices[index_offset + 1U], static_cast<uint16_t>(vertex_offset + 1U));
        EXPECT_EQ(indices[index_offset + 2U], static_cast<uint16_t>(vertex_offset + 2U));
        EXPECT_EQ(indices[index_offset + 3U], vertex_offset);
        EXPECT_EQ(indices[index_offset + 4U], static_cast<uint16_t>(vertex_offset + 2U));
        EXPECT_EQ(indices[index_offset + 5U], static_cast<uint16_t>(vertex_offset + 3U));
    }
    EXPECT_EQ(indices.back(), 16'383U);
}

TEST(StoneIndirectDrawsTest, IndexedBlockCommandsBoundQuadCountsAndSignedVertexOffsets)
{
    static constexpr uint32_t MAXIMUM_FIRST_QUAD = static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) / 4U;
    static constexpr uint32_t LAST_SIGNED_BLOCK_FIRST_QUAD =
        MAXIMUM_FIRST_QUAD - Draws::QUADS_PER_INDEX_BLOCK + 1U;
    static constexpr Draws::Range ZERO_RANGE{ 17U, 0U };
    static constexpr Draws::Range SINGLE_RANGE{ 5U, 1U };
    static constexpr Draws::Range FULL_RANGE{ 100U, Draws::QUADS_PER_INDEX_BLOCK };
    static constexpr Draws::Range LAST_SIGNED_RANGE{ MAXIMUM_FIRST_QUAD, 1U };
    static constexpr Draws::Range LAST_SIGNED_BLOCK_RANGE{
        LAST_SIGNED_BLOCK_FIRST_QUAD,
        Draws::QUADS_PER_INDEX_BLOCK,
    };
    static constexpr Draws::Range TOO_MANY_QUADS{ 0U, Draws::QUADS_PER_INDEX_BLOCK + 1U };
    static constexpr Draws::Range OVERFLOWING_RANGE{ std::numeric_limits<uint32_t>::max(), 1U };
    static constexpr Draws::Range UNSIGNED_OVERFLOW_RANGE{
        std::numeric_limits<uint32_t>::max() - 1U,
        2U,
    };
    static constexpr Draws::Range SIGNED_OVERFLOW_RANGE{ MAXIMUM_FIRST_QUAD + 1U, 1U };
    static constexpr Draws::Range SIGNED_VERTEX_OVERFLOW_RANGE{ MAXIMUM_FIRST_QUAD, 2U };

    VkDrawIndexedIndirectCommand const zero = Draws::indexedBlockCommand(ZERO_RANGE);
    EXPECT_EQ(zero.indexCount, 0U);
    EXPECT_EQ(zero.instanceCount, 1U);
    EXPECT_EQ(zero.vertexOffset, 68);
    EXPECT_EQ(zero.firstInstance, 0U);

    VkDrawIndexedIndirectCommand const single = Draws::indexedBlockCommand(SINGLE_RANGE);
    EXPECT_EQ(single.indexCount, 6U);
    EXPECT_EQ(single.instanceCount, 1U);
    EXPECT_EQ(single.vertexOffset, 20);

    VkDrawIndexedIndirectCommand const full = Draws::indexedBlockCommand(FULL_RANGE);
    EXPECT_EQ(full.indexCount, Draws::INDEX_COUNT_PER_BLOCK);
    EXPECT_EQ(full.vertexOffset, 400);

    VkDrawIndexedIndirectCommand const last_signed = Draws::indexedBlockCommand(LAST_SIGNED_RANGE);
    EXPECT_EQ(last_signed.vertexOffset, std::numeric_limits<int32_t>::max() - 3);
    VkDrawIndexedIndirectCommand const last_signed_block = Draws::indexedBlockCommand(LAST_SIGNED_BLOCK_RANGE);
    EXPECT_EQ(last_signed_block.indexCount, Draws::INDEX_COUNT_PER_BLOCK);
    EXPECT_EQ(last_signed_block.vertexOffset, static_cast<int32_t>(LAST_SIGNED_BLOCK_FIRST_QUAD * 4U));
    EXPECT_THROW(static_cast<void>(Draws::indexedBlockCommand(TOO_MANY_QUADS)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(Draws::indexedBlockCommand(OVERFLOWING_RANGE)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(Draws::indexedBlockCommand(UNSIGNED_OVERFLOW_RANGE)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(Draws::indexedBlockCommand(SIGNED_OVERFLOW_RANGE)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(Draws::indexedBlockCommand(SIGNED_VERTEX_OVERFLOW_RANGE)), std::invalid_argument);
}

TEST(StoneIndirectDrawsTest, IndexedBlockPackingSplitsTexturedAndSolidRangesAndBatchesCommands)
{
    static constexpr uint32_t MAXIMUM_COMMAND_COUNT = 8U;
    static constexpr uint32_t MAXIMUM_DRAW_COUNT = 1U;
    static constexpr std::array<Draws::Range, 2> TEXTURED_RANGES{
        Draws::Range{ 0U, Draws::QUADS_PER_INDEX_BLOCK + 1U },
        Draws::Range{ 100U, 0U },
    };
    static constexpr std::array<Draws::Range, 1> SOLID_RANGES{
        Draws::Range{ 8192U, Draws::QUADS_PER_INDEX_BLOCK },
    };
    FactoryProbe probe;
    probe.indexed = true;
    Draws draws = makeDraws(
        probe,
        MAXIMUM_COMMAND_COUNT,
        MAXIMUM_DRAW_COUNT,
        true,
        true,
        Draws::QuadMode::IndexedBlock
    );

    Draws::Prepared const prepared = draws.prepareAcquiredSlot(91U, TEXTURED_RANGES, SOLID_RANGES);
    ASSERT_NE(prepared.buffer, nullptr);
    EXPECT_TRUE(prepared.indexed);
    EXPECT_EQ(prepared.textured_command_count, 3U);
    EXPECT_EQ(prepared.logical_draw_count, 4U);
    auto const& commands = probe.created[0]->indexed_commands;
    ASSERT_EQ(commands.size(), 4U);
    EXPECT_EQ(commands[0].indexCount, Draws::INDEX_COUNT_PER_BLOCK);
    EXPECT_EQ(commands[0].instanceCount, 1U);
    EXPECT_EQ(commands[0].firstIndex, 0U);
    EXPECT_EQ(commands[0].vertexOffset, 0);
    EXPECT_EQ(commands[0].firstInstance, 0U);
    EXPECT_EQ(commands[1].indexCount, 6U);
    EXPECT_EQ(commands[1].vertexOffset, static_cast<int32_t>(Draws::QUADS_PER_INDEX_BLOCK * 4U));
    EXPECT_EQ(commands[2].indexCount, 0U);
    EXPECT_EQ(commands[2].vertexOffset, 400);
    EXPECT_EQ(commands[3].indexCount, Draws::INDEX_COUNT_PER_BLOCK);
    EXPECT_EQ(commands[3].vertexOffset, 32'768);

    prepared.record(VK_NULL_HANDLE, 0U, prepared.logical_draw_count);
    auto const& batches = probe.created[0]->batches;
    ASSERT_EQ(batches.size(), prepared.logical_draw_count);
    for (uint32_t command_index = 0U; command_index < prepared.logical_draw_count; ++command_index) {
        EXPECT_EQ(batches[command_index].offset, command_index * sizeof(VkDrawIndexedIndirectCommand));
        EXPECT_EQ(batches[command_index].draw_count, 1U);
    }
}

TEST(StoneIndirectDrawsTest, IndexedBlockFallbackAndCommandLimitsCountPackedCommands)
{
    static constexpr uint32_t MAXIMUM_COMMAND_COUNT = 2U;
    static constexpr std::array<Draws::Range, 1> TEXTURED_RANGES{
        Draws::Range{ 0U, Draws::QUADS_PER_INDEX_BLOCK + 1U },
    };
    static constexpr std::array<Draws::Range, 1> SOLID_RANGES{
        Draws::Range{ 40U, 1U },
    };
    FactoryProbe probe;
    Draws unavailable = makeDraws(
        probe,
        MAXIMUM_COMMAND_COUNT,
        4U,
        false,
        true,
        Draws::QuadMode::IndexedBlock
    );
    Draws::Prepared const fallback = unavailable.prepareAcquiredSlot(7U, TEXTURED_RANGES, {});
    EXPECT_EQ(fallback.buffer, nullptr);
    EXPECT_TRUE(fallback.indexed);
    EXPECT_EQ(fallback.textured_command_count, 2U);
    EXPECT_EQ(fallback.logical_draw_count, 2U);
    EXPECT_EQ(probe.attempts, 0U);

    Draws enabled = makeDraws(
        probe,
        MAXIMUM_COMMAND_COUNT,
        4U,
        true,
        true,
        Draws::QuadMode::IndexedBlock
    );
    EXPECT_THROW(
        static_cast<void>(enabled.prepareAcquiredSlot(7U, TEXTURED_RANGES, SOLID_RANGES)),
        std::invalid_argument
    );
    EXPECT_EQ(probe.attempts, 0U);
}

TEST(StoneIndirectDrawsTest, IndexedQuadsPreserveInstancesAndUseIndexedCommandStride)
{
    static constexpr uint32_t MAXIMUM_COMMAND_COUNT = 16U;
    static constexpr uint32_t MAXIMUM_DRAW_COUNT = 2U;
    static constexpr uint32_t INDEX_COUNT = 6U;
    std::array const textured{ Draws::Range{ 10U, 3U }, Draws::Range{ 30U, 0U }, Draws::Range{ 50U, 7U } };
    std::array const solid{ Draws::Range{ 100U, 11U } };
    FactoryProbe probe;
    probe.indexed = true;
    Draws draws{
        MAXIMUM_COMMAND_COUNT,
        MAXIMUM_DRAW_COUNT,
        true,
        true,
        [&probe](uint32_t const capacity) { return probe.create(capacity); },
        Draws::QuadMode::IndexedInstanced,
    };
    Draws::Prepared const prepared = draws.prepareAcquiredSlot(91U, textured, solid);
    ASSERT_NE(prepared.buffer, nullptr);
    ASSERT_EQ(probe.created.size(), 1U);
    EXPECT_TRUE(prepared.indexed);
    EXPECT_EQ(prepared.logical_draw_count, 4U);
    auto const& commands = probe.created[0]->indexed_commands;
    std::array const expected{ textured[0], textured[1], textured[2], solid[0] };
    for (uint32_t index = 0U; index < expected.size(); ++index) {
        EXPECT_EQ(commands[index].indexCount, INDEX_COUNT);
        EXPECT_EQ(commands[index].instanceCount, expected[index].instance_count);
        EXPECT_EQ(commands[index].firstIndex, 0U);
        EXPECT_EQ(commands[index].vertexOffset, 0);
        EXPECT_EQ(commands[index].firstInstance, expected[index].first_instance);
    }
    prepared.record(VK_NULL_HANDLE, 0U, 3U);
    prepared.record(VK_NULL_HANDLE, 3U, 1U);
    auto const& batches = probe.created[0]->batches;
    ASSERT_EQ(batches.size(), 3U);
    EXPECT_EQ(batches[0].offset, 0U);
    EXPECT_EQ(batches[0].draw_count, 2U);
    EXPECT_EQ(batches[1].offset, 2U * sizeof(VkDrawIndexedIndirectCommand));
    EXPECT_EQ(batches[1].draw_count, 1U);
    EXPECT_EQ(batches[2].offset, 3U * sizeof(VkDrawIndexedIndirectCommand));
    EXPECT_EQ(batches[2].draw_count, 1U);
}

TEST(StoneIndirectDrawsTest, IndexedFeatureFallbackRetainsLogicalDrawsWithoutAllocation)
{
    static constexpr uint32_t MAXIMUM_COMMAND_COUNT = 16U;
    std::array const ranges{ Draws::Range{ 10U, 3U } };
    FactoryProbe probe;
    Draws draws{
        MAXIMUM_COMMAND_COUNT,
        4U,
        false,
        true,
        [&probe](uint32_t const capacity) { return probe.create(capacity); },
        Draws::QuadMode::IndexedInstanced,
    };
    Draws::Prepared const prepared = draws.prepareAcquiredSlot(7U, ranges, {});
    EXPECT_TRUE(prepared.indexed);
    EXPECT_EQ(prepared.buffer, nullptr);
    EXPECT_EQ(prepared.logical_draw_count, 1U);
    EXPECT_EQ(probe.attempts, 0U);
}

TEST(StoneIndirectDrawsTest, SingleDrawDeviceLimitRetainsEveryCommandAndChecksSubranges)
{
    static constexpr uint32_t MAXIMUM_COMMAND_COUNT = 4U;
    std::array const ranges{ Draws::Range{ 10U, 3U }, Draws::Range{ 20U, 5U }, Draws::Range{ 30U, 7U } };
    FactoryProbe probe;
    Draws draws = makeDraws(probe, MAXIMUM_COMMAND_COUNT, 1U);
    Draws::Prepared const prepared = draws.prepareAcquiredSlot(91U, ranges, {});
    prepared.record(VK_NULL_HANDLE, 0U, 3U);
    ASSERT_EQ(probe.created[0]->batches.size(), 3U);
    for (Batch const batch : probe.created[0]->batches) {
        EXPECT_EQ(batch.draw_count, 1U);
    }
    EXPECT_THROW(prepared.record(VK_NULL_HANDLE, 2U, 2U), std::invalid_argument);
    EXPECT_THROW(prepared.record(VK_NULL_HANDLE, 4U, 0U), std::invalid_argument);
}

TEST(StoneIndirectDrawsTest, GrowsOnlyTheAcquiredSlotAndKeepsOtherSubmittedSlotsUntouched)
{
    static constexpr uint32_t MAXIMUM_COMMAND_COUNT = 16U;
    static constexpr uint32_t FIRST_SLOT = 7U;
    static constexpr uint32_t SECOND_SLOT = 91U;
    std::array const ranges{ Draws::Range{ 10U, 3U }, Draws::Range{ 20U, 5U }, Draws::Range{ 30U, 7U } };
    FactoryProbe probe;
    {
        Draws draws = makeDraws(probe, MAXIMUM_COMMAND_COUNT);
        static_cast<void>(draws.prepareAcquiredSlot(FIRST_SLOT, std::span{ ranges }.first(1U), {}));
        probe.created[0]->submitted = true;
        static_cast<void>(draws.prepareAcquiredSlot(SECOND_SLOT, std::span{ ranges }.first(1U), {}));
        static_cast<void>(draws.prepareAcquiredSlot(SECOND_SLOT, ranges, {}));
        ASSERT_EQ(probe.created.size(), 3U);
        EXPECT_FALSE(probe.created[0]->destroyed);
        EXPECT_FALSE(probe.created[0]->touched_while_submitted);
        EXPECT_TRUE(probe.created[1]->destroyed);
        EXPECT_FALSE(probe.created[1]->touched_while_submitted);
        EXPECT_EQ(probe.created[2]->commands[2].firstInstance, 30U);
        probe.created[0]->submitted = false;
        static_cast<void>(draws.prepareAcquiredSlot(FIRST_SLOT, std::span{ ranges }.first(1U), {}));
        EXPECT_EQ(probe.attempts, 3U);
    }
    EXPECT_TRUE(probe.created[0]->destroyed);
    EXPECT_TRUE(probe.created[2]->destroyed);
    EXPECT_FALSE(probe.created[0]->touched_while_submitted);
    EXPECT_FALSE(probe.created[2]->touched_while_submitted);
}

TEST(StoneIndirectDrawsTest, FailedGrowthFallsBackCleansPartialResourcesAndRetainsReusableSlot)
{
    static constexpr uint32_t MAXIMUM_COMMAND_COUNT = 16U;
    std::array const ranges{ Draws::Range{ 10U, 3U }, Draws::Range{ 20U, 5U }, Draws::Range{ 30U, 7U } };
    FactoryProbe probe;
    Draws draws = makeDraws(probe, MAXIMUM_COMMAND_COUNT);
    Draws::Prepared const first = draws.prepareAcquiredSlot(7U, std::span{ ranges }.first(1U), {});
    probe.failure = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    Draws::Prepared const fallback = draws.prepareAcquiredSlot(7U, ranges, {});
    EXPECT_EQ(fallback.buffer, nullptr);
    EXPECT_EQ(fallback.logical_draw_count, ranges.size());
    EXPECT_FALSE(probe.created[0]->destroyed);
    EXPECT_TRUE(probe.created[1]->destroyed);
    EXPECT_EQ(probe.created[0]->commands[0].firstInstance, 10U);
    probe.failure.reset();
    Draws::Prepared const reused = draws.prepareAcquiredSlot(7U, std::span{ ranges }.first(1U), {});
    EXPECT_EQ(reused.buffer, first.buffer);
    EXPECT_EQ(probe.attempts, 2U);
    Draws::Prepared const grown = draws.prepareAcquiredSlot(7U, ranges, {});
    EXPECT_NE(grown.buffer, nullptr);
    EXPECT_TRUE(probe.created[0]->destroyed);
    EXPECT_EQ(probe.attempts, 3U);
}

TEST(StoneIndirectDrawsTest, HostMemoryFailureFallsBackAndCleansPartialResources)
{
    static constexpr uint32_t MAXIMUM_COMMAND_COUNT = 16U;
    std::array const ranges{ Draws::Range{ 10U, 3U } };
    FactoryProbe probe;
    probe.failure = VK_ERROR_OUT_OF_HOST_MEMORY;
    Draws draws = makeDraws(probe, MAXIMUM_COMMAND_COUNT);
    Draws::Prepared const prepared = draws.prepareAcquiredSlot(7U, ranges, {});
    EXPECT_EQ(prepared.buffer, nullptr);
    EXPECT_EQ(prepared.logical_draw_count, 1U);
    EXPECT_TRUE(probe.created[0]->destroyed);
}

TEST(StoneIndirectDrawsTest, MissingCoherentMappingFallsBackAndCleansPartialResources)
{
    static constexpr uint32_t MAXIMUM_COMMAND_COUNT = 16U;
    std::array const ranges{ Draws::Range{ 10U, 3U } };
    FactoryProbe probe;
    probe.failure = VK_ERROR_MEMORY_MAP_FAILED;
    Draws draws = makeDraws(probe, MAXIMUM_COMMAND_COUNT);
    Draws::Prepared const prepared = draws.prepareAcquiredSlot(7U, ranges, {});
    EXPECT_EQ(prepared.buffer, nullptr);
    EXPECT_TRUE(probe.created[0]->destroyed);
}

TEST(StoneIndirectDrawsTest, CppAllocationFailureFallsBackAndCleansPartialResources)
{
    static constexpr uint32_t MAXIMUM_COMMAND_COUNT = 16U;
    std::array const ranges{ Draws::Range{ 10U, 3U } };
    FactoryProbe probe;
    probe.fail_cpp_allocation = true;
    Draws draws = makeDraws(probe, MAXIMUM_COMMAND_COUNT);
    EXPECT_EQ(draws.prepareAcquiredSlot(7U, ranges, {}).buffer, nullptr);
    EXPECT_TRUE(probe.created[0]->destroyed);
}

TEST(StoneIndirectDrawsTest, DeviceLossPropagatesItsTypedResultAfterPartialCleanup)
{
    static constexpr uint32_t MAXIMUM_COMMAND_COUNT = 16U;
    std::array const ranges{ Draws::Range{ 10U, 3U } };
    FactoryProbe probe;
    probe.failure = VK_ERROR_DEVICE_LOST;
    Draws draws = makeDraws(probe, MAXIMUM_COMMAND_COUNT);
    try {
        static_cast<void>(draws.prepareAcquiredSlot(7U, ranges, {}));
        FAIL() << "device loss was hidden by optional drawing";
    } catch (Draws::AllocationError const& error) {
        EXPECT_EQ(error.result(), VK_ERROR_DEVICE_LOST);
        EXPECT_EQ(std::string{ error.what() }, "injected stone allocation failed with Vulkan result -4");
    }
    EXPECT_TRUE(probe.created[0]->destroyed);
}

TEST(StoneIndirectDrawsTest, UnexpectedAllocationErrorsPropagateInsteadOfHidingInvalidState)
{
    static constexpr uint32_t MAXIMUM_COMMAND_COUNT = 16U;
    std::array const ranges{ Draws::Range{ 10U, 3U } };
    FactoryProbe probe;
    probe.failure = VK_ERROR_INITIALIZATION_FAILED;
    Draws draws = makeDraws(probe, MAXIMUM_COMMAND_COUNT);
    try {
        static_cast<void>(draws.prepareAcquiredSlot(7U, ranges, {}));
        FAIL() << "unexpected allocation error was hidden";
    } catch (Draws::AllocationError const& error) {
        EXPECT_EQ(error.result(), VK_ERROR_INITIALIZATION_FAILED);
        EXPECT_EQ(std::string{ error.what() }, "injected stone allocation failed with Vulkan result -3");
    }
    EXPECT_TRUE(probe.created[0]->destroyed);
}

TEST(StoneIndirectDrawsTest, ClearReleasesEverySlotAndAllowsResourcesToBeRecreated)
{
    static constexpr uint32_t MAXIMUM_COMMAND_COUNT = 16U;
    std::array const ranges{ Draws::Range{ 10U, 3U } };
    FactoryProbe probe;
    Draws draws = makeDraws(probe, MAXIMUM_COMMAND_COUNT);
    static_cast<void>(draws.prepareAcquiredSlot(7U, ranges, {}));
    static_cast<void>(draws.prepareAcquiredSlot(91U, ranges, {}));
    draws.clear();
    EXPECT_TRUE(probe.created[0]->destroyed);
    EXPECT_TRUE(probe.created[1]->destroyed);
    draws.clear();
    EXPECT_NE(draws.prepareAcquiredSlot(7U, ranges, {}).buffer, nullptr);
    EXPECT_EQ(probe.attempts, 3U);
}

TEST(StoneIndirectDrawsTest, ValidatesBothRangeCountsBeforeAnyFactoryAllocation)
{
    static constexpr uint32_t MAXIMUM_COMMAND_COUNT = 2U;
    std::array const ranges{ Draws::Range{ 10U, 3U }, Draws::Range{ 20U, 5U }, Draws::Range{ 30U, 7U } };
    FactoryProbe probe;
    Draws draws = makeDraws(probe, MAXIMUM_COMMAND_COUNT);
    EXPECT_THROW(static_cast<void>(draws.prepareAcquiredSlot(7U, ranges, {})), std::invalid_argument);
    EXPECT_THROW(
        static_cast<void>(draws.prepareAcquiredSlot(7U, std::span{ ranges }.first(1U), ranges)),
        std::invalid_argument
    );
    EXPECT_EQ(probe.attempts, 0U);
}

} // namespace

TEST(StoneIndirectDrawsTest, TerrainTrianglePreservesSignedOffsetsAndFractionalHeightsInSharedLayout)
{
    std::array<std::array<int32_t, 3>, 3> const vertices {
        std::array { 65'528, 65'528, 12'801 },
        std::array { 65'520, 65'536, 12'825 },
        std::array { 65'536, 65'536, 12'800 },
    };
    auto const face = client::StoneFaceInstance::fromTerrainTriangle(vertices, shared::Block::Grass);
    EXPECT_EQ(face.direction, 6U);
    EXPECT_EQ(face.x, vertices[0][0]);
    EXPECT_EQ(face.y, vertices[0][1]);
    EXPECT_EQ(static_cast<uint32_t>(face.z) & 0xffffU, 12'801U);
    EXPECT_EQ(static_cast<uint32_t>(face.z) >> 16U, 12'825U);
    EXPECT_EQ(static_cast<int16_t>(face.u_extent & 0xffffU), -8);
    EXPECT_EQ(static_cast<int16_t>(face.u_extent >> 16U), 8);
    EXPECT_EQ(static_cast<int16_t>(face.v_extent & 0xffffU), 8);
    EXPECT_EQ(static_cast<int16_t>(face.v_extent >> 16U), 8);
    EXPECT_EQ(face.reserved_1, 12'800U);
    EXPECT_EQ(face.material_id, static_cast<uint32_t>(shared::Block::Grass));
    EXPECT_EQ(sizeof(face), 32U);
}

TEST(StoneIndirectDrawsTest, RejectsUnrepresentableTerrainTriangleHeights)
{
    std::array<std::array<int32_t, 3>, 3> vertices { };
    vertices[1][2] = -1;
    EXPECT_THROW(static_cast<void>(client::StoneFaceInstance::fromTerrainTriangle(vertices, shared::Block::Stone)),
        std::invalid_argument);
}

TEST(StoneIndirectDrawsTest, RejectsUnrepresentableTerrainTriangleOffsets)
{
    std::array<std::array<int32_t, 3>, 3> vertices { };
    vertices[1][0] = 32'768;
    EXPECT_THROW(static_cast<void>(client::StoneFaceInstance::fromTerrainTriangle(vertices, shared::Block::Stone)),
        std::invalid_argument);
}

TEST(StoneIndirectDrawsTest, SurfaceTrianglePacksSignedInterpolatedClimateWithoutChangingGeometry)
{
    std::array<std::array<int32_t, 3>, 3> const vertices{
        std::array{ 16, 16, 12'321 }, std::array{ 0, 16, 12'449 }, std::array{ 16, 0, 12'288 },
    };
    auto const face = client::StoneFaceInstance::fromTerrainSurfaceTriangle(vertices, { -160, -64, 400 });
    EXPECT_NE(face.reserved_1 & 0x8000'0000U, 0U);
    uint32_t const encoded_temperature = (face.reserved_1 << 1U) >> 17U;
    int32_t const temperature = encoded_temperature < 0x4000U
        ? static_cast<int32_t>(encoded_temperature)
        : static_cast<int32_t>(encoded_temperature) - 0x8000;
    EXPECT_EQ(temperature, -160);
    EXPECT_EQ(static_cast<int16_t>(face.material_id), -64);
    EXPECT_EQ(static_cast<int16_t>(face.material_id >> 16U), 400);
    EXPECT_EQ(face.reserved_1 & 0xffffU, 12'288U);
    auto const explicit_face = client::StoneFaceInstance::fromTerrainTriangle(vertices, shared::Block::Stone);
    EXPECT_EQ(explicit_face.reserved_1 & 0x8000'0000U, 0U);
    EXPECT_EQ(face.z, explicit_face.z);
    EXPECT_EQ(face.u_extent, explicit_face.u_extent);
    EXPECT_EQ(face.v_extent, explicit_face.v_extent);
}
