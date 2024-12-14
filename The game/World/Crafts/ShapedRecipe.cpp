#include "ShapedRecipe.h"

ShapedRecipe::ShapedRecipe(const ItemStack &result, const std::vector<ItemID> &grid, int x, int y) : IRecipe(result), m_x(x), m_y(y) {
	m_grid.insert(m_grid.end(), grid.begin(), grid.begin() + (m_x * m_y));
}

bool ShapedRecipe::equals(const ShapedRecipe &other) const {
	if (m_y != other.m_y || m_x != other.m_x)
		return false;

	return !memcmp(m_grid.data(), other.m_grid.data(), m_grid.size());
}

int ShapedRecipe::gridWidth() const {
	return m_x;
}

int ShapedRecipe::gridHeight() const {
	return m_y;
}
