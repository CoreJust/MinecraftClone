#include "ChunkRenderer.h"
#include <SFML\System\Clock.hpp>
#include "../Maths/Matrix.h"
#include "../Textures/TextureAtlas.h"

ChunkRenderer::ChunkRenderer()
	: m_solidShader(), m_waterShader() { }

void ChunkRenderer::draw(Meshes &meshes) {
	m_solidInfo.push_back(RenderInfo{ meshes.solidMesh.getModel().getVAO(), meshes.solidMesh.getModel().getIndicesCount() });
	m_waterInfo.push_back(RenderInfo{ meshes.waterMesh.getModel().getVAO(), meshes.waterMesh.getModel().getIndicesCount() });
	m_positions.push_back(meshes.position);
}

void ChunkRenderer::setDestroyingBlock(const BlockPos &pos, int destroyLevel) {
	if (pos.y == -1)
		m_destroyModel.clear();

	auto coords = Atlas::getTextureCoords({ destroyLevel - 1, 1 });
	VectorGLfloat textureCoords;

	for (int i = 0; i < 6; ++i)
		textureCoords.insert(textureCoords.end(), coords.begin(), coords.end());

	m_destroyModel.setData(Model::getBlockVertices(pos), textureCoords, Model::getBlockLights(), Model::getBlockIndices());
}

void ChunkRenderer::update(const Camera &cam) {
	glEnable(GL_CULL_FACE);
	Atlas::getAtlas()->bind();
	m_solidShader.bind();
	m_solidShader.projViewMat() = cam.getProjectionMatrix() * cam.getViewMatrix();

	// solid blocks
	if (m_solidInfo.size()) {
		int index = 0;
		for (auto &i : m_solidInfo) {
			m_solidShader.chunkSectionPos() = m_positions[index++];
			glBindVertexArray(i.vao);
			glDrawElements(GL_TRIANGLES, i.indicesCount, GL_UNSIGNED_INT, nullptr);
		}

		m_solidInfo.clear();
	}

	// destroying block
	m_destroyingShader.bind();
	m_destroyingShader.projViewMat() = cam.getProjectionMatrix() * cam.getViewMatrix();

	m_destroyModel.bind();
	glDrawElements(GL_TRIANGLES, m_destroyModel.getIndicesCount(), GL_UNSIGNED_INT, nullptr);

	m_destroyModel.unbind();

	// water
	glDisable(GL_CULL_FACE);
	if (m_waterInfo.size()) {
		m_waterShader.bind();
		m_waterShader.projViewMat() = cam.getProjectionMatrix() * cam.getViewMatrix();
		m_waterShader.loadTime();

		int index = 0;
		for (auto &i : m_waterInfo) {
			m_waterShader.chunkSectionPos() = m_positions[index++];
			glBindVertexArray(i.vao);
			glDrawElements(GL_TRIANGLES, i.indicesCount, GL_UNSIGNED_INT, nullptr);
		}

		m_waterInfo.clear();
	}

	m_positions.clear();
}
