#pragma once
#include <Core/Memory/TypeErasedObject.hpp>

namespace graphics::vulkan {
    class Instance final {
        core::memory::TypeErasedObject m_instance;
        core::memory::TypeErasedObject m_debugMessenger;

    public:
        Instance() noexcept = default;
        Instance(char const* const appName, char const** windowRequiredExtensions, uint32_t windowRequiredExtensionsCount);
        Instance(Instance&&) noexcept = default;
        Instance(const Instance&) noexcept = delete;
        Instance& operator=(Instance&&) noexcept = default;
        Instance& operator=(const Instance&) noexcept = delete;
        ~Instance();
    };
} // namespace graphics::vulkan
