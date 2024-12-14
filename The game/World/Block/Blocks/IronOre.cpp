#include "IronOre.h"
#include "../../World.h"

IronOre::IronOre() : Block(ItemID::IRON_ORE) { }

bool IronOre::onDestroy(sf::Vector3i blockPos, Player &player, World &world) {
	player.addItem(ItemDatabase::get().getItem(ItemID::IRON_INGOT));
	return true;
}
