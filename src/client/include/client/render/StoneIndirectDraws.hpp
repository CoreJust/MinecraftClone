#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <unordered_map>

namespace client {

class StoneIndirectDraws final {
public:
    static constexpr std::array<uint16_t, 6> QUAD_INDICES{ 0U, 1U, 2U, 0U, 2U, 3U };

    class Buffer;
    using Factory = std::function<std::unique_ptr<Buffer>(uint32_t)>;

    struct Range final {
        uint32_t first_instance = 0U;
        uint32_t instance_count = 0U;
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
        uint32_t maximum_draw_count = 0U;
        bool indexed = false;

        void record(VkCommandBuffer command, uint32_t first_command, uint32_t command_count) const;
    };

    StoneIndirectDraws(
        uint32_t maximum_command_count,
        uint32_t maximum_draw_count,
        bool multi_draw_indirect_enabled,
        bool draw_indirect_first_instance_enabled,
        Factory factory,
        bool indexed = false
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
    bool m_indexed = false;
    std::unordered_map<uint32_t, std::unique_ptr<Buffer>> m_buffers;
};

} // namespace client
