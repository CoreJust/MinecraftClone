#include "Model2D.h"

VectorGLfloat Model2D::stdTextureCoords = {
	0.f, 1.f,
	0.f, 0.f,
	1.f, 1.f,

	1.f, 0.f,
	0.f, 0.f,
	1.f, 1.f
};

Model2D::Model2D(VectorGLfloat &vertices, VectorGLfloat &textureCoords)
	: m_verticesCount(vertices.size() / 2) {
	glGenVertexArrays(1, &m_vao);
	glBindVertexArray(m_vao);

	addVBO(vertices, 2);
	addVBO(textureCoords, 2);
}

Model2D::Model2D(Model2D &&other)
	: m_vao(other.m_vao), m_vboCount(other.m_vboCount), m_verticesCount(other.m_verticesCount), m_buffers(std::move(other.m_buffers)) {
	other.m_vao = other.m_vboCount = 0;
	other.m_buffers.clear();
}

Model2D& Model2D::operator=(Model2D &&other) {
	m_vao = other.m_vao;
	m_vboCount = other.m_vboCount;
	m_verticesCount = other.m_verticesCount;
	m_buffers = std::move(other.m_buffers);

	other.m_vao = other.m_vboCount = other.m_verticesCount = 0;
	other.m_buffers.clear();

	return *this;
}

Model2D::~Model2D() {
	clear();
}

void Model2D::setData(VectorGLfloat &vertices, VectorGLfloat &textureCoords) {
	clear();

	m_verticesCount = static_cast<GLuint>(vertices.size()) / 2;
	glGenVertexArrays(1, &m_vao);
	glBindVertexArray(m_vao);

	addVBO(vertices, 3);
	addVBO(textureCoords, 2);
}

void Model2D::addVBO(VectorGLfloat &data, int dim) {
	GLuint vbo;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(GLfloat), data.data(), GL_STATIC_DRAW);
	glVertexAttribPointer(m_vboCount, dim, GL_FLOAT, GL_FALSE, 0, (GLvoid*)0);
	glEnableVertexAttribArray(m_vboCount++);
	m_buffers.push_back(vbo);
}

void Model2D::clear() {
	if (m_buffers.size()) {
		glDeleteBuffers(m_buffers.size(), m_buffers.data());
		m_buffers.clear();
	}

	if (m_vao)
		glDeleteVertexArrays(1, &m_vao);

	m_vao = 0;
	m_vboCount = 0;
	m_verticesCount = 0;
}

void Model2D::bind() const {
	glBindVertexArray(m_vao);
}

void Model2D::unbind() const {
	glBindVertexArray(0);
}

GLuint Model2D::getVAO() const {
	return m_vao;
}

GLuint Model2D::getVerticesCount() const {
	return m_verticesCount;
}
