#include "CraftManager.h"
#include "../ItemDatabase.h"

std::vector<ShapelessRecipe> CraftManager::s_shapelessRecipes = { };
std::vector<ShapedRecipe> CraftManager::s_shapedRecipes = { };

void CraftManager::registerRecipe(const ShapelessRecipe &recipe) {
	s_shapelessRecipes.push_back(recipe);
}

void CraftManager::registerRecipe(const ShapedRecipe &recipe) {
	s_shapedRecipes.push_back(recipe);
}

ItemStack& CraftManager::getCraftResult(const CraftingGrid &grid) {
	static ItemStack none;

	ShapedRecipe gridShaped = grid.getShapedRecipe();
	if (gridShaped.gridWidth() != 0) {
		for (auto &r : s_shapedRecipes) {
			if (gridShaped.equals(r))
				return r.getResult();
		}

		ShapelessRecipe gridShapeless = grid.getShapelessRecipe();
		for (auto &r : s_shapelessRecipes) {
			if (r.equals(gridShapeless))
				return r.getResult();
		}
	}

	return none;
}