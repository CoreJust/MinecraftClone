#include "StonePickaxe.h"
#include "../../Crafts/CraftManager.h"

StonePickaxe::StonePickaxe() : Item(ItemID::STONE_PICKAXE) {
	m_level = InstrumentStrength(PICKAXE, 2, 2.f);

	CraftManager::registerRecipe(ShapedRecipe(
		ItemStack(this, 1),
		{
			ItemID::COBBLESTONE, ItemID::COBBLESTONE, ItemID::COBBLESTONE,
			ItemID::AIR,		 ItemID::STICK,		  ItemID::AIR,
			ItemID::AIR,		 ItemID::STICK,		  ItemID::AIR
		}, 3, 3
	));
}
