#pragma once

#include <core/vulkan/SurfaceProvider.hpp>

struct ANativeWindow;

namespace game_android {

class AndroidSurfaceProvider final : public core::vk::SurfaceProvider {
public:
    explicit AndroidSurfaceProvider(ANativeWindow* window) noexcept;

    [[nodiscard]]
    std::span<core::vk::VulkanExtension const> requiredInstanceExtensions() const noexcept override;
    [[nodiscard]]
    core::vk::Extent2d framebufferExtent() const noexcept override;
    [[nodiscard]]
    bool isFramebufferExtentZero() const noexcept override;
    [[nodiscard]]
    VkSurfaceKHR createSurface(VkInstance instance) const override;
private:
    ANativeWindow* m_window = nullptr;
};

} // namespace game_android
