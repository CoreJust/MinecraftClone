#include "PlanksOak.h"
#include "../../ItemDatabase.h"
#include "../../Crafts/CraftManager.h"

PlanksOak::PlanksOak() : Block(ItemID::PLANKS_OAK) {
	CraftManager::registerRecipe(ShapelessRecipe(
		ItemStack(this, 4),
		{ { ItemID::LOG_OAK, 1 } }
	));
}
