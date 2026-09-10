#pragma once

#include <client/render/ShaderAssets.hpp>

namespace client {

class InstalledShaderAssets final : public ShaderAssets {
public:
    [[nodiscard]]
    core::vk::SpirV load(std::string_view name) const override;
};

} // namespace client
