#pragma once

#include <client/render/ShaderAssets.hpp>

#include <filesystem>
#include <string_view>

namespace client {

[[nodiscard]] std::filesystem::path installedClientAssetPath(std::filesystem::path const& relative_path);

class InstalledShaderAssets final : public ShaderAssets {
public:
    [[nodiscard]]
    core::kernel::SpirvModule load(std::string_view name) const override;
};

} // namespace client
