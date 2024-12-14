#pragma once
#include "../Items/Item.h"
#include "ShapelessRecipe.h"
#include "ShapedRecipe.h"

class CraftingGrid final {
public:
	CraftingGrid(const std::vector<ItemID> &items);

	ShapelessRecipe getShapelessRecipe() const;
	ShapedRecipe getShapedRecipe() const;

private:
	std::vector<ItemID> m_grid;
	int m_n = 2;
};