#pragma once
#include <Core/Container/UniquePtr.hpp>
#include <Core/Container/ArrayView.hpp>
#include "Vertex.hpp"

namespace graphics::vulkan::internal {
    class VertexBuffer;
    class IndexBuffer;
    class CopyBuffer;
    class MappedMemory;
} // namespace graphics::vulkan::internal


namespace graphics::vulkan::pipeline {
    class VerticesBase {
    protected:
        class VulkanArrayBase {
            core::UniquePtr<internal::CopyBuffer> m_copyBuffer;
            core::UniquePtr<internal::MappedMemory> m_memory;

        public:
            VulkanArrayBase(VulkanArrayBase&&) = default;
            VulkanArrayBase(core::UniquePtr<internal::CopyBuffer> copyBuffer);
            ~VulkanArrayBase();

            core::RawMemory rawMemory() const;
        };

    protected:
        core::UniquePtr<internal::VertexBuffer> m_vertexBuffer;
        core::UniquePtr<internal::IndexBuffer>  m_indexBuffer;

    public:
        VerticesBase(VerticesBase&&) = default;
        VerticesBase(core::UniquePtr<internal::VertexBuffer> vertexBuffer, core::UniquePtr<internal::IndexBuffer> indexBuffer);
        ~VerticesBase();

        PURE internal::VertexBuffer& vertexBuffer() { return *m_vertexBuffer; }
        PURE internal::IndexBuffer*  indexBuffer()  { return m_indexBuffer.get(); }

    protected:
        PURE VulkanArrayBase mapVertices();
        PURE VulkanArrayBase mapIndices();
        void loadVertices(core::RawMemory vertices);
        void loadIndices (core::RawMemory indices);
    };

    template<VertexConcept Vertex, typename Index>
    class Vertices final : public VerticesBase {
    public:
        class VerticesArray final : public VulkanArrayBase {
        public:
            VerticesArray(VulkanArrayBase base) : VulkanArrayBase(core::move(base)) { }

            core::ArrayView<Vertex> arrayView() const {
                return core::ArrayView<Vertex>(core::RawArray<sizeof(Vertex)>(VulkanArrayBase::rawMemory()));
            }
        };

        class IndicesArray final : public VulkanArrayBase {
        public:
            IndicesArray(VulkanArrayBase base) : VulkanArrayBase(core::move(base)) { }

            core::ArrayView<Index> arrayView() const {
                return core::ArrayView<Index>(core::RawArray<sizeof(Index)>(VulkanArrayBase::rawMemory()));
            }
        };

    public:
        Vertices(VerticesBase&& base) : VerticesBase(core::move(base)) { }

        // Note: it is best to use loadXXX rather than mapXXX if the data is already generated.
        VerticesArray mapVertices() { return VerticesBase::mapVertices(); }
        IndicesArray  mapIndices () { return VerticesBase::mapIndices (); }
        void loadVertices(core::ArrayView<Vertex> vertices) { return VerticesBase::loadVertices(vertices); }
        void loadIndices (core::ArrayView<Index>  indices)  { return VerticesBase::loadIndices (indices); }
    };
} // namespace graphics::vulkan::pipeline
