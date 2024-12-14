#include "Model.h"
#include <glm\gtx\rotate_vector.hpp>
#include <iostream>

Model::Model(const VectorGLfloat &vertices, const VectorGLfloat &textureCoords, const VectorGLfloat &lights, const VectorGLuint &indices)
	: m_indicesCount(indices.size()) {
	glGenVertexArrays(1, &m_vao);
	glBindVertexArray(m_vao);

	addVBO(vertices, 3);
	addVBO(textureCoords, 2);
	addVBO(lights, 1);
	addEBO(indices);
}

Model::Model(const VectorGLfloat &vertices, const VectorGLfloat &textureCoords, const VectorGLuint &indices)
	: m_indicesCount(indices.size()) {
	glGenVertexArrays(1, &m_vao);
	glBindVertexArray(m_vao);

	addVBO(vertices, 3);
	addVBO(textureCoords, 2);
	addEBO(indices);
}

Model::Model(Model &&other) 
	: m_vao(other.m_vao), m_vboCount(other.m_vboCount), m_indicesCount(other.m_indicesCount),
	m_buffers(std::move(other.m_buffers)) {
	other.m_vao = other.m_vboCount = other.m_indicesCount = 0;
	other.m_buffers.clear();
}

Model& Model::operator=(Model &&other) {
	m_vao = other.m_vao;
	m_vboCount = other.m_vboCount;
	m_indicesCount = other.m_indicesCount;
	m_buffers = std::move(other.m_buffers);

	other.m_vao = other.m_vboCount = other.m_indicesCount = 0;
	other.m_buffers.clear();

	return *this;
}

Model::~Model() {
	clear();
}

void Model::setData(const VectorGLfloat &vertices, const VectorGLfloat &textureCoords, const VectorGLfloat &lights, const VectorGLuint &indices) {
	clear();

	m_indicesCount = indices.size();
	glGenVertexArrays(1, &m_vao);
	glBindVertexArray(m_vao);

	addVBO(vertices, 3);
	addVBO(textureCoords, 2);
	addVBO(lights, 1);
	addEBO(indices);
}

void Model::setData(const VectorGLuint &vertices, const VectorGLfloat &textureCoords, const VectorGLfloat &lights, const VectorGLuint &indices) {
	clear();

	m_indicesCount = indices.size();
	glGenVertexArrays(1, &m_vao);
	glBindVertexArray(m_vao);

	addVBO(vertices, 1);
	addVBO(textureCoords, 2);
	addVBO(lights, 1);
	addEBO(indices);
}

void Model::addVBO(const VectorGLfloat &data, int dim) {
	GLuint vbo;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(GLfloat), data.data(), GL_STATIC_DRAW);
	glVertexAttribPointer(m_vboCount, dim, GL_FLOAT, GL_FALSE, 0, (GLvoid*)0);
	glEnableVertexAttribArray(m_vboCount++);
	m_buffers.push_back(vbo);
}

void Model::addVBO(const VectorGLuint &data, int dim) {
	GLuint vbo;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(GLuint), data.data(), GL_STATIC_DRAW);
	glVertexAttribIPointer(m_vboCount, dim, GL_UNSIGNED_INT, 0, (GLvoid*)0);
	glEnableVertexAttribArray(m_vboCount++);
	m_buffers.push_back(vbo);
}

void Model::addEBO(const VectorGLuint &indices) {
	GLuint ebo;
	glGenBuffers(1, &ebo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(GLuint), indices.data(), GL_STATIC_DRAW);

	m_buffers.push_back(ebo);
}

void Model::clear() {
	if (m_buffers.size()) {
		glDeleteBuffers(m_buffers.size(), m_buffers.data());
		m_buffers.clear();

		glDeleteVertexArrays(1, &m_vao);
	}
		

	m_vao = 0;
	m_vboCount = 0;
	m_indicesCount = 0;
}

void Model::bind() const {
	glBindVertexArray(m_vao);
}

void Model::unbind() const {
	glBindVertexArray(0);
}

GLuint Model::getVAO() const {
	return m_vao;
}

GLuint Model::getIndicesCount() const {
	return m_indicesCount;
}

