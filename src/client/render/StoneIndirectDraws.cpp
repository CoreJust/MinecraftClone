#include <client/render/StoneIndirectDraws.hpp>

#include <algorithm>
#include <new>
#include <string>
#include <utility>

namespace client {

namespace {

constexpr uint32_t STONE_FACE_VERTEX_COUNT = 6U;

[[nodiscard]] bool isRecoverableAllocationResult(VkResult const result) noexcept
{
    return result == VK_ERROR_OUT_OF_HOST_MEMORY
        || result == VK_ERROR_OUT_OF_DEVICE_MEMORY
        || result == VK_ERROR_MEMORY_MAP_FAILED;
}

} // namespace

StoneIndirectDraws::AllocationError::AllocationError(VkResult const result, char const* const operation)
    : std::runtime_error(std::string{ operation } + " failed with Vulkan result " + std::to_string(result))
    , m_result(result)
{}

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
    uint32_t const maximum_draw_count,
    bool const multi_draw_indirect_enabled,
    bool const draw_indirect_first_instance_enabled,
    Factory factory,
    bool const indexed
)
    : m_maximum_command_count(maximum_command_count)
    , m_maximum_draw_count(multi_draw_indirect_enabled && draw_indirect_first_instance_enabled
        ? maximum_draw_count : 0U)
    , m_factory(std::move(factory))
    , m_indexed(indexed)
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
    if (draw_ranges.size() > m_maximum_command_count) {
        throw std::invalid_argument("visible stone ranges exceed the indirect command limit");
    }
    uint32_t const textured_count = static_cast<uint32_t>(draw_ranges.size());
    if (solid_draw_ranges.size() > m_maximum_command_count - textured_count) {
        throw std::invalid_argument("visible stone ranges exceed the indirect command limit");
    }
    uint32_t const command_count = textured_count + static_cast<uint32_t>(solid_draw_ranges.size());
    Prepared prepared{
        .logical_draw_count = command_count,
        .maximum_draw_count = m_maximum_draw_count,
        .indexed = m_indexed,
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
    if (m_indexed) {
        std::span<VkDrawIndexedIndirectCommand> const commands = entry->second->mappedIndexedCommands();
        if (commands.size() < command_count) {
            throw std::logic_error("stone indexed indirect buffer has insufficient mapped capacity");
        }
        uint32_t command_index = 0U;
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
