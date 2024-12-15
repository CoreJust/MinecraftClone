#include "GUIRenderer.h"
#include <Core/GL/ShaderBuilder.hpp>
#include <Core/GL/Texture.hpp>

namespace {
	core::gl::Shader buildGUIShader() {
		return core::gl::ShaderBuilder("gui_shader")
			.buildFromFiles("gui_vertex", "gui_fragment");
	}
}

GUIRenderer::GUIRenderer() : m_shader(buildGUIShader()) { }

void GUIRenderer::draw(Element *elem) {
	m_elements.push_back(RenderInfo{ elem->getTexture(), elem->getModel().getVAO(), elem->getModel().getVerticesCount() });
}

void GUIRenderer::update() {
	m_shader.bind();
	for (auto &elem : m_elements) {
		elem.text->bind();
		glBindVertexArray(elem.vao);
		glDrawArrays(GL_TRIANGLES, 0, elem.verticesCount);
	}

	m_shader.unbind();
	m_elements.clear();
}
