#pragma once
#include <vulkan/vulkan.h>
#include <Core/Macro/Attributes.hpp>
#include <Core/Math/Vec.hpp>
#include <Core/Memory/UniquePtr.hpp>
#include <Core/Collection/DynArray.hpp>
#include "../Semaphore.hpp"

namespace graphics::vulkan::internal {
    struct SwapchainSupport;
    class Vulkan;
    class Queue;
    class CommandBuffer;

    class Swapchain final {
        Vulkan& m_vulkan;
        core::collection::DynArray<VkImage> m_images;
        core::collection::DynArray<VkImageView> m_imageViews;
        core::memory::UniquePtr<Queue> m_graphicsQueue;
        core::memory::UniquePtr<Queue> m_presentQueue;
        Semaphore m_imageAvailable;
        Semaphore m_renderingDone;
        VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
        VkSurfaceFormatKHR m_surfaceFormat;
        VkPresentModeKHR m_presentMode;
        VkExtent2D m_extent;
        u32 m_imageCount;

    public:
        Swapchain(Vulkan& vulkan, core::math::Vec2u32 pixelSize);
        ~Swapchain();

        // Returns -1 on failure
        PURE u32 acquireNextFrame();
        
        void submit(CommandBuffer& commandBuffer);
        void present(u32 index);

        PURE VkSwapchainKHR            get()               const noexcept { return m_swapchain; }
        PURE auto const&               imageViews()        const noexcept { return m_imageViews; }
        PURE Semaphore&                imageAvailableSemaphore() noexcept { return m_imageAvailable; }
        PURE Semaphore&                renderingDoneSemaphore()  noexcept { return m_renderingDone; }
        PURE VkSurfaceFormatKHR const& surfaceFormat()     const noexcept { return m_surfaceFormat; }
        PURE VkExtent2D const&         extent()            const noexcept { return m_extent; }
        PURE u32                  imageCount()        const noexcept { return m_imageCount; }
    };
} // namespace graphics::vulkan::internal
