#pragma once
#include <Core/Collection/ArrayView.hpp>
#include "Attribute.hpp"

namespace graphics::vulkan::pipeline {
    struct PipelineOptions final {
        char const* const vertexShaderPath;
        char const* const fragmentShaderPath;
        core::ArrayView<const Attribute> attributes;
        core::ArrayView<const Attribute> vertexPushContants { };
        core::ArrayView<const Attribute> fragmentPushContants { };
    };
} // namespace graphics::vulkan::pipeline
