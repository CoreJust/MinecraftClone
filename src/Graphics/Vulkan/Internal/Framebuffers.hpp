#pragma once
#include <Core/Macro/Attributes.hpp>
#include <Core/Collection/DynArray.hpp>
#include "Wrapper/Handles.hpp"

namespace graphics::vulkan::internal {
    class Vulkan;
    class RenderPass;

    class Framebuffers final {
        core::DynArray<VkFramebuffer> m_buffers;
        Vulkan& m_vulkan;

    public:
        Framebuffers(Vulkan& vulkan, RenderPass& renderPass);
        ~Framebuffers();

        PURE core::DynArray<VkFramebuffer> const& get() const noexcept { return m_buffers; }
    };
} // namespace graphics::vulkan::internal
