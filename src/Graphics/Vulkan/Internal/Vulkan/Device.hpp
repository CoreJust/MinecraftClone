#pragma once
#include <Core/Macro/Attributes.hpp>
#include "../Wrapper/Handles.hpp"
#include "PhysicalDevice.hpp"

namespace graphics::vulkan::internal {
    class Device final {
        VkDevice m_logicalDevice = VK_NULL_HANDLE;

    public:
        Device() noexcept = default;
        Device(PhysicalDevice const& physicalDevice);
        Device(Device&&) noexcept = default;
        Device(const Device&) noexcept = delete;
        Device& operator=(Device&&) &noexcept = default;
        Device& operator=(const Device&) noexcept = delete;
        ~Device();

        void waitIdle() const;

        PURE VkDevice get() const noexcept { return m_logicalDevice; }
    };
} // namespace graphics::vulkan::internal
