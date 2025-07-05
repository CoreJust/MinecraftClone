#pragma once
#include <Core/Memory/TypeErasedObject.hpp>
#include "Instance.hpp"

namespace graphics::vulkan {
    class Device final {
        core::memory::TypeErasedObject m_physicalDevice;
        core::memory::TypeErasedObject m_logicalDevice;
        core::memory::TypeErasedObject m_graphicsQueue;

    public:
        Device() noexcept = default;
        Device(Instance& instance);
        Device(Device&&) noexcept = default;
        Device(const Device&) noexcept = delete;
        Device& operator=(Device&&) noexcept = default;
        Device& operator=(const Device&) noexcept = delete;
        ~Device();
    };
} // namespace graphics::vulkan
