#pragma once
#include <vector>
#include <SFML\Graphics.hpp>

class SFMLRenderer final {
public:
	void draw(sf::Drawable &drawable);
	void update(sf::RenderWindow &window);

private:
	std::vector<sf::Drawable*> m_drawables;
};