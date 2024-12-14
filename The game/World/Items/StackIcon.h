#pragma once
#include <SFML\Graphics\Text.hpp>
#include "ItemIcon.h"

class StackIcon {
public:
	StackIcon(int ID = 0, int count = 0);

	void draw(RenderMaster &renderer);

	void setCount(int count);
	ItemIcon &getIcon();

private:
	ItemIcon m_icon;
	sf::Text m_text;
	int m_count = 0;
};