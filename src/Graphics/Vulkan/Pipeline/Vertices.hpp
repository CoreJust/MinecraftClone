#pragma once
#include <Core/Container/UniquePtr.hpp>
#include <Core/Container/ArrayView.hpp>
#include "Vertex.hpp"

namespace graphics::vulkan::internal {
    class VertexBuffer;
    class CopyBuffer;
    class MappedMemory;
} // namespace graphics::vulkan::internal


namespace graphics::vulkan::pipeline {
    class VerticesBase {
    protected:
        class VerticesArrayBase {
            core::UniquePtr<internal::CopyBuffer> m_copyBuffer;
            core::UniquePtr<internal::MappedMemory> m_memory;

        public:
            VerticesArrayBase(VerticesArrayBase&&) = default;
            VerticesArrayBase(core::UniquePtr<internal::CopyBuffer> copyBuffer);
            ~VerticesArrayBase();

            core::RawMemory rawMemory() const;
        };

    protected:
        core::UniquePtr<internal::VertexBuffer> m_buffer;

    public:
        VerticesBase(VerticesBase&&) = default;
        VerticesBase(core::UniquePtr<internal::VertexBuffer> buffer);
        ~VerticesBase();

        PURE internal::VertexBuffer& buffer() { return *m_buffer; }

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
