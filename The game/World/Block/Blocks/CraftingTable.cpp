#include "CraftingTable.h"
#include "../../../Entity/Player.h"
#include "../../Crafts/CraftManager.h"
#include "../../../GUI/CraftingTableInterface.h"

CraftingTable::CraftingTable() : Block(ItemID::CRAFTING_TABLE) {
	CraftManager::registerRecipe(ShapedRecipe(
		ItemStack(this, 1),
		{ ItemID::PLANKS_OAK, ItemID::PLANKS_OAK, ItemID::PLANKS_OAK, ItemID::PLANKS_OAK }, 2, 2
	));
}

void CraftingTable::onRightClick(sf::Vector3i blockPos, Player &player, World &world) {
	static CraftingTableInterface *gui = new CraftingTableInterface();
	player.openGUI(gui);
}
