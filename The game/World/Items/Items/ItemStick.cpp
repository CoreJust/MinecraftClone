#include "ItemStick.h"
#include "../../ItemDatabase.h"
#include "../../Crafts/CraftManager.h"
#include "../../World.h"

ItemStick::ItemStick() : Item(ItemID::STICK) {
	CraftManager::registerRecipe(ShapedRecipe(
		ItemStack(this, 4),
		{
			ItemID::PLANKS_OAK,
			ItemID::PLANKS_OAK
		}, 1, 2
	));
}

bool ItemStick::onPut(sf::Vector3i clickPos, Player &player, World &world) {
	return false;
}
