#pragma once
#include "Block.h"

class Bedrock : public Block {
public:
	Bedrock();

	bool onDestroy(sf::Vector3i blockPos, Player &player, World &world) override;
};