#include "Air.h"

Air::Air() : Block(ItemID::AIR) { }

bool Air::onPut(sf::Vector3i clickPos, Player &player, World &world) {
	return false;
}

void Air::onRightClick(sf::Vector3i blockPos, Player &player, World &world) { }
