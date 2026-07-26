#pragma once

#include <core/common/EnumBits.hpp>
#include <core/vulkan/enum/VulkanEnum.hpp>

namespace core::vk {

enum class ShaderStage {
    Vertex,
    TessellationControl,
    TesselationEvaluation,
    Geometry,
    Fragment,
    Compute,
    Task,
    Mesh,
    Raygen,
    AnyHit,
    ClosestHit,
    Miss,
    Intersection,
    Callable,
    SubpassShading,
    Unused0,
    Unused1,
    Unused2,
    Unused3,
    ClusterCulling,

    Count,
};

using ShaderStages = EnumBits<ShaderStage>;

} // namespace core::vk

CORE_VK_REGISTER_ENUM(ShaderStage);
