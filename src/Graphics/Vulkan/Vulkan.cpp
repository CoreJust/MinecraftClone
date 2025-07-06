#include "Vulkan.hpp"
#include <cstdlib>
#include <Core/IO/Logger.hpp>
#include "Internal/Instance.hpp"
#include "Internal/Surface.hpp"
#include "Internal/PhysicalDevice.hpp"
#include "Internal/Device.hpp"
#include "Internal/Queue.hpp"
#include "Internal/Functions.hpp"
#include "Exception.hpp"
#include "Extensions.hpp"
#include "Layers.hpp"
#include "Version.hpp"

namespace graphics::vulkan {
    Vulkan::Vulkan(void* window, void* surfaceCreator, char const* const appName, char const** windowRequiredExtensions, uint32_t windowRequiredExtensionsCount) {
        internal::loadInstancelessVkFunctions();
        loadVkVersion();
        loadVkSupportedLayerList();
        loadVkSupportedExtensionList();
        m_instance       = core::memory::makeUP<internal::Instance>(appName, windowRequiredExtensions, windowRequiredExtensionsCount);
        m_surface        = core::memory::makeUP<internal::Surface>(window, surfaceCreator, *m_instance);
        m_physicalDevice = core::memory::makeUP<internal::PhysicalDevice>(internal::PhysicalDevice::choosePhysicalDevice(*m_instance, *m_surface));
        m_device         = core::memory::makeUP<internal::Device>(*m_physicalDevice);
        internal::QueueMaker queueMaker(m_device->get(), m_physicalDevice->queueFamilies());
        m_graphicsQueue  = queueMaker.make(internal::QueueType::Graphics);
        m_presentQueue   = queueMaker.make(internal::QueueType::Present);
    }
} // namespace graphics::vulkan

