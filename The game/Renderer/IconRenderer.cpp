#include "IconRenderer.h"
#include <glm\gtc\matrix_transform.hpp>
#include "../Textures/TextureAtlas.h"

IconRenderer::IconRenderer() : m_shader() { }

void IconRenderer::draw(ItemIcon &icon, int z) {
	auto &model = icon.getModel();
	m_renderInfos.push_back(RenderInfo{ icon.getPosition(), z, model.getVAO(), model.getIndicesCount() });
}

void IconRenderer::update() {
	Atlas::getAtlas()->bind(GL_TEXTURE0);
	m_shader.bind();

	glm::mat4 m;
	m = glm::rotate(m, glm::radians(325.f), { 1, 0, 0 });
	m = glm::rotate(m, glm::radians(35.f), { 0, 1, 0 });

	m_shader.matrix() = m;
	for (int z = 1; z >= 0; --z) {
		glClear(GL_DEPTH_BUFFER_BIT);
		for (auto &i : m_renderInfos) {
			if (z != i.z)
				continue;

			m_shader.pos() = glm::vec2(i.pos.x, i.pos.y);
			glBindVertexArray(i.vao);
			glDrawElements(GL_TRIANGLES, i.indicesCount, GL_UNSIGNED_INT, nullptr);
		}
	}

	m_renderInfos.clear();
}
