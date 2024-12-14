#pragma once
#include <vector>
#include <span>
#include "Type.hpp"

namespace core::gl {
	class Vertices final {
		std::vector<UID> m_bufferIds;
		std::vector<AttributeType> m_attrs;
		size_t m_bufferVerticesCount = 0;
		size_t m_verticesCount = 0;
		UID m_vaoId = 0;
		Type m_indicesType;

	public:
		Vertices(Vertices&&) noexcept;
		Vertices(std::vector<AttributeType> attrs, Type indicesType, size_t verticesCount, size_t bufferVerticesCount);
		~Vertices();

		void render();

		template<class T>
		void pushBuffers(size_t idx, std::span<T> data, size_t offset = 0) {
			pushBufferImpl(idx, reinterpret_cast<void*>(data.data()), data.size_bytes(), offset);
		}

		template<class T>
		void pushIndices(std::span<T> indices, size_t offset = 0) {
			pushIndicesImpl(reinterpret_cast<void*>(indices.data()), indices.size_bytes(), offset);
		}

	private:
		void pushBufferImpl(size_t idx, void* data, size_t size, size_t offset);
		void pushIndicesImpl(void* data, size_t size, size_t offset);
	};
}
