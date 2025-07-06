#include "PhysicalDevice.hpp"
#include <Core/IO/Logger.hpp>
#include "Instance.hpp"
#include "Surface.hpp"
#include "../Version.hpp"
#include "../Exception.hpp"

namespace graphics::vulkan::internal {
namespace {
    uint64_t evaluateDevice(PhysicalDevice& d) {
        if (!d.queueFamilies().hasFamily(QueueType::Graphics)
            || !d.queueFamilies().hasFamily(QueueType::Present)
            || !d.extensions().hasExtension(VulkanExtension::Swapchain)
            || d.swapchainSupport().formats.empty()
            || d.swapchainSupport().presentModes.empty())
            return 0;

        return d.props().apiVersion;
    }
} // namespace


    PhysicalDevice::PhysicalDevice(VkPhysicalDevice device, Surface& surface) noexcept
        : m_physicalDevice(device)
        , m_extensions(*this)
        , m_swapchainSupport(m_physicalDevice, surface.get()) {
        vkGetPhysicalDeviceProperties(m_physicalDevice, &m_properties);
        m_queueFamilies = QueueFamilies(m_physicalDevice, surface);
    }

    std::vector<PhysicalDevice> PhysicalDevice::getPhysicalDevices(Instance& instance, Surface& surface) noexcept {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance.get(), &deviceCount, nullptr);
        if (deviceCount == 0)
            return { };

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance.get(), &deviceCount, devices.data());

        std::vector<PhysicalDevice> result;
        result.reserve(deviceCount);
        for (const VkPhysicalDevice& d : devices)
            result.push_back(PhysicalDevice { d, surface });
        return result;
    }

    PhysicalDevice PhysicalDevice::choosePhysicalDevice(Instance& instance, Surface& surface) {
        core::io::info("Choosing physical device...");
        std::vector<PhysicalDevice> devices = getPhysicalDevices(instance, surface);
        PhysicalDevice* result = nullptr;
        if (devices.empty()) {
            core::io::fatal("No physical devices found");
            throw VulkanException { };
        }

        uint64_t bestScore = 0;
        for (PhysicalDevice& d : devices) {
            Version apiVersion = Version::fromVk(d.props().apiVersion);
            core::io::info(
                "Found physical device {} (id {})\n\tVulkan API version {}.{}.{}",
                d.props().deviceName,
                d.props().deviceID,
                apiVersion.major, apiVersion.minor, apiVersion.patch);
            uint64_t const score = evaluateDevice(d);
            if (score == 0)
                continue; // Not suitable
            if (!result || score > bestScore) {
                result = &d;
                bestScore = score;
            }
        }

        if (!result) {
            core::io::fatal("No suitable physical device found");
            throw VulkanException { };
        } else {
            core::io::info("Chose device {} (id {})", result->props().deviceName, result->props().deviceID);
        }

        if (auto bestVersion = Version::fromVk(result->props().apiVersion); bestVersion < getVkVersion()) {
            core::io::warn(
                "Vulkan version downgraded: {}.{}.{} -> {}.{}.{}",
                getVkVersion().major, getVkVersion().minor, getVkVersion().patch,
                bestVersion.major, bestVersion.minor, bestVersion.patch);
            downgradeVkVersion(bestVersion);
        }

        return std::move(*result);
    }
} // namespace graphics::vulkan::internal
