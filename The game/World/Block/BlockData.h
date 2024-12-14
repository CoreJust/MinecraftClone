#pragma once
#include <cstdint>
#include <vector>
#include <gl\glew.h>
#include <SFML\System\Vector2.hpp>
#include "../Items/ItemID.h"

struct BlockData final {
	ItemID blockID;
	sf::Vector2i topTextureCoords;
	sf::Vector2i bottomTextureCoords;
	sf::Vector2i sideTextureCoords;
	sf::Vector2i frontTextureCoords;

	std::vector<GLfloat> topTextureCoords_vec;
	std::vector<GLfloat> bottomTextureCoords_vec;
	std::vector<GLfloat> sideTextureCoords_vec;
	std::vector<GLfloat> frontTextureCoords_vec;

	bool isOpaque = true;
	bool isSolid = true;
	bool isLiquid = false;
	bool isUpdatable = false;
	InstrumentLevel instrumentType = { ANY, 0 };
	int hardness = 0;
};