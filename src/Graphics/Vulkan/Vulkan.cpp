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
        m_instance       = new internal::Instance(appName, windowRequiredExtensions, windowRequiredExtensionsCount);
        m_surface        = new internal::Surface(window, surfaceCreator, *m_instance);
        m_physicalDevice = new internal::PhysicalDevice(internal::PhysicalDevice::choosePhysicalDevice(*m_instance));
        m_device         = new internal::Device(*m_physicalDevice);
        m_graphicsQueue  = new internal::Queue(m_device->get(), m_physicalDevice->queueFamilies().getIndex(internal::QueueType::Graphics));
    }

    Vulkan::~Vulkan() {
        if (m_graphicsQueue) {
            delete m_graphicsQueue;
            m_graphicsQueue = nullptr;
        }
        if (m_device) {
            delete m_device;
            m_device = nullptr;
        }
        if (m_physicalDevice) {
            delete m_physicalDevice;
            m_physicalDevice = nullptr;
        }
        if (m_surface) {
            delete m_surface;
            m_surface = nullptr;
        }
        if (m_instance) {
            delete m_instance;
            m_instance = nullptr;
        }
    }
} // namespace graphics::vulkan

