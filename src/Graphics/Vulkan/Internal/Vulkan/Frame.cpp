#include "Frame.hpp"
#include "../Vulkan/Vulkan.hpp"

namespace graphics::vulkan::internal {
    Frame::Frame(Vulkan& vulkan, Queue& graphicsQueue)
        : m_pool(vulkan, CommandPoolTypeBit::ResetCommandBuffer, graphicsQueue)
        , m_imageAvailable(vulkan)
        , m_renderingDone(vulkan)
        , m_fence(vulkan) {
        m_pool.allocate({ &m_commandBuffer, 1 });
    }

    Frame::~Frame() {
        m_pool.free({ &m_commandBuffer, 1 });
    }
} // namespace graphics::vulkan::internal
