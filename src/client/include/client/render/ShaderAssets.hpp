#pragma once

#include <core/vulkan/SpirV.hpp>

#include <string_view>

namespace client {

class ShaderAssets {
public:
    virtual ~ShaderAssets();

    [[nodiscard]]
    virtual core::vk::SpirV load(std::string_view name) const = 0;
};

} // namespace client
