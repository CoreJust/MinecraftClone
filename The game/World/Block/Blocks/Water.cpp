#include "Water.h"
#include "../../World.h"

Water::Water() : Block(ItemID::WATER) { }

void Water::onUpdate(sf::Vector3i blockPos, World &world) {
	static sf::Vector3i offsets[] = {
		{ 1, 0, 0 },				{ 0, 0, 1 },
		{ -1, 0, 0 }, { 0, -1, 0 }, { 0, 0, -1 }
	};

	bool isFlows = !ItemDatabase::isSolid(world.getBlock(blockPos - sf::Vector3i{ 0, 1, 0 }));

	for (int i = 0; i < 5; ++i) {
		if (world.getBlock(blockPos + offsets[i]) == ItemID::AIR 
			&& (ItemDatabase::isSolid(world.getBlock(blockPos + offsets[i] - sf::Vector3i{ 0, 1, 0 })) || !isFlows || offsets[i].y == -1))
			world.setBlock(ItemID::WATER, blockPos + offsets[i]);
	}
}

float Water::timeToUpdate() {
	return 1.1f;
}