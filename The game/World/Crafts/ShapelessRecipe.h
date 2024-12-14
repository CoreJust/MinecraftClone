#pragma once
#include <vector>
#include <unordered_map>
#include "IRecipe.h"
#include "../Items/Item.h"

class ShapelessRecipe final : public IRecipe {
public:
	ShapelessRecipe(const ItemStack &result, const std::vector<ItemID> &ingridients);
	ShapelessRecipe(const ItemStack &result, std::unordered_map<int, int> &&ingridients);

	bool equals(const ShapelessRecipe &other);

private:
	std::unordered_map<int, int> m_recipe;
};