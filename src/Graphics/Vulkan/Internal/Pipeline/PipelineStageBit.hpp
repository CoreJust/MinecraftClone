#pragma once

namespace graphics::vulkan::internal {
    enum class PipelineStageBit {
        None                        = 0,
        TopOfPipe                   = 1,
        DrawIndirect                = 2,
        VertexInput                 = 4,
        VertexShader                = 8,
        TesselationControlShader    = 16,
        TesselationEvaluationShader = 32,
        GeometryShader              = 64,
        FragmentShader              = 128,
        EarlyFragmentTest           = 256,
        LateFragmentTest            = 512,
        ColorAttachmentOutput       = 1024,
        ComputeShader               = 2048,
        Transfer                    = 4096,
        BottomOfPipe                = 8192,
        Host                        = 16'384,
    };

    constexpr PipelineStageBit operator|(PipelineStageBit lhs, PipelineStageBit rhs) noexcept {
        return static_cast<PipelineStageBit>(static_cast<unsigned int>(lhs) | static_cast<unsigned int>(rhs));
    }

    constexpr PipelineStageBit operator&(PipelineStageBit lhs, PipelineStageBit rhs) noexcept {
        return static_cast<PipelineStageBit>(static_cast<unsigned int>(lhs) & static_cast<unsigned int>(rhs));
    }
} // namespace graphics::vulkan::internal
