#pragma once
#include "Block.h"

class Air : public Block {
public:
	Air();

	bool onPut(sf::Vector3i clickPos, Player &player, World &world) override;
	void onRightClick(sf::Vector3i blockPos, Player &player, World &world) override;
};