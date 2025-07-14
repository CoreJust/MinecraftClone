#include "Vulkan.hpp"
#include <Core/Common/Assert.hpp>
#include <Core/IO/Logger.hpp>
#include <Graphics/Window/Window.hpp>
#include "../../Version.hpp"
#include "../../Exception.hpp"
#include "Layers.hpp"
#include "Extensions.hpp"
#include "Functions.hpp"

namespace graphics::vulkan::internal {

    Vulkan::Vulkan(window::Window& win, core::common::Version const& appVersion) {
        loadInstancelessVkFunctions();
        loadVkSupportedLayerList();
        loadVkSupportedExtensionList();
        loadVkVersion();

        m_instance       = core::memory::makeUP<Instance>(ProjectInfo { win.name(), { appVersion } }, win.getRequiredExtensions());
        m_surface        = core::memory::makeUP<Surface>(win, *m_instance);
        m_physicalDevice = PhysicalDevice::choosePhysicalDevice(*this);
        updateVkSupportedExtensionListForDevice(*m_physicalDevice);
        m_device         = core::memory::makeUP<Device>(*m_physicalDevice);
        m_swapchain      = core::memory::makeUP<internal::Swapchain>(*this, win.pixelSize());
    }

    Vulkan::~Vulkan() = default;
        
    void Vulkan::recreateSwapchain(core::math::Vec2u32 pixelSize) {
        core::io::debug("Vulkan::recreateSwapchain(pixelSize: {}x{})", pixelSize[0], pixelSize[1]);
        m_swapchain = core::memory::makeUP<internal::Swapchain>(*this, pixelSize);
    }

    void Vulkan::creationFailure(char const* const resourceName) {
        core::io::error("Failed to create {}", resourceName);
        throw VulkanException { };
    }
} // namespace graphics::vulkan::internal
