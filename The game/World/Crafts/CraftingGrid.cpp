#include "CraftingGrid.h"

CraftingGrid::CraftingGrid(const std::vector<ItemID>& items) {
	m_n = int(floorf(sqrtf(items.size())));
	m_grid.insert(m_grid.end(), items.begin(), items.begin() + m_n * m_n);
}

ShapelessRecipe CraftingGrid::getShapelessRecipe() const {
	std::vector<ItemID> ingridients;
	for (int i = 0; i < m_n * m_n; ++i)
		if (m_grid[i] != 0)
			ingridients.push_back(m_grid[i]);
	return ShapelessRecipe(ItemStack(), ingridients);
}

ShapedRecipe CraftingGrid::getShapedRecipe() const {
	ItemID const* grid = m_grid.data();
	int rX = m_n, rY = m_n, cX = 0, cY = 0;

	// Cut by x
	for (int x = cX; x < rX; ++x) {
		bool isEmpty = true;
		for (int y = cY; y < rY; ++y) {
			if (grid[m_n * y + x] != 0) {
				isEmpty = false;
				break;
			}
		}

		if (isEmpty)
			cX++;
		else
			break;
	}

	for (int x = rX - 1; x >= cX; --x) {
		bool isEmpty = true;
		for (int y = cY; y < rY; ++y) {
			if (grid[m_n * y + x] != 0) {
				isEmpty = false;
				break;
			}
		}

		if (isEmpty)
			rX--;
		else
			break;
	}

	// Cut by y
	for (int y = cY; y < rY; ++y) {
		bool isEmpty = true;
		for (int x = cX; x < rX; ++x) {
			if (grid[m_n * y + x] != 0) {
				isEmpty = false;
				break;
			}
		}

		if (isEmpty)
			cY++;
		else
			break;
	}

	for (int y = rY - 1; y >= cY; --y) {
		bool isEmpty = true;
		for (int x = cX; x < rX; ++x) {
			if (grid[m_n * y + x] != 0) {
				isEmpty = false;
				break;
			}
		}

		if (isEmpty)
			rY--;
		else
			break;
	}

	std::vector<ItemID> r;
	r.reserve(rX * rY);
	for (int x = cX; x < rX; ++x) {
		for (int y = cY; y < rY; ++y) {
			r.emplace_back(grid[m_n * y + x]);
		}
	}

	return ShapedRecipe(ItemStack(), r, rY - cY, rX - cX); // swap coords
}