#pragma once

namespace graphics::vulkan::pipeline {
    struct PipelineOptions final {
        char const* const vertexShaderPath;
        char const* const fragmentShaderPath;
    };
} // namespace graphics::vulkan::pipeline
