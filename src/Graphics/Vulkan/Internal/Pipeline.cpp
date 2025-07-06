#include "Pipeline.hpp"
#include <Core/IO/Logger.hpp>
#include "Device.hpp"
#include "ShaderModule.hpp"

namespace graphics::vulkan::internal {
    Pipeline::Pipeline(Device& device, char const* const vertexShaderPath, char const* const fragmentShaderPath) {
        ShaderModule vertex   { device.get(), vertexShaderPath   ? vertexShaderPath   : "basic.vert" };
        ShaderModule fragment { device.get(), fragmentShaderPath ? fragmentShaderPath : "basic.frag" };
    }

    Pipeline::~Pipeline() {

    }
} // namespace graphics::vulkan::internal
