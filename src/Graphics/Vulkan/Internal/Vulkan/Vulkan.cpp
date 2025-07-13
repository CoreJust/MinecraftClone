#include "Vulkan.hpp"
#include <Core/Common/Assert.hpp>
#include <Core/IO/Logger.hpp>
#include "../../Version.hpp"
#include "../../Exception.hpp"
#include "Layers.hpp"
#include "Extensions.hpp"
#include "Functions.hpp"

namespace graphics::vulkan::internal {
    Vulkan::Vulkan(
        void* window,
        void* surfaceCreator,
        char const* const appName,
        core::common::Version const& appVersion,
        char const** windowRequiredExtensions,
        uint32_t windowRequiredExtensionsCount
    ) {
        ASSERT(window, "WIndow is null");
        loadInstancelessVkFunctions();
        loadVkSupportedLayerList();
        loadVkSupportedExtensionList();
        loadVkVersion();

        m_instance       = core::memory::makeUP<Instance>(ProjectInfo { appName, { appVersion } }, windowRequiredExtensions, windowRequiredExtensionsCount);
        m_surface        = core::memory::makeUP<Surface>(window, surfaceCreator, *m_instance);
        m_physicalDevice = PhysicalDevice::choosePhysicalDevice(*this);
        updateVkSupportedExtensionListForDevice(*m_physicalDevice);
        m_device         = core::memory::makeUP<Device>(*m_physicalDevice);
        m_swapchain      = core::memory::makeUP<internal::Swapchain>(*this, window);
    }

    Vulkan::~Vulkan() = default;

    void Vulkan::creationFailure(char const* const resourceName) {
        core::io::error("Failed to create {}", resourceName);
        throw VulkanException { };
    }
} // namespace graphics::vulkan::internal
