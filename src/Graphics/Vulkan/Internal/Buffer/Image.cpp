#include "Image.hpp"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <Core/IO/Logger.hpp>
#include "../Vulkan/Vulkan.hpp"
#include "../Command/CommandPool.hpp"
#include "../Command/CommandBuffer.hpp"
#include "../Command/Queue.hpp"
#include "Buffer.hpp"

namespace graphics::vulkan::internal {
    Image::Image(Vulkan& vulkan, CommandPool& commandPool, core::Image& image, u32 mipLevels) 
        : m_size(image.size())
        , m_vulkan(vulkan)
        , m_mipLevels(mipLevels)
    {
        core::Vec2u32 sizeu32 = m_size.to<u32>();
        VkImageCreateInfo createInfo { };
        createInfo.sType     = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        createInfo.imageType = VK_IMAGE_TYPE_2D;
        createInfo.format    = VK_FORMAT_R8G8B8A8_SRGB;
        createInfo.extent    = VkExtent3D { .width = sizeu32.width(), .height = sizeu32.height(), .depth = 1 };
        createInfo.mipLevels = mipLevels;
        createInfo.usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        VmaAllocationCreateInfo allocCreateInfo { };
        allocCreateInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        allocCreateInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        if (!m_vulkan.safeCall<vmaCreateImage>(&createInfo, &allocCreateInfo, &m_image, &m_allocation, nullptr)) {
            core::error("Failed to create vulkan image");
            return;
        }
        
        VkImageSubresourceRange subresourceRange { };
        subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        subresourceRange.baseMipLevel   = 0;
        subresourceRange.levelCount     = m_mipLevels;
        subresourceRange.baseArrayLayer = 0;
        subresourceRange.layerCount     = 1;


        VkImageViewCreateInfo imageViewCreateInfo { };
        imageViewCreateInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        imageViewCreateInfo.image    = m_image;
        imageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        imageViewCreateInfo.format   = VK_FORMAT_R8G8B8A8_SRGB;
        imageViewCreateInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        imageViewCreateInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        imageViewCreateInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        imageViewCreateInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        imageViewCreateInfo.subresourceRange = subresourceRange;
        m_imageView = m_vulkan.create<vkCreateImageView>(&imageViewCreateInfo, nullptr);

        Buffer transferBuffer {
            m_vulkan,
            m_size.width() * m_size.height() * 4ull,
            BufferTypeBit::TransferSrc,
            AllocationTypeBit::HostAccessSequentialWrite,
            AllocationUsage::CpuToGpu,
        };
        transferBuffer.loadFrom(image.data().raw());
        
		VkImageMemoryBarrier image_barrier_to_transfer { };
		image_barrier_to_transfer.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		image_barrier_to_transfer.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
		image_barrier_to_transfer.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		image_barrier_to_transfer.image            = m_image;
		image_barrier_to_transfer.subresourceRange = subresourceRange;
		image_barrier_to_transfer.srcAccessMask    = 0;
		image_barrier_to_transfer.dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT;

        VkBufferImageCopy copyRegion { };
        copyRegion.bufferOffset      = 0;
        copyRegion.bufferRowLength   = 0;
        copyRegion.bufferImageHeight = 0;
        copyRegion.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.mipLevel       = m_mipLevels;
        copyRegion.imageSubresource.baseArrayLayer = 0;
        copyRegion.imageSubresource.layerCount     = 1;
        copyRegion.imageExtent = createInfo.extent;
        
        VkImageMemoryBarrier image_barrier_to_readable = image_barrier_to_transfer;
        image_barrier_to_readable.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        image_barrier_to_readable.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        image_barrier_to_readable.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        image_barrier_to_readable.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        commandPool.execImmediate([&](CommandBuffer& cmd) {
		    vkCmdPipelineBarrier(cmd.get(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &image_barrier_to_transfer);
	        vkCmdCopyBufferToImage(cmd.get(), transferBuffer.get(), m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
	        vkCmdPipelineBarrier(cmd.get(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &image_barrier_to_readable);
        });
    }

    Image::~Image() {
        m_vulkan.destroy<vkDestroyImageView>(m_imageView, nullptr);
    }
} // namespace graphics::vulkan::internal
