#include "StackIcon.h"
#include "../../Renderer/RenderMaster.h"

StackIcon::StackIcon(int ID, int count) : m_count(count) {
	m_text = sf::Text(std::to_string(count), *RenderMaster::getFont(), 40);
	m_icon.setID(ID);
}

void StackIcon::draw(RenderMaster &renderer) {
	if (m_icon.getID() == 0)
		return;

	m_icon.draw(renderer);

	if (m_count > 1) {
		sf::Vector2i pos = RenderMaster::glCoords2sfmlCoords(m_icon.getPosition() + sf::Vector2f(0.035f, 0.05f));

		m_text.setPosition(sf::Vector2f{ (float)pos.x, (float)pos.y });
		renderer.draw(m_text);
	}
}

void StackIcon::setCount(int count) {
	m_count = count;
	m_text.setString(std::to_string(count));
}

ItemIcon& StackIcon::getIcon() {
	return m_icon;
}
