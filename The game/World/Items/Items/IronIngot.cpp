#include "IronIngot.h"

IronIngot::IronIngot() : Item(ItemID::IRON_INGOT) { }

bool IronIngot::onPut(sf::Vector3i clickPos, Player &player, World &world) {
	return false;
}
