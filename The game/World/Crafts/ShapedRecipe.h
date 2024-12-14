#pragma once
#include <vector>
#include "IRecipe.h"
#include "../Items/Item.h"

class ShapedRecipe final : public IRecipe {
public:
	ShapedRecipe(const ItemStack &result, const std::vector<ItemID> &grid, int x, int y);

	bool equals(const ShapedRecipe &other) const;
	int gridWidth() const;
	int gridHeight() const;

private:
	std::vector<ItemID> m_grid;
	int m_x = 2;
	int m_y = 2;
};