#include "ShapelessRecipe.h"
#include "../../Utils/Memory.h"

ShapelessRecipe::ShapelessRecipe(const ItemStack &result, const std::vector<ItemID> &ingridients)
	: IRecipe(result) {
	for (int i = 0; i < ingridients.size(); ++i)
		m_recipe[ingridients[i]]++;
}

ShapelessRecipe::ShapelessRecipe(const ItemStack &result, std::unordered_map<int, int> &&ingridients)
	: IRecipe(result) {
	memory::exchange(&m_recipe, &ingridients);
}

bool ShapelessRecipe::equals(const ShapelessRecipe &other) {
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
