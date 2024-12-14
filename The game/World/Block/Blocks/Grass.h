#pragma once
#include "Block.h"

class Grass : public Block {
public:
	Grass();

	void onRightClick(sf::Vector3i blockPos, Player &player, World &world) override;
	bool onDestroy(sf::Vector3i blockPos, Player &player, World &world) override;

	void onUpdate(sf::Vector3i blockPos, World &world) override;

	float timeToUpdate() override;
};