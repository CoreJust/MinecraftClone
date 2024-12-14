#include "StandartInterface.h"
#include "../World/Crafts/CraftManager.h"

StandartInterface::StandartInterface() {
	sf::Vector2f pos(-0.1f, 0.2f);
	for (int k = 0; k < 2; ++k) {
		for (int i = 0; i < 2; ++i) {
			m_craftingSlots[i * 2 + k].setPosition(pos);
			m_craftingSlots[i * 2 + k].setType(Slot::INVENTORY_SLOT);
			pos.x += 0.1f;
		}

		pos.x = -0.1f;
		pos.y -= 0.17f;
	}

	m_craftingResult.setPosition(sf::Vector2f{ 0.22f, 0.12f });
	m_craftingResult.setType(Slot::INVENTORY_SLOT);
	m_craftingResult.setProperties(Slot::CANT_PUT_ITEM);
}

void StandartInterface::draw(RenderMaster &renderer, bool showInventory) {
	for (int i = 0; i < 5; ++i)
		getSlot(i).draw(renderer);
}

void StandartInterface::update() {
	std::vector<ItemID> items;
	for (int i = 0; i < 4; ++i)
		items.push_back(m_craftingSlots[i].getItem()->getID());

	CraftingGrid grid = CraftingGrid(items);
	ItemStack craftingResult = CraftManager::getCraftResult(grid);
	m_craftingResult.getStack().setItem(craftingResult.getItem(), craftingResult.getCount());
}

void StandartInterface::handleGettingFrom(int n) {
	if (n == 4) {
		for (int i = 0; i < 4; ++i)
			m_craftingSlots[i].getStack().deleteItem();
	}
}

Slot& StandartInterface::getSlot(int n) {
	n = n % 5;
	if (n == 4)
		return m_craftingResult;
	else
		return m_craftingSlots[n];
}

int StandartInterface::getSlotsCount() {
	return 5;
}

float StandartInterface::getAdditionalSize() {
	return 0.42f;
}
