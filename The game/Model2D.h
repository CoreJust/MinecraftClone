#pragma once
#include <vector>
#include <gl\glew.h>
#include "Defs.h"

class Model2D final {
public:
	Model2D() = default;
	Model2D(VectorGLfloat &vertices, VectorGLfloat &textureCoords);

	Model2D(const Model2D& other) = delete;
	Model2D& operator=(const Model2D& other) = delete;

	Model2D(Model2D&& other);
	Model2D& operator=(Model2D&& other);

	~Model2D();

	void setData(VectorGLfloat &vertices, VectorGLfloat &textureCoords);
	void addVBO(VectorGLfloat &data, int dim);

	void clear();

	void bind() const;
	void unbind() const;

	GLuint getVAO() const;
	GLuint getVerticesCount() const;

private:
	std::vector<GLuint> m_buffers;
	GLuint m_vao = 0;
	GLuint m_vboCount = 0;
	GLuint m_verticesCount = 0;

public:
	static VectorGLfloat stdTextureCoords;
};