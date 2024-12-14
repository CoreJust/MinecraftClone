#include "Sand.h"
#include "../../World.h"

Sand::Sand() : Block(ItemID::SAND) { }

void Sand::onUpdate(sf::Vector3i blockPos, World &world) {
	if (!ItemDatabase::isSolid(world.getBlock(blockPos - sf::Vector3i{ 0, 1, 0 }))) {
		world.setBlock(ItemID::SAND, blockPos - sf::Vector3i{ 0, 1, 0 });
		world.setBlock(ItemID::AIR, blockPos);
	}
}

float Sand::timeToUpdate() {
	return 0.35f;
}
