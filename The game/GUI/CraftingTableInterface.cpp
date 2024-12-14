#include "CraftingTableInterface.h"
#include "../World/Crafts/CraftManager.h"

CraftingTableInterface::CraftingTableInterface() {
	sf::Vector2f pos(-0.2f, 0.37f);
	for (int k = 0; k < 3; ++k) {
		for (int i = 0; i < 3; ++i) {
			m_craftingSlots[i * 3 + k].setPosition(pos);
			m_craftingSlots[i * 3 + k].setType(Slot::INVENTORY_SLOT);
			pos.x += 0.1f;
		}

		pos.x = -0.2f;
		pos.y -= 0.17f;
	}

	m_craftingResult.setPosition(sf::Vector2f{ 0.22f, 0.2f });
	m_craftingResult.setType(Slot::INVENTORY_SLOT);
	m_craftingResult.setProperties(Slot::CANT_PUT_ITEM);
}

void CraftingTableInterface::draw(RenderMaster &renderer, bool showInventory) {
	for (int i = 0; i < 10; ++i)
		getSlot(i).draw(renderer);
}

void CraftingTableInterface::update() {
	std::vector<ItemID> items;
	for (int i = 0; i < 9; ++i)
		items.push_back(m_craftingSlots[i].getItem()->getID());

	CraftingGrid grid = CraftingGrid(items);
	ItemStack craftingResult = CraftManager::getCraftResult(grid);
	m_craftingResult.getStack().setItem(craftingResult.getItem(), craftingResult.getCount());
}

void CraftingTableInterface::handleGettingFrom(int n) {
	if (n == 9) {
		for (int i = 0; i < 9; ++i)
			m_craftingSlots[i].getStack().deleteItem();
	}
}

Slot& CraftingTableInterface::getSlot(int n) {
	n = n % 10;
	if (n == 9)
		return m_craftingResult;
	else
		return m_craftingSlots[n];
}

int CraftingTableInterface::getSlotsCount() {
	return 10;
}

float CraftingTableInterface::getAdditionalSize() {
	return 0.59f;
}
