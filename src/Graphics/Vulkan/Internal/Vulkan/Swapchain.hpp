#pragma once
#include <Core/Macro/Attributes.hpp>
#include <Core/Math/Vec.hpp>
#include <Core/Container/UniquePtr.hpp>
#include <Core/Container/DynArray.hpp>
#include "../Wrapper/Handles.hpp"
#include "Frame.hpp"

namespace graphics::vulkan::internal {
    struct SwapchainSupport;
    class Vulkan;
    class Queue;
    class CommandBuffer;
    class Semaphore;
    struct SwapchainFormat;

    class Swapchain final {
        Vulkan& m_vulkan;
        core::DynArray<VkImage> m_images;
        core::DynArray<VkImageView> m_imageViews;
        core::UniquePtr<Queue> m_graphicsQueue;
        core::UniquePtr<Queue> m_presentQueue;
        core::DynArray<Frame> m_frames;
        core::DynArray<Fence*> m_swapchainFences;
        VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
        core::UniquePtr<SwapchainFormat> m_format;
        u64 m_framesCounter = 0;
        u32 m_swapchainIndex = static_cast<u32>(-1);
        u32 m_imageCount;

    public:
        Swapchain(Vulkan& vulkan, core::Vec2u32 pixelSize, usize framesCount);
        ~Swapchain();

        // Returns -1 on failure
        PURE Frame* acquireNextFrame();
        PURE Frame& currentFrame    ()       noexcept { return m_frames[m_framesCounter % m_frames.size()]; }
        PURE u32    swapchainIndex  () const noexcept { return m_swapchainIndex; }
        
        void submit();
        void present();

        PURE Queue                            & graphicsQueue()       noexcept { return *m_graphicsQueue; }
        PURE VkSwapchainKHR                     get          () const noexcept { return m_swapchain; }
        PURE core::DynArray<VkImageView> const& imageViews   () const noexcept { return m_imageViews; }
        PURE SwapchainFormat             const& format       () const noexcept { return *m_format; }
        PURE u32                                imageCount   () const noexcept { return m_imageCount; }
        PURE u32                                frameCount   () const noexcept { return static_cast<u32>(m_frames.size()); }

    private:
        void initialize(core::Vec2u32 pixelSize);
    };
} // namespace graphics::vulkan::internal
