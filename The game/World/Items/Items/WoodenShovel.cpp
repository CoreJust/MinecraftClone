#include "WoodenShovel.h"
#include "../../Crafts/CraftManager.h"

WoodenShovel::WoodenShovel() : Item(ItemID::WOODEN_SHOVEL) {
	m_level = InstrumentStrength(InstrumentType::SHOVEL, 1, 1.5f);

	CraftManager::registerRecipe(ShapedRecipe(
		ItemStack(this, 1),
		{
			ItemID::PLANKS_OAK,
			ItemID::STICK,
			ItemID::STICK
		}, 1, 3
	));
}