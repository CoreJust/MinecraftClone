#pragma once
#include <Core/Macro/Attributes.hpp>
#include <Core/Math/Vec.hpp>
#include <Core/Memory/UniquePtr.hpp>
#include <Core/Collection/DynArray.hpp>
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
        struct Snapshot final {
            core::UniquePtr<Queue> graphicsQueue;
            core::UniquePtr<Queue> presentQueue;
            core::DynArray<Frame> frames;
            Vulkan& vulkan;
        };

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
        Swapchain(Snapshot snapshot, core::Vec2u32 pixelSize); // Recreates swapchain
        Swapchain(Vulkan& vulkan, core::Vec2u32 pixelSize, usize framesCount);
        ~Swapchain();

        Snapshot makeSnapshot();

        // Returns -1 on failure
        PURE Frame* acquireNextFrame();
        PURE Frame& currentFrame() noexcept { return m_frames[m_framesCounter % m_frames.size()]; }
        PURE u32    swapchainIndex() const noexcept { return m_swapchainIndex; }
        
        void submit();
        void present();

        PURE VkSwapchainKHR                     get()               const noexcept { return m_swapchain; }
        PURE core::DynArray<VkImageView> const& imageViews()        const noexcept { return m_imageViews; }
        PURE SwapchainFormat             const& format()            const noexcept { return *m_format; }
        PURE u32                                imageCount()        const noexcept { return m_imageCount; }

    private:
        void initialize(core::Vec2u32 pixelSize);
    };
} // namespace graphics::vulkan::internal
