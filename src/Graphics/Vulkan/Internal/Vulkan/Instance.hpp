#pragma once
#include <vulkan/vulkan.h>
#include <Core/Macro/Attributes.hpp>
#include <Core/Collection/ArrayView.hpp>
#include <Graphics/Vulkan/Version.hpp>

namespace graphics::vulkan::internal {
    struct ProjectInfo final {
        char const* const name;
        Version version;
    };

    class Instance final {
        VkInstance m_instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;

        Instance();
    public:
        Instance(ProjectInfo const& appInfo, core::ArrayView<char const*> requiredExtensions);
        Instance(Instance&&) noexcept = delete;
        Instance(const Instance&) noexcept = delete;
        Instance& operator=(Instance&&) noexcept = delete;
        Instance& operator=(const Instance&) noexcept = delete;
        ~Instance();

        PURE static Instance makeTemporaryInstance();

        PURE VkInstance get() const noexcept { return m_instance; };
    };
} // namespace graphics::vulkan::internal
