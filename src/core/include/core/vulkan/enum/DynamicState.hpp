#pragma once

#include <core/meta/Enum.hpp>

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
    LineStripe,

    // TODO: fill in the rest

    Count,
};

[[nodiscard]]
constexpr DynamicState dynamicStateFromVk(uint32_t const state) noexcept {
    return static_cast<DynamicState>(
        state <= 8
            ? state
            : state == 1000259000
                ? static_cast<uint32_t>(DynamicState::LineStripe)
                : state >= 1000267000 && state <= 1000267011
                    ? state - 1000267000 + static_cast<uint32_t>(DynamicState::CullMode)
                    : state - 1000377000 + static_cast<uint32_t>(DynamicState::PatchControlPoints)
    );
}

[[nodiscard]]
constexpr uint32_t dynamicStateToVk(DynamicState const state) noexcept {
    uint32_t const as_uint32 = static_cast<uint32_t>(state);
    return as_uint32 <= 8
            ? as_uint32
            : state == DynamicState::LineStripe
                ? 1000259000
                : true
                    && as_uint32 >= static_cast<uint32_t>(DynamicState::CullMode)
                    && as_uint32 <= static_cast<uint32_t>(DynamicState::StencilOp)
                    ? as_uint32 + 1000267000 - static_cast<uint32_t>(DynamicState::CullMode)
                    : as_uint32 + 1000377000 - static_cast<uint32_t>(DynamicState::PatchControlPoints)
    ;
}

} // namespace core::vk

CORE_ENUM_FUNCTIONS(vk::DynamicState);
