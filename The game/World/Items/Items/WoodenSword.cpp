#include "WoodenSword.h"
#include "../../Crafts/CraftManager.h"

WoodenSword::WoodenSword() : Item(ItemID::WOODEN_SWORD) {
	m_level = InstrumentStrength(InstrumentType::SWORD, 1, 1.65f);

	CraftManager::registerRecipe(ShapedRecipe(
		ItemStack(this, 1),
		{
			ItemID::PLANKS_OAK,
			ItemID::PLANKS_OAK,
			ItemID::STICK
		}, 1, 3
	));
}