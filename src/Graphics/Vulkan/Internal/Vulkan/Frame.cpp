#include "Frame.hpp"
#include "../Vulkan/Vulkan.hpp"

namespace graphics::vulkan::internal {
    Frame::Frame(Vulkan& vulkan)
        : m_pool(vulkan, vulkan.queueFamilies().getIndex(QueueType::Graphics))
        , m_imageAvailable(vulkan)
        , m_renderingDone(vulkan)
        , m_fence(vulkan) {
        m_pool.allocate({ &m_commandBuffer, 1 });
    }
} // namespace graphics::vulkan::internal
