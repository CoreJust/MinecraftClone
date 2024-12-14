#include "SFMLRenderer.h"
#include <gl\glew.h>

void SFMLRenderer::draw(sf::Drawable &drawable) {
	m_drawables.push_back(&drawable);
}

void SFMLRenderer::update(sf::RenderWindow &window) {
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);

	window.pushGLStates();
	window.resetGLStates();
	for (auto &d : m_drawables)
		window.draw(*d);
	window.popGLStates();

	m_drawables.clear();
}
