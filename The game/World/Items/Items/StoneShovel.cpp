#include "StoneShovel.h"
#include "../../Crafts/CraftManager.h"

StoneShovel::StoneShovel() : Item(ItemID::STONE_SHOVEL) {
	m_level = InstrumentStrength(InstrumentType::SHOVEL, 2, 3.f);

	CraftManager::registerRecipe(ShapedRecipe(
		ItemStack(this, 1),
		{
			ItemID::COBBLESTONE,
			ItemID::STICK,
			ItemID::STICK
		}, 1, 3
	));
}