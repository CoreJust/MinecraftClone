#pragma once
#include "Block.h"

class Water : public Block {
public:
	Water();

	void onUpdate(sf::Vector3i blockPos, World &world) override;

	float timeToUpdate() override;
};