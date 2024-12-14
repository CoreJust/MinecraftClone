#pragma once
#include "Block.h"

class Sand : public Block {
public:
	Sand();

	void onUpdate(sf::Vector3i blockPos, World &world) override;

	float timeToUpdate() override;
};