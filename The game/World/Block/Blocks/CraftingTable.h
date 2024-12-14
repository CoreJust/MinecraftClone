#pragma once
#include "Block.h"

class CraftingTable : public Block {
public:
	CraftingTable();

	void onRightClick(sf::Vector3i blockPos, Player &player, World &world) override;
};