#include "WoodenPickaxe.h"
#include "../../Crafts/CraftManager.h"

WoodenPickaxe::WoodenPickaxe() : Item(ItemID::WOODEN_PICKAXE) {
	m_level = InstrumentStrength(PICKAXE, 1, 1.f);

	CraftManager::registerRecipe(ShapedRecipe(
		ItemStack(this, 1), 
		{
			ItemID::PLANKS_OAK, ItemID::PLANKS_OAK, ItemID::PLANKS_OAK,
			ItemID::AIR,		ItemID::STICK,		ItemID::AIR,
			ItemID::AIR,		ItemID::STICK,		ItemID::AIR
		}, 3, 3
	));
}
