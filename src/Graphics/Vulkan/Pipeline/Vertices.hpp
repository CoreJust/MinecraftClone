#pragma once
#include <Core/Memory/UniquePtr.hpp>
#include <Core/Collection/ArrayView.hpp>
#include "Vertex.hpp"

namespace graphics::vulkan::internal {
    class Buffer;
    class MappedMemory;
} // namespace graphics::vulkan::internal


namespace graphics::vulkan::pipeline {
    class VerticesBase {
    protected:
        class VerticesArrayBase {
            core::UniquePtr<internal::MappedMemory> m_memory;

        public:
            VerticesArrayBase(VerticesArrayBase&&) = default;
            VerticesArrayBase(core::UniquePtr<internal::MappedMemory> memory);
            ~VerticesArrayBase();

            core::RawMemory rawMemory() const;
        };

    protected:
        core::UniquePtr<internal::Buffer> m_buffer;

    public:
        VerticesBase(VerticesBase&&) = default;
        VerticesBase(core::UniquePtr<internal::Buffer> buffer);
        ~VerticesBase();

        PURE internal::Buffer& buffer() { return *m_buffer; }

    protected:
        PURE VerticesArrayBase mapVertices();
    };

    template<VertexConcept Vertex>
    class Vertices final : public VerticesBase {
    public:
        class VerticesArray final : public VerticesArrayBase {
        public:
            VerticesArray(VerticesArrayBase base) : VerticesArrayBase(core::move(base)) { }

            core::ArrayView<Vertex> arrayView() const {
                return core::ArrayView<Vertex>(core::RawArray<sizeof(Vertex)>(VerticesArrayBase::rawMemory()));
            }
        };

    public:
        Vertices(VerticesBase&& base) : VerticesBase(core::move(base)) { }

        VerticesArray vertices() { return VerticesBase::mapVertices(); }
    };
} // namespace graphics::vulkan::pipeline
