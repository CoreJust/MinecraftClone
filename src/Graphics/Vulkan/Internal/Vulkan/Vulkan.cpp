#include "Vulkan.hpp"
#include <Core/Common/Assert.hpp>
#include <Core/IO/Logger.hpp>
#include <Graphics/Window/Window.hpp>
#include <Graphics/Vulkan/Version.hpp>
#include <Graphics/Vulkan/Exception.hpp>
#include "Layers.hpp"
#include "Extensions.hpp"
#include "Functions.hpp"

constexpr static usize FRAMES_COUNT = 3;

namespace graphics::vulkan::internal {
    Vulkan::Vulkan(window::Window& win, core::Version const& appVersion) {
        loadInstancelessVkFunctions();
        loadVkSupportedLayerList();
        loadVkSupportedExtensionList();
        loadVkVersion();

        m_instance       = core::makeUP<Instance>(ProjectInfo { win.name(), { appVersion } }, win.getRequiredExtensions());
        m_surface        = core::makeUP<Surface>(win, *m_instance);
        m_physicalDevice = PhysicalDevice::choosePhysicalDevice(*this);
        setDeviceVkVersion(Version::fromVk(m_physicalDevice->props().apiVersion));
        updateVkSupportedExtensionListForDevice(*m_physicalDevice);
        m_device         = core::makeUP<Device>(*m_physicalDevice);
        m_swapchain      = core::makeUP<internal::Swapchain>(*this, win.pixelSize(), FRAMES_COUNT);
    }

    Vulkan::~Vulkan() = default;
        
    void Vulkan::recreateSwapchain(core::Vec2u32 pixelSize) {
        m_swapchain.reset();
        m_physicalDevice->reloadSwapchainSupport(*m_surface);
        m_swapchain = core::makeUP<internal::Swapchain>(*this, pixelSize, FRAMES_COUNT);
    }
    
    VkDeviceMemory Vulkan::allocDevice(usize size, u32 type, VkMemoryPropertyFlags properties) {
        VkPhysicalDeviceMemoryProperties const& memProps = m_physicalDevice->memoryProps();
        u32 heapIdx = static_cast<u32>(-1);
        for (u32 i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((type & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
                heapIdx = i;
                break;
            }
        }
        if (heapIdx == static_cast<u32>(-1)) {
            core::error("Failed to allocate device memory with size {}, type {}, properties {}; No suitable heap found", size, type, properties);
            return VK_NULL_HANDLE;
        }

        VkMemoryAllocateInfo allocInfo { };
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = size;
        allocInfo.memoryTypeIndex = heapIdx;

        VkDeviceMemory result = VK_NULL_HANDLE;
        if (!VK_CHECK(vkAllocateMemory(m_device->get(), &allocInfo, nullptr, &result))) {
            core::error("Failed to allocate device memory with size {}, type {}, properties {}", size, type, properties);
            return VK_NULL_HANDLE;
        }
        return result;
    }

    void Vulkan::creationFailure(char const* const resourceName) {
        core::error("Failed to create {}", resourceName);
        throw VulkanException { };
    }
} // namespace graphics::vulkan::internal
