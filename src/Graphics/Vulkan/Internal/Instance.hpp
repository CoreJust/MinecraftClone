#pragma once
#include <vulkan/vulkan.h>
#include <Core/Macro/Attributes.hpp>

namespace graphics::vulkan::internal {
    class Instance final {
        VkInstance m_instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;

    public:
        Instance() noexcept = default;
        Instance(char const* const appName, char const** windowRequiredExtensions, uint32_t windowRequiredExtensionsCount);
        Instance(Instance&&) noexcept = default;
        Instance(const Instance&) noexcept = delete;
        Instance& operator=(Instance&&) noexcept = default;
        Instance& operator=(const Instance&) noexcept = delete;
        ~Instance();

        PURE VkInstance get() const noexcept { return m_instance; };
    };
} // namespace graphics::vulkan::internal
