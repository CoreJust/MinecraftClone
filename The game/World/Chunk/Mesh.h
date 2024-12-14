#pragma once
#include "../Block/BlockPosition.h"
#include "../../Maths/VectorXZ.h"
#include "../../Model.h"

class Mesh final {
public:
	void addFace(const VectorGLubyte &vertices, const VectorGLfloat &textureCoords, const VectorGLfloat &lights, 
		BlockPosition pos);

	void buffer();
	void reset();

	Model& getModel();
	GLuint getFacesCount() const;

private:
	VectorGLuint m_vertices;
	VectorGLfloat m_textureCoords;
	VectorGLfloat m_lights;
	VectorGLuint m_indices;

	Model m_model;

	GLuint m_facesCount = 0;
	GLuint m_indicesIndex = 0;
};