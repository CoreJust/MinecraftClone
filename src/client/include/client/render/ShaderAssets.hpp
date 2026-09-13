#pragma once

#include <core/kernel/Spirv.hpp>

#include <string_view>

namespace client {

class ShaderAssets {
public:
    virtual ~ShaderAssets();

    [[nodiscard]]
    virtual core::kernel::SpirvModule load(std::string_view name) const = 0;
};

} // namespace client
