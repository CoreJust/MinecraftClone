#pragma once
#include <vector>
#include <gl\glew.h>
#include <glm\glm.hpp>
#include "Defs.h"
#include "Maths\BlockPos.h"

class Model {
public:
	Model() = default;
	Model(const VectorGLfloat &vertices, const VectorGLfloat &textureCoords, const VectorGLfloat &lights, const VectorGLuint &indices);
	Model(const VectorGLfloat &vertices, const VectorGLfloat &textureCoords, const VectorGLuint &indices);

	Model(const Model& other) = delete;
	Model& operator=(const Model& other) = delete;

	Model(Model&& other);
	Model& operator=(Model&& other);

	~Model();

	void setData(const VectorGLfloat &vertices, const VectorGLfloat &textureCoords, const VectorGLfloat &lights, const VectorGLuint &indices);
	void setData(const VectorGLuint &vertices, const VectorGLfloat &textureCoords, const VectorGLfloat &lights, const VectorGLuint &indices);

	void addVBO(const VectorGLfloat &data, int dim);
	void addVBO(const VectorGLuint &data, int dim);
	void addEBO(const VectorGLuint &indices);

	void clear();

	void bind() const;
	void unbind() const;

	GLuint getVAO() const;
	GLuint getIndicesCount() const;

public:
	static VectorGLfloat getBlockVertices(BlockPos pos = { 0, 0, 0 });
	static VectorGLfloat getBoxVertices(const glm::vec3 &size, const glm::vec3 &pos = { 0, 0, 0 }, const glm::vec3 &rot = { 0, 0, 0 },
										const glm::vec3 &rotCoords = { 0, 0, 0 });
	static VectorGLfloat getBlockLights();
	static VectorGLuint getBlockIndices();
	static VectorGLuint getBoxIndices(unsigned int &index);

private:
	std::vector<GLuint> m_buffers;
	GLuint m_vao = 0;
	GLuint m_vboCount = 0;
	GLuint m_indicesCount = 0;
};