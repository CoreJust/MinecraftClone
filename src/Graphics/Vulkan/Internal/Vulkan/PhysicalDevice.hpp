#pragma once
#include <Core/Macro/Attributes.hpp>
#include <Core/Memory/UniquePtr.hpp>
#include <Core/Collection/DynArray.hpp>
#include "../Wrapper/Handles.hpp"
#include "Extensions.hpp"
#include "../QueueFamilies.hpp"

namespace graphics::vulkan::internal {
    class Instance;
    class Surface;
    class Vulkan;
    struct SwapchainSupport;

    class PhysicalDevice final {
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        core::UniquePtr<VkPhysicalDeviceProperties> m_properties { };
        QueueFamilies m_queueFamilies { };
        SupportedExtensions m_extensions { };
        core::UniquePtr<SwapchainSupport> m_swapchainSupport { };

    private:
        struct PassKey { explicit PassKey() = default; };

    public:
        PhysicalDevice(VkPhysicalDevice device, Surface const& surface, PassKey) noexcept;
        static core::DynArray<core::UniquePtr<PhysicalDevice>> getPhysicalDevices(Vulkan& vulkan) noexcept;
        static core::UniquePtr<PhysicalDevice> choosePhysicalDevice(Vulkan& vulkan);

        PURE VkPhysicalDevice get() const noexcept { return m_physicalDevice; }
        PURE VkPhysicalDeviceProperties const& props()            const noexcept { return *m_properties; }
        PURE QueueFamilies              const& queueFamilies()    const noexcept { return m_queueFamilies; }
        PURE SupportedExtensions        const& extensions()       const noexcept { return m_extensions; }
        PURE SwapchainSupport           const& swapchainSupport() const noexcept { return *m_swapchainSupport; }
    };
} // namespace graphics::vulkan::internal

