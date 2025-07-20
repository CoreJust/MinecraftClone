#pragma once
#include <Core/Common/Int.hpp>

namespace graphics::vulkan::internal {
    enum class PipelineStage {
        Vertex,
        Geometry,
        Fragment,
        Compute,

        PipelineStagesCount,
    };

    u32 pipelineStageToVK(PipelineStage stage);
} // namespace graphics::vulkan::internal
