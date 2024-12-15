#pragma once
#include <vector>
#include <SFML\System\Vector2.hpp>
#include "../Items/ItemID.h"

struct BlockData final {
	ItemID blockID;
	sf::Vector2i topTextureCoords;
	sf::Vector2i bottomTextureCoords;
	sf::Vector2i sideTextureCoords;
	sf::Vector2i frontTextureCoords;

	std::vector<float> topTextureCoords_vec;
	std::vector<float> bottomTextureCoords_vec;
	std::vector<float> sideTextureCoords_vec;
	std::vector<float> frontTextureCoords_vec;

	bool isOpaque = true;
	bool isSolid = true;
	bool isLiquid = false;
	bool isUpdatable = false;
	InstrumentLevel instrumentType = { ANY, 0 };
	int hardness = 0;
};