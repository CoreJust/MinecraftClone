#pragma once
#include "ShapelessRecipe.h"
#include "CraftingGrid.h"

class CraftManager final {
public:
	static void registerRecipe(const ShapelessRecipe &recipe);
	static void registerRecipe(const ShapedRecipe &recipe);
	static ItemStack& getCraftResult(const CraftingGrid &grid);

private:
	static std::vector<ShapelessRecipe> s_shapelessRecipes;
	static std::vector<ShapedRecipe> s_shapedRecipes;
};