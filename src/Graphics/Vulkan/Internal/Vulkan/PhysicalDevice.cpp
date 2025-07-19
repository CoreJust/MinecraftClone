#include "PhysicalDevice.hpp"
#include <vulkan/vulkan.h>
#include <Core/IO/Logger.hpp>
#include <Graphics/Vulkan/Version.hpp>
#include <Graphics/Vulkan/Exception.hpp>
#include "Vulkan.hpp"
#include "SwapchainSupport.hpp"

namespace graphics::vulkan::internal {
namespace {
    u64 evaluateDevice(PhysicalDevice& d) {
        if (!d.queueFamilies().hasFamily(QueueType::Graphics)
            || !d.queueFamilies().hasFamily(QueueType::Present)
            || !d.extensions().hasExtension(VulkanExtension::Swapchain)
            || d.swapchainSupport().formats.size() == 0
            || d.swapchainSupport().presentModes.size() == 0)
            return 0;

        return d.props().apiVersion;
    }
} // namespace


    PhysicalDevice::PhysicalDevice(VkPhysicalDevice device, Surface const& surface, PassKey) noexcept
        : m_physicalDevice(device)
        , m_extensions(*this) {
        reloadSwapchainSupport(surface);
        m_properties = core::makeUP<VkPhysicalDeviceProperties>();
        vkGetPhysicalDeviceProperties(m_physicalDevice, m_properties.get());
        m_memoryProperties = core::makeUP<VkPhysicalDeviceMemoryProperties>();
        vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, m_memoryProperties.get());
        m_queueFamilies = QueueFamilies(*this, surface);
    }

    core::DynArray<core::UniquePtr<PhysicalDevice>> PhysicalDevice::getPhysicalDevices(Vulkan& vulkan) noexcept {
        core::DynArray<VkPhysicalDevice> devices = vulkan.enumerate<vkEnumeratePhysicalDevices>();
        return { devices.size(), [&](usize i) {
            return core::makeUP<PhysicalDevice>(devices[i], vulkan.surface(), PassKey { });
        }};
    }

    core::UniquePtr<PhysicalDevice> PhysicalDevice::choosePhysicalDevice(Vulkan& vulkan) {
        core::note("Choosing physical device...");
        auto devices = getPhysicalDevices(vulkan);
        core::UniquePtr<PhysicalDevice>* result = nullptr;
        if (devices.size() == 0) {
            core::fatal("No physical devices found");
            throw VulkanException { };
        }

        u64 bestScore = 0;
        for (auto& d : devices) {
            Version apiVersion = Version::fromVk(d->props().apiVersion);
            core::info(
                "Found physical device {} (id {})\n\tVulkan API version {}.{}.{}",
                d->props().deviceName,
                d->props().deviceID,
                apiVersion.major, apiVersion.minor, apiVersion.patch);
            u64 const score = evaluateDevice(*d);
            if (score == 0)
                continue; // Not suitable
            if (!result || score > bestScore) {
                result = &d;
                bestScore = score;
            }
        }

        if (!result) {
            core::fatal("No suitable physical device found");
            throw VulkanException { };
        } else {
            core::info("Chose device {} (id {})", (*result)->props().deviceName, (*result)->props().deviceID);
        }

        return std::move(*result);
    }

    void PhysicalDevice::reloadSwapchainSupport(Surface const& surface) {
        m_swapchainSupport = core::makeUP<SwapchainSupport>(m_physicalDevice, surface.get());
    }
} // namespace graphics::vulkan::internal
