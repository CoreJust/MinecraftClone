#pragma once
#include <SFML/System/Vector2.hpp>
#include "../../Model.h"

class RenderMaster;

class ItemIcon {
public:
	ItemIcon(int ID = 0);

	void draw(RenderMaster &renderer);

	void setID(int ID);
	void setPosition(sf::Vector2f pos);
	void setZ(int z);

	Model &getModel();
	sf::Vector2f getPosition();
	int getID();

private:
	Model m_model;
	sf::Vector2f m_pos;
	int m_z = 1;
	int m_ID;
};