#include "Vertices.hpp"
#include "../Internal/Buffer/Buffer.hpp"

namespace graphics::vulkan::pipeline {
    VerticesBase::VerticesArrayBase::VerticesArrayBase(core::UniquePtr<internal::MappedMemory> memory)
        : m_memory(core::move(memory)) { }

    VerticesBase::VerticesArrayBase::~VerticesArrayBase() = default;

    core::RawMemory VerticesBase::VerticesArrayBase::rawMemory() const {
        return m_memory->get();
    }

    VerticesBase::VerticesBase(core::UniquePtr<internal::Buffer> buffer)
        : m_buffer(core::move(buffer)) { }

    VerticesBase::~VerticesBase() = default;

    VerticesBase::VerticesArrayBase VerticesBase::mapVertices() {
        return core::makeUP<internal::MappedMemory>(m_buffer->map());
    }
} // namespace graphics::vulkan::pipeline
