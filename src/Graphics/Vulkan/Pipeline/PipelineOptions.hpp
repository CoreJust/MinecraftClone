#pragma once
#include <Core/Container/ArrayView.hpp>
#include "Attribute.hpp"
#include "Descriptor.hpp"

namespace graphics::vulkan::pipeline {
    struct PipelineOptions final {
        char const* const vertexShaderPath;
        char const* const fragmentShaderPath;
        core::ArrayView<Attribute  const> attributes;
        core::ArrayView<Attribute  const> vertexPushContants   { };
        core::ArrayView<Attribute  const> fragmentPushContants { };
        core::ArrayView<Descriptor const> descriptors          { };
    };
} // namespace graphics::vulkan::pipeline
