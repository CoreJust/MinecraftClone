#include "HotBar.h"
#include "../Renderer/RenderMaster.h"

HotBar::HotBar() {
	sf::Vector2f pos(-0.5f, -0.8f);
	for (int i = 0; i < 9; ++i) {
		m_slots[i].setPosition(pos);
		pos.x += 0.1f;
	}
}

void HotBar::draw(RenderMaster &renderer, bool isInInventory) {
	for (int i = 0; i < 9; ++i) {
		if (!isInInventory)
			m_slots[i].setType(i == m_activeSlot ? Slot::ACTIVE_SLOT : Slot::BASIC_SLOT);
		else
			m_slots[i].setType(Slot::INVENTORY_SLOT);

		m_slots[i].draw(renderer);
	}
}

bool HotBar::addItem(Item *item) {
	for (int i = 0; i < 9; ++i) {
		auto *it = m_slots[i].getItem();
		if (it->getID() == 0) {
			m_slots[i].getStack().setItem(item);
			return true;
		}

		if (it->getID() == item->getID() && m_slots[i].getStack().getCount() < 64) {
			m_slots[i].getStack().addItem();
			return true;
		}
	}

	return false;
}

void HotBar::setActiveSlot(int n) {
	if (n < 0)
		n += 9;
	else if (n > 8)
		n -= 9;

	m_activeSlot = n;
}

int HotBar::getActiveSlotNum() {
	return m_activeSlot;
}

Slot& HotBar::getActiveSlot() {
	return m_slots[m_activeSlot];
}

Slot& HotBar::getSlot(int n) {
	return m_slots[n];
}
