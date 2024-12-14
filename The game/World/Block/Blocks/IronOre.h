#pragma once
#include "Block.h"

class IronOre : public Block {
public:
	IronOre();

	bool onDestroy(sf::Vector3i blockPos, Player &player, World &world) override;
};