#include <core/vulkan/GraphicsPipeline.hpp>

#include <volk.h>

namespace core::vk {

CORE_VK_RESOURCE_DESTROY_IMPL(RawGraphicsPipeline) {
    vkDestroyPipeline(device_handle, self.m_handle, nullptr);
}

BoundGraphicsPipeline RawGraphicsPipeline::bind(RawCommandBuffer cmd) const {
    vkCmdBindPipeline(cmd.handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, m_handle);
    return BoundGraphicsPipeline{ *this, cmd };
}

void RawGraphicsPipeline::pushConstants(
    RawCommandBuffer cmd,
    ShaderStages const stages,
    uint32_t const offset,
    std::span<uint8_t const> const data
) const {
    vkCmdPushConstants(
        cmd.handle(),
        m_layout.handle(),
        static_cast<VkShaderStageFlags>(stages.value),
        offset,
        static_cast<uint32_t>(data.size_bytes()),
        data.data()
    );
}

void RawGraphicsPipeline::draw(
    RawCommandBuffer cmd,
    uint32_t const vertexCount,
    uint32_t const instanceCount,
    uint32_t const firstVertex,
    uint32_t const firstInstance
) const {
    vkCmdDraw(cmd.handle(), vertexCount, instanceCount, firstVertex, firstInstance);
}

void RawGraphicsPipeline::drawIndexed(
    RawCommandBuffer cmd,
    uint32_t const indexCount,
    uint32_t const instanceCount,
    uint32_t const firstIndex,
    int32_t const vertexOffset,
    uint32_t const firstInstance
) const {
    vkCmdDrawIndexed(cmd.handle(), indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void RawGraphicsPipeline::drawMeshTasks(
    RawCommandBuffer cmd,
    uint32_t const groupCountX,
    uint32_t const groupCountY,
    uint32_t const groupCountZ
) const {
    vkCmdDrawMeshTasksEXT(cmd.handle(), groupCountX, groupCountY, groupCountZ);
}

} // namespace core::vk
