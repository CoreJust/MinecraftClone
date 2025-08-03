#pragma once
#include <Core/Container/Image.hpp>
#include "../Wrapper/Handles.hpp"

namespace graphics::vulkan::internal {
    class Vulkan;
    class CommandPool;

    class Image final {
        VkImage m_image;
        VkImageView m_imageView;
        core::Vec2<usize> m_size;
        VmaAllocation m_allocation;
        Vulkan& m_vulkan;
        u32 m_mipLevels;

    public:
        Image(Vulkan& vulkan, CommandPool& commandPool, core::Image& image, u32 mipLevels = 0);
        ~Image();

        PURE VkImage                  get      () const noexcept { return m_image; }
        PURE core::Vec2<usize> const& size     () const noexcept { return m_size; }
        PURE u32                      mipLevels() const noexcept { return m_mipLevels; }
    };
} // namespace graphics::vulkan::internal
