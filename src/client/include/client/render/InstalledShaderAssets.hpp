#pragma once

#include <client/render/ShaderAssets.hpp>

namespace client {

class InstalledShaderAssets final : public ShaderAssets {
public:
    [[nodiscard]]
    core::kernel::SpirvModule load(std::string_view name) const override;
};

} // namespace client
