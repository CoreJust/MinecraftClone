#pragma once
#include <vector>
#include <unordered_map>
#include "IRecipe.h"
#include "../Items/Item.h"

class ShapelessRecipe final : public IRecipe {
	std::unordered_map<int, int> m_recipe;

public:
	ShapelessRecipe(ItemStack const& result, std::vector<ItemID> const& ingridients);
	ShapelessRecipe(ItemStack const& result, std::unordered_map<int, int> ingridients);

	bool equals(ShapelessRecipe const& other) const;
};