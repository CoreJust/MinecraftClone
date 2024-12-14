#include "WoodenAxe.h"
#include "../../Crafts/CraftManager.h"

WoodenAxe::WoodenAxe() : Item(ItemID::WOODEN_AXE) {
	m_level = InstrumentStrength(InstrumentType::AXE, 1, 2.f);

	CraftManager::registerRecipe(ShapedRecipe(
		ItemStack(this, 1),
		{
			ItemID::PLANKS_OAK, ItemID::PLANKS_OAK,
			ItemID::PLANKS_OAK, ItemID::STICK,
			ItemID::AIR,		ItemID::STICK
		}, 2, 3
	));

	CraftManager::registerRecipe(ShapedRecipe(
		ItemStack(this, 1),
		{
			ItemID::PLANKS_OAK, ItemID::PLANKS_OAK,
			ItemID::STICK,		ItemID::PLANKS_OAK,
			ItemID::STICK,		ItemID::AIR
		}, 2, 3
	));
}
