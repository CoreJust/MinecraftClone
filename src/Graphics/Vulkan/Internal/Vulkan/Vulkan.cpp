#include "Vulkan.hpp"
#include <Core/Common/Assert.hpp>
#include <Core/IO/Logger.hpp>
#include <Graphics/Window/Window.hpp>
#include "../../Version.hpp"
#include "../../Exception.hpp"
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

    void Vulkan::creationFailure(char const* const resourceName) {
        core::error("Failed to create {}", resourceName);
        throw VulkanException { };
    }
} // namespace graphics::vulkan::internal
