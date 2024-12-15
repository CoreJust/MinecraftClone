#include "Background.h"
#include "../Renderer/RenderMaster.h"

Background::Background(sf::Vector2f pos, sf::Vector2f size) : m_pos(pos) {
	setSize(size);
}

void Background::draw(RenderMaster &renderer) {
	renderer.draw(this);
}

void Background::setSize(sf::Vector2f size) {
	float x0 = m_pos.x;
	float y0 = m_pos.y;

	float x1 = m_pos.x + size.x;
	float y1 = m_pos.y + size.y;

	VectorGLfloat vertices{
		x0, y0,
		x0, y1,
		x1, y0,

		x1, y1,
		x0, y1,
		x1, y0
	};

	m_model = Model2D(vertices, Model2D::stdTextureCoords);
}

Model2D& Background::getModel() {
	return m_model;
}

core::gl::Texture* Background::getTexture() {
	static core::gl::Texture backgroundTexture("gui/background.png");
	return &backgroundTexture;
}
