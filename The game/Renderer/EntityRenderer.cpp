#include "EntityRenderer.h"
#include "../Entity/Entity.h"
#include "../Maths/Matrix.h"

EntityRenderer::EntityRenderer() : m_entityShader() { }

void EntityRenderer::draw(Entity *entity) {
	auto entityModel = entity->getEntityModel();
	m_renderInfos.push_back({ entityModel->texture, entityModel->genModel(), entity->getObj(), entity->getSize() });
}

void EntityRenderer::update(const Camera &cam) {
	if (m_renderInfos.size()) {
		glDisable(GL_CULL_FACE);
		m_entityShader.bind();
		m_entityShader.projViewMat() = cam.getProjectionMatrix() * cam.getViewMatrix();

		for (auto &i : m_renderInfos) {
			m_entityShader.modelMat() = Maths::genModelMatrix(i.obj, i.size * 0.5);
			i.texture->bind(GL_TEXTURE0);
			i.model->bind();

			glDrawElements(GL_TRIANGLES, i.model->getIndicesCount(), GL_UNSIGNED_INT, nullptr);
			i.model->unbind();
			delete i.model;
		}

		m_renderInfos.clear();
	}
}
