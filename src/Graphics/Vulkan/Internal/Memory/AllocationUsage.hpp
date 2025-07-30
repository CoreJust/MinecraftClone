#pragma once

namespace graphics::vulkan::internal {
    enum class AllocationUsage {
        Unknown,
        GpuOnly,
        CpuOnly,
        CpuToGpu,
        CpuCopy,
        GpuLazilyAllocated,
        Auto,
        AutoPreferDevice,
        AutoPreferHost,
    };
} // namespace graphics::vulkan::internal
