#include "ShapelessRecipe.h"

ShapelessRecipe::ShapelessRecipe(ItemStack const& result, std::vector<ItemID> const& ingridients)
	: IRecipe(result) {
	for (int i = 0; i < ingridients.size(); ++i)
		m_recipe[ingridients[i]]++;
}

ShapelessRecipe::ShapelessRecipe(ItemStack const& result, std::unordered_map<int, int> ingridients)
	: IRecipe(result), m_recipe(std::move(ingridients)) { }

bool ShapelessRecipe::equals(ShapelessRecipe const& other) const {
	if (other.m_recipe.size() != m_recipe.size())
		return false;

	for (auto &i : m_recipe) {
		if (other.m_recipe.find(i.first) == other.m_recipe.end())
			return false;

		if (i.second != other.m_recipe.at(i.first))
			return false;
	}

	return true;
}
