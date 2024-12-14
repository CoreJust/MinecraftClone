#pragma once
#include "Slot.h"

class HotBar {
public:
	HotBar();

	void draw(RenderMaster &renderer, bool isInInventory);

	bool addItem(Item *item);

	void setActiveSlot(int n);
	int getActiveSlotNum();
	Slot& getActiveSlot();
	Slot& getSlot(int n);

private:
	Slot m_slots[9];
	int m_activeSlot = 0;
};