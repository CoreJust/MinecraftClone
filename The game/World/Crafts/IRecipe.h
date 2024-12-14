#pragma once
#include "../Items/ItemStack.h"

class IRecipe {
public:
	IRecipe(const ItemStack &s) : m_result(s) { };

	ItemStack& getResult() { return m_result; }

protected:
	ItemStack m_result;
};