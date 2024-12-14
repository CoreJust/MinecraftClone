#pragma once
#include "Block.h"

class Stone : public Block {
public:
	Stone();

	bool onDestroy(sf::Vector3i blockPos, Player &player, World &world) override;
};