#include "PipelineStage.hpp"
#include <vulkan/vulkan.hpp>

namespace graphics::vulkan::internal {
    u32 pipelineStageToVK(PipelineStage stage) {
        constexpr static u32 VK_PIPELINE_STAGES[] = {
            VK_SHADER_STAGE_VERTEX_BIT,
            VK_SHADER_STAGE_GEOMETRY_BIT,
            VK_SHADER_STAGE_FRAGMENT_BIT,
            VK_SHADER_STAGE_COMPUTE_BIT,
        };

        return VK_PIPELINE_STAGES[static_cast<usize>(stage)];
    }
} // namespace graphics::vulkan::internal
