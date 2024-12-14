#include "Vertices.hpp"
#include <GL/glew.h>
#include <Core/Common/Assert.hpp>

core::gl::Vertices::Vertices(Vertices&& other) noexcept
	: m_bufferIds(std::move(other.m_bufferIds))
	, m_verticesCount(std::exchange(other.m_verticesCount, 0))
	, m_vaoId(std::exchange(other.m_vaoId, 0))
	{ }

core::gl::Vertices::Vertices(std::vector<AttributeType> attrs, Type indicesType, size_t verticesCount, size_t bufferVerticesCount)
	: m_attrs(std::move(attrs)), m_bufferVerticesCount(bufferVerticesCount), m_verticesCount(verticesCount), m_indicesType(indicesType) {
	ASSERT(!isFloat(indicesType), "Indices must be of integral type");
	m_bufferIds.resize(m_attrs.size() + 1);

	glGenVertexArrays(1, &m_vaoId);
	glBindVertexArray(m_vaoId);

	glGenBuffers(m_bufferIds.size(), m_bufferIds.data());
	size_t index = 0;
	for (const auto& attr : m_attrs) {
		glBindBuffer(GL_ARRAY_BUFFER, m_bufferIds[index]);
		glBufferData(GL_ARRAY_BUFFER, m_bufferVerticesCount * typeSize(attr.type), nullptr, GL_DYNAMIC_DRAW);
		if (isFloat(attr.type)) {
			glVertexAttribPointer(index, 1, toRawGL(attr.type), attr.isNormalized, 0, (GLvoid*)0);
		} else {
			glVertexAttribIPointer(index, 1, toRawGL(attr.type), 0, (GLvoid*)0);
		}
		glEnableVertexAttribArray(index++);
	}

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_bufferIds.back());
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_verticesCount * typeSize(m_indicesType), nullptr, GL_DYNAMIC_DRAW);
}

core::gl::Vertices::~Vertices() {
	if (m_vaoId) {
		glDeleteBuffers(m_bufferIds.size(), m_bufferIds.data());
		glDeleteVertexArrays(1, &m_vaoId);
		m_bufferIds.clear();
		m_vaoId = 0;
		m_verticesCount = 0;
	}
}

void core::gl::Vertices::render() {
	ASSERT(m_vaoId, "Vertices must be initialized before rendering");
	glBindVertexArray(m_vaoId);
	glDrawElements(GL_TRIANGLES, m_verticesCount, GL_UNSIGNED_INT, nullptr);
}

void core::gl::Vertices::pushBufferImpl(size_t idx, void* data, size_t size, size_t offset) {
	ASSERT(m_vaoId, "Vertices must be initialized before loading buffers");
	ASSERT(idx < m_bufferIds.size() - 1, "Buffer index out of bounds");
	ASSERT(m_bufferVerticesCount * typeSize(m_attrs[idx].type) <= size - offset, "Too much vertices or too large of an offset provided");
	glBindBuffer(GL_ARRAY_BUFFER, m_bufferIds[idx]);
	glBufferSubData(GL_ARRAY_BUFFER, offset, size, data);
}

void core::gl::Vertices::pushIndicesImpl(void* data, size_t size, size_t offset) {
	ASSERT(m_vaoId, "Vertices must be initialized before loading indices");
	ASSERT(m_verticesCount * typeSize(m_indicesType) == size - offset, "Too much vertices or too large of an offset provided");
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_bufferIds.back());
	glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, offset, size, data);
}
