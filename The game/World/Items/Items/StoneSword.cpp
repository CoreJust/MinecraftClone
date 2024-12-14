#include "StoneSword.h"
#include "../../Crafts/CraftManager.h"

StoneSword::StoneSword() : Item(ItemID::STONE_SWORD) {
	m_level = InstrumentStrength(InstrumentType::SWORD, 2, 2.5f);

	CraftManager::registerRecipe(ShapedRecipe(
		ItemStack(this, 1),
		{
			ItemID::COBBLESTONE,
			ItemID::COBBLESTONE,
			ItemID::STICK
		}, 1, 3
	));
}