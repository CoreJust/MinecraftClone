#include "StoneAxe.h"
#include "../../Crafts/CraftManager.h"

StoneAxe::StoneAxe() : Item(ItemID::STONE_AXE) {
	m_level = InstrumentStrength(InstrumentType::AXE, 2, 4.f);

	CraftManager::registerRecipe(ShapedRecipe(
		ItemStack(this, 1),
		{
			ItemID::COBBLESTONE, ItemID::COBBLESTONE,
			ItemID::COBBLESTONE, ItemID::STICK,
			ItemID::AIR,		 ItemID::STICK
		}, 2, 3
	));

	CraftManager::registerRecipe(ShapedRecipe(
		ItemStack(this, 1),
		{
			ItemID::COBBLESTONE, ItemID::COBBLESTONE,
			ItemID::STICK,		 ItemID::COBBLESTONE,
			ItemID::STICK,		 ItemID::AIR
		}, 2, 3
	));
}
