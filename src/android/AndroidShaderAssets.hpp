#pragma once

#include <client/render/ShaderAssets.hpp>

struct AAssetManager;

namespace game_android {

class AndroidShaderAssets final : public client::ShaderAssets {
public:
    explicit AndroidShaderAssets(AAssetManager* asset_manager) noexcept;

    [[nodiscard]]
    core::vk::SpirV load(std::string_view name) const override;
private:
    AAssetManager* m_asset_manager = nullptr;
};

} // namespace game_android
