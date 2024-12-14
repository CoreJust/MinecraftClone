#include "Mesh.h"
#include <iostream>

void Mesh::addFace(const VectorGLubyte &vertices, const VectorGLfloat &textureCoords, const VectorGLfloat &lights, 
	BlockPosition pos) {
	++m_facesCount;

	for (int i = 0, index = 0; i < 4; ++i) {
		GLuint c = 0;
		GLubyte cX = vertices[index++] + pos.x;
		GLubyte cY = vertices[index++] + (pos.y & 15);
		GLubyte cZ = vertices[index++] + pos.z;

		c |= cX;
		c |= cY << 5;
		c |= cZ << 10;

		m_vertices.push_back(c);
	}

	m_textureCoords.insert(m_textureCoords.end(), textureCoords.begin(), textureCoords.end());
	m_lights.insert(m_lights.end(), lights.begin(), lights.end());
	m_indices.insert(m_indices.end(), {
			m_indicesIndex,
			m_indicesIndex + 1,
			m_indicesIndex + 2,
			m_indicesIndex + 2,
			m_indicesIndex + 3,
			m_indicesIndex
		});

	m_indicesIndex += 4;
}

void Mesh::buffer() {
	m_model.setData(m_vertices, m_textureCoords, m_lights, m_indices);
	
	m_vertices.clear();
	m_indices.clear();
	m_textureCoords.clear();
	m_lights.clear();
}

void Mesh::reset() {
	m_vertices.clear();
	m_indices.clear();
	m_textureCoords.clear();
	m_lights.clear();

	m_facesCount = m_indicesIndex = 0;
	m_model.clear();
}

Model& Mesh::getModel() {
	return m_model;
}

GLuint Mesh::getFacesCount() const {
	return m_facesCount;
}
