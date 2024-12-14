#include "HitBoxRenderer.h"
#include "../Textures/TextureAtlas.h"
#include "../Maths/Matrix.h"

HitBoxRenderer::HitBoxRenderer() : m_shader() {
	auto coords = Atlas::getTextureCoords({ 0, 2 });
	VectorGLfloat textureCoords;

	for (unsigned int i = 0, index = 0; i < 6; ++i)
		textureCoords.insert(textureCoords.end(), coords.begin(), coords.end());

	m_model.setData(Model::getBlockVertices(), textureCoords, Model::getBlockLights(), Model::getBlockIndices());
}

void HitBoxRenderer::draw(sf::Vector3i &pos) {
	m_pos = pos;
}

void HitBoxRenderer::update(Camera &cam) {
	if (m_pos.y == -1)
		return;

	glLineWidth(2);
	m_shader.bind();
	m_model.bind();

	m_shader.projViewMat() = cam.getProjectionMatrix() * cam.getViewMatrix();
	m_shader.modelMat() = Maths::genModelMatrix({ glm::vec3(m_pos.x, m_pos.y, m_pos.z), { 0, 0, 0 } });

	glDrawElements(GL_LINES, m_model.getIndicesCount(), GL_UNSIGNED_INT, nullptr);
	m_pos.y = -1;
}
