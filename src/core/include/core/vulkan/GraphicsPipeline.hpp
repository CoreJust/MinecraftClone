#pragma once

#include <core/vulkan/PipelineLayout.hpp>
#include <core/vulkan/CommandBuffer.hpp>
#include <core/vulkan/enum/ShaderStage.hpp>

#include <span>

namespace core::vk {

struct BoundGraphicsPipeline;

class RawGraphicsPipeline : public VulkanResourceBase<VkPipeline> {
    friend struct BoundGraphicsPipeline;

    CORE_VK_RESOURCE_CONTEXT(RawGraphicsPipeline,
        VkDevice device_handle;);
    CORE_VK_RESOURCE_CONSTRUCTION_FROM(
        Device const& device,
        VkPipeline const pipeline,
        RawPipelineLayout layout
    ) {
        self.m_handle = pipeline;
        self.m_layout = layout;
        CORE_VK_CAPTURE_DESTRUCTION_CONTEXT(){ .device_handle = device.handle() };
    }
public:
    BoundGraphicsPipeline bind(RawCommandBuffer cmd) const;

    void execute(RawCommandBuffer cmd, std::invocable<BoundGraphicsPipeline> auto&& fn) const {
        fn(bind(cmd));
    }
private:
    void pushConstants(
        RawCommandBuffer cmd,
        ShaderStages const stages,
        uint32_t const offset,
        std::span<uint8_t const> const data
    ) const;

    void draw(
        RawCommandBuffer cmd,
        uint32_t const vertex_count,
        uint32_t const instance_count,
        uint32_t const first_vertex,
        uint32_t const first_instance
    ) const;

    void drawIndexed(
        RawCommandBuffer cmd,
        uint32_t const index_count,
        uint32_t const instance_count,
        uint32_t const first_index,
        int32_t const vertex_offset,
        uint32_t const first_instance
    ) const;

    void drawMeshTasks(
        RawCommandBuffer cmd,
        uint32_t const group_count_x,
        uint32_t const group_count_y,
        uint32_t const group_count_z
    ) const;

    [[nodiscard]]
    constexpr RawPipelineLayout const& layout() const noexcept { return m_layout; }
private:
    RawPipelineLayout m_layout;
};

using GraphicsPipeline = VulkanRaii<RawGraphicsPipeline>;

struct BoundGraphicsPipeline final {
    RawGraphicsPipeline const& pipeline;
    RawCommandBuffer cmd;

    void pushConstants(
        ShaderStages const stages,
        uint32_t const offset,
        std::span<uint8_t const> const data
    ) const {
        pipeline.pushConstants(cmd, stages, offset, data);
    }

    void pushConstants(
        ShaderStages const stages,
        uint32_t const offset,
        auto const& data
    ) const {
        std::span<uint8_t const> const raw_bytes{
            reinterpret_cast<uint8_t const*>(&data),
            sizeof(data),
        };
        pushConstants(stages, offset, raw_bytes);
    }

    void draw(
        uint32_t const vertex_count,
        uint32_t const instance_count = 1,
        uint32_t const first_vertex = 0,
        uint32_t const first_instance = 0
    ) const {
        pipeline.draw(cmd, vertex_count, instance_count, first_vertex, first_instance);
    }

    void drawIndexed(
        uint32_t const index_count,
        uint32_t const instance_count = 1,
        uint32_t const first_index = 0,
        int32_t const vertex_offset = 0,
        uint32_t const first_instance = 0
    ) const {
        pipeline.drawIndexed(cmd, index_count, instance_count, first_index, vertex_offset, first_instance);
    }

    void drawMeshTasks(
        uint32_t const group_count_x = 1,
        uint32_t const group_count_y = 1,
        uint32_t const group_count_z = 1
    ) const {
        pipeline.drawMeshTasks(cmd, group_count_x, group_count_y, group_count_z);
    }
};

} // namespace core::vk
