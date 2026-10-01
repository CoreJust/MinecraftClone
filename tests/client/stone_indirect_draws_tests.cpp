#include <client/render/StoneIndirectDraws.hpp>

#include <shared/world/ChunkMesher.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <span>
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
    bool const draw_indirect_first_instance_enabled = true
) {
    return {
        maximum_command_count,
        maximum_draw_count,
        multi_draw_indirect_enabled,
        draw_indirect_first_instance_enabled,
        [&probe](uint32_t const capacity) { return probe.create(capacity); },
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
        true,
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
        true,
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
