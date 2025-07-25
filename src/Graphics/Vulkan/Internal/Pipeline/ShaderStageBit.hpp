#pragma once

namespace graphics::vulkan::internal {
    enum class ShaderStageBit {
        None                  = 0,
        Vertex                = 1,
        TesselationControl    = 2,
        TesselationEvaluation = 4,
        Geometry              = 8,
        Fragment              = 16,
        Compute               = 32,
    };

    constexpr ShaderStageBit operator|(ShaderStageBit lhs, ShaderStageBit rhs) noexcept {
        return static_cast<ShaderStageBit>(static_cast<unsigned int>(lhs) | static_cast<unsigned int>(rhs));
    }

    constexpr ShaderStageBit operator&(ShaderStageBit lhs, ShaderStageBit rhs) noexcept {
        return static_cast<ShaderStageBit>(static_cast<unsigned int>(lhs) & static_cast<unsigned int>(rhs));
    }
} // namespace graphics::vulkan::internal
