#pragma once
#include <vulkan/vulkan.h>
#include <Core/Macro/Attributes.hpp>

namespace graphics::vulkan::internal {
    class Device;

    class Pipeline final {
    public:
        Pipeline(Device& device, char const* const vertexShaderPath, char const* const fragmentShaderPath);
        ~Pipeline();
    };
} // namespace graphics::vulkan::internal
