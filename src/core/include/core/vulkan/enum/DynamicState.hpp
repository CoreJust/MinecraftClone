#pragma once

#include <core/vulkan/enum/VulkanEnum.hpp>

namespace core::vk {

enum class DynamicState {
    Viewport,
    Scissor,
    LineWidth,
    DepthBias,
    BlendConstants,
    DepthBounds,
    StencilCompareMask,
    StencilWriteMask,
    StencilReference,
    LineStripe,
    CullMode,
    FrontFace,
    PrimitiveTopology,
    ViewportWithCount,
    ScissorWithCount,
    VertexInputBindingStride,
    DepthTestEnable,
    DepthWriteEnable,
    DepthCompareOp,
    DepthBoundsTestEnable,
    StencilTestEnable,
    StencilOp,
    PatchControlPoints,
    RasterizedDiscardEnable,
    DepthBiasEnable,
    StateLogicOp,
    PrimitiveRestartEnable,

    // TODO: fill in the rest

    Count,
};

} // namespace core::vk

CORE_VK_REGISTER_ENUM(DynamicState,
    { LineStripe, 1'000'259'000 },
    { CullMode,   1'000'267'000 },
    { PatchControlPoints, 1'000'377'000 }
);
