#include "Vertices.hpp"
#include "../Internal/Buffer/VertexBuffer.hpp"
#include "../Internal/Buffer/IndexBuffer.hpp"

namespace graphics::vulkan::pipeline {
    VerticesBase::VulkanArrayBase::VulkanArrayBase(core::UniquePtr<internal::CopyBuffer> copyBuffer)
        : m_copyBuffer(core::move(copyBuffer))
        , m_memory(core::makeUP<internal::MappedMemory>(m_copyBuffer->map()))
    { }

    VerticesBase::VulkanArrayBase::~VulkanArrayBase() = default;

    core::RawMemory VerticesBase::VulkanArrayBase::rawMemory() const {
        return m_memory->get();
    }

    VerticesBase::VerticesBase(core::UniquePtr<internal::VertexBuffer> vertexBuffer, core::UniquePtr<internal::IndexBuffer> indexBuffer)
        : m_vertexBuffer(core::move(vertexBuffer))
        , m_indexBuffer (core::move(indexBuffer))
    { }

    VerticesBase::~VerticesBase() = default;

    VerticesBase::VulkanArrayBase VerticesBase::mapVertices() {
        return core::makeUP<internal::CopyBuffer>(m_vertexBuffer->makeCopyBuffer());
    }
    
    VerticesBase::VulkanArrayBase VerticesBase::mapIndices() {
        ASSERT(m_indexBuffer);
        return core::makeUP<internal::CopyBuffer>(m_indexBuffer->makeCopyBuffer());
    }
} // namespace graphics::vulkan::pipeline
