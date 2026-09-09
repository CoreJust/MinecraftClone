#pragma once

#include <client/render/ShaderAssets.hpp>

#include <core/common/NonCopyable.hpp>
#include <core/common/NonMovable.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace core::vk {

class SurfaceProvider;

} // namespace core::vk

namespace client {

struct PlayerRenderData final {
    uint32_t x = 0;
    uint32_t y = 0;
    std::array<float, 4> color{ 1.0f, 1.0f, 1.0f, 1.0f };
};

struct VulkanRendererOptions final {
    bool require_validation = false;
    bool prefer_mesh_shaders = true;
};

class VulkanRenderer final : core::NonCopyable, core::NonMovable {
public:
    explicit VulkanRenderer(
        core::vk::SurfaceProvider const& surface_provider,
        ShaderAssets const& shader_assets,
        VulkanRendererOptions options = {}
    );
    ~VulkanRenderer();

    void render(std::span<PlayerRenderData const> const players);
    void hotReload();
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace client
