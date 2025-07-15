#include "PhysicalDevice.hpp"
#include <Core/IO/Logger.hpp>
#include "Vulkan.hpp"
#include "../../Version.hpp"
#include "../../Exception.hpp"

namespace graphics::vulkan::internal {
namespace {
    u64 evaluateDevice(PhysicalDevice& d) {
        if (!d.queueFamilies().hasFamily(QueueType::Graphics)
            || !d.queueFamilies().hasFamily(QueueType::Present)
            || !d.extensions().hasExtension(VulkanExtension::Swapchain)
            || d.swapchainSupport().formats.size() == 0
            || d.swapchainSupport().presentModes.size() == 0
            || Version::fromVk(d.props().apiVersion) < getVkVersion())
            return 0;

        return 1;
    }
} // namespace


    PhysicalDevice::PhysicalDevice(VkPhysicalDevice device, Surface const& surface, PassKey) noexcept
        : m_physicalDevice(device)
        , m_extensions(*this)
        , m_swapchainSupport(m_physicalDevice, surface.get()) {
        vkGetPhysicalDeviceProperties(m_physicalDevice, &m_properties);
        m_queueFamilies = QueueFamilies(*this, surface);
    }
        
    Version PhysicalDevice::getHighestPhysicalDeviceVersion(Instance& instance) noexcept {
        u32 deviceCount = 0;
        vkEnumeratePhysicalDevices(instance.get(), &deviceCount, nullptr);
        if (deviceCount == 0)
            return { };

        core::DynArray<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance.get(), &deviceCount, devices.data());

        Version result;
        for (const VkPhysicalDevice& d : devices) {
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(d, &properties);
            Version apiVersion = Version::fromVk(properties.apiVersion);
            if (result < apiVersion)
                result = apiVersion;
        }
        return result;
    }

    core::DynArray<core::UniquePtr<PhysicalDevice>> PhysicalDevice::getPhysicalDevices(Vulkan& vulkan) noexcept {
        core::DynArray<VkPhysicalDevice> devices = vulkan.enumerate<vkEnumeratePhysicalDevices>();
        return { devices.size(), [&](usize i) { 
            return core::makeUP<PhysicalDevice>(devices[i], vulkan.surface(), PassKey { });
        }};
    }

    core::UniquePtr<PhysicalDevice> PhysicalDevice::choosePhysicalDevice(Vulkan& vulkan) {
        core::info("Choosing physical device...");
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
} // namespace graphics::vulkan::internal