VectorGLfloat Model::getBlockVertices(BlockPos pos) {
	VectorGLfloat vertices {
		//Back
		1 + 0.01f,   -0.01f,      -0.01f,
		-0.01f,      -0.01f,      -0.01f,
		-0.01f,      1 + 0.01f,   -0.01f,
		1 + 0.01f,   1 + 0.01f,   -0.01f,

		//Right-Side
		1 + 0.01f, -0.01f,        1 + 0.01f,
		1 + 0.01f, -0.01f,        -0.01f,
		1 + 0.01f, 1 + 0.01f,     -0.01f,
		1 + 0.01f, 1 + 0.01f,     1 + 0.01f,

		//Front
		-0.01f,      -0.01f,      1 + 0.01f,
		1 + 0.01f,   -0.01f,      1 + 0.01f,
		1 + 0.01f,   1 + 0.01f,   1 + 0.01f,
		-0.01f,      1 + 0.01f,   1 + 0.01f,

		//Left
		-0.01f, -0.01f,           -0.01f,
		-0.01f, -0.01f,           1 + 0.01f,
		-0.01f, 1 + 0.01f,        1 + 0.01f,
		-0.01f, 1 + 0.01f,        -0.01f,

		//Top
		-0.01f,      1 + 0.01f,   1 + 0.01f,
		1 + 0.01f,   1 + 0.01f,   1 + 0.01f,
		1 + 0.01f,   1 + 0.01f,   -0.01f,
		-0.01f,      1 + 0.01f,   -0.01f,

		//Bottom
		-0.01f,      -0.01f,      -0.01f,
		1 + 0.01f,   -0.01f,      -0.01f,
		1 + 0.01f,   -0.01f,      1 + 0.01f,
		-0.01f,      -0.01f,      1 + 0.1f
	};

	for (int i = 0; i < vertices.size(); i += 3) {
		vertices[i] += pos.x;
		vertices[i + 1] += pos.y;
		vertices[i + 2] += pos.z;
	}

	return vertices;
}

VectorGLfloat Model::getBoxVertices(const glm::vec3 &size, const glm::vec3 &pos, const glm::vec3 &rot, const glm::vec3 &rotCoords) {
	static auto makeRotation = [](glm::vec3 &r, const glm::vec3 &rot) -> void {
		r = glm::rotateX(r, glm::radians(rot.x));
		r = glm::rotateY(r, glm::radians(rot.y));
		r = glm::rotateZ(r, glm::radians(rot.z));
	};

	VectorGLfloat vertices {
		//Back
		size.x, 0.f,    0.f,
		0.f,    0.f,    0.f,
		0.f,    size.y, 0.f,
		size.x, size.y, 0.f,

		//Right-Side
		size.x, 0.f,    size.z,
		size.x, 0.f,    0.f,
		size.x, size.y, 0.f,
		size.x, size.y, size.z,

		//Front
		0.f,    0.f,    size.z,
		size.x, 0.f,    size.z,
		size.x, size.y, size.z,
		0.f,    size.y, size.z,

		//Left
		0.f, 0.f,    0.f,
		0.f, 0.f,    size.z,
		0.f, size.y, size.z,
		0.f, size.y, 0.f,

		//Top
		0.f,    size.y, size.z,
		size.x, size.y, size.z,
		size.x, size.y, 0.f,
		0.f,    size.y, 0.f,

		//Bottom
		0.f,    0.f, 0.f,
		size.x, 0.f, 0.f,
		size.x, 0.f, size.z,
		0.f,    0.f, size.z
	};

	for (int i = 0; i < vertices.size(); i += 3) {
		glm::vec3 vec{ vertices[i], vertices[i + 1], vertices[i + 2] };
		vec -= rotCoords;
		makeRotation(vec, rot);
		vec += rotCoords;

		vertices[i] = vec.x + pos.x;
		vertices[i + 1] = vec.y + pos.y;
		vertices[i + 2] = vec.z + pos.z;
	}

	return vertices;
}

VectorGLfloat Model::getBlockLights() {
	static VectorGLfloat lights {
		16.f, 16.f, 16.f, 16.f,
		16.f, 16.f, 16.f, 16.f,
		16.f, 16.f, 16.f, 16.f,
		16.f, 16.f, 16.f, 16.f,
		16.f, 16.f, 16.f, 16.f,
		16.f, 16.f, 16.f, 16.f
	};

	return lights;
}

VectorGLuint Model::getBlockIndices() {
	VectorGLuint indices;

	for (unsigned int i = 0, index = 0; i < 6; ++i) {
		indices.insert(indices.end(), {
			index, index + 1, index + 2,
			index + 2, index + 3, index
			});
		index += 4;
	}

	return indices;
}

VectorGLuint Model::getBoxIndices(unsigned int &index) {
	VectorGLuint indices;

	for (unsigned int i = 0; i < 6; ++i) {
		indices.insert(indices.end(), {
			index, index + 1, index + 2,
			index + 2, index + 3, index
			});
		index += 4;
	}

	return indices;
}
