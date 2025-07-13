#pragma once
#include <vulkan/vulkan.h>
#include <Core/Macro/Attributes.hpp>
#include <Core/Memory/UniquePtr.hpp>
#include <Core/Collection/DynArray.hpp>
#include "Extensions.hpp"
#include "SwapchainSupport.hpp"
#include "../QueueFamilies.hpp"

namespace graphics::vulkan::internal {
    class Instance;
    class Surface;
    class Vulkan;

    class PhysicalDevice final {
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties m_properties { };
        QueueFamilies m_queueFamilies { };
        SupportedExtensions m_extensions { };
        SwapchainSupport m_swapchainSupport { };

    private:
        struct PassKey { explicit PassKey() = default; };

    public:
        PhysicalDevice(VkPhysicalDevice device, Surface const& surface, PassKey) noexcept;
        static Version getHighestPhysicalDeviceVersion(Instance& instance) noexcept;
        static core::collection::DynArray<core::memory::UniquePtr<PhysicalDevice>> getPhysicalDevices(Vulkan& vulkan) noexcept;
        static core::memory::UniquePtr<PhysicalDevice> choosePhysicalDevice(Vulkan& vulkan);

        PURE VkPhysicalDevice get() const noexcept { return m_physicalDevice; }
        PURE VkPhysicalDeviceProperties const& props()            const noexcept { return m_properties; }
        PURE QueueFamilies              const& queueFamilies()    const noexcept { return m_queueFamilies; }
        PURE SupportedExtensions        const& extensions()       const noexcept { return m_extensions; }
        PURE SwapchainSupport           const& swapchainSupport() const noexcept { return m_swapchainSupport; }
    };
} // namespace graphics::vulkan::internal

