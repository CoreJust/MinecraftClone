#include "Vertices.hpp"
#include "../Internal/Buffer/VertexBuffer.hpp"

namespace graphics::vulkan::pipeline {
    VerticesBase::VerticesArrayBase::VerticesArrayBase(core::UniquePtr<internal::CopyBuffer> copyBuffer)
        : m_copyBuffer(core::move(copyBuffer))
        , m_memory(core::makeUP<internal::MappedMemory>(m_copyBuffer->map()))
    { }

    VerticesBase::VerticesArrayBase::~VerticesArrayBase() = default;

    core::RawMemory VerticesBase::VerticesArrayBase::rawMemory() const {
        return m_memory->get();
    }

    VerticesBase::VerticesBase(core::UniquePtr<internal::VertexBuffer> buffer)
        : m_buffer(core::move(buffer)) { }

    VerticesBase::~VerticesBase() = default;

    VerticesBase::VerticesArrayBase VerticesBase::mapVertices() {
        return core::makeUP<internal::CopyBuffer>(m_buffer->makeCopyBuffer());
    }
} // namespace graphics::vulkan::pipeline
