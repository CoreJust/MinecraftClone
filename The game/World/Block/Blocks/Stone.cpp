#include "Stone.h"
#include "../../World.h"

Stone::Stone() : Block(ItemID::STONE) { }

bool Stone::onDestroy(sf::Vector3i blockPos, Player &player, World &world) {
	player.addItem(ItemDatabase::get().getBlock(ItemID::COBBLESTONE));
	return true;
}
