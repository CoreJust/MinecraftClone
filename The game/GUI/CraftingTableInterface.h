#pragma once
#include "Interface.h"

class CraftingTableInterface : public AdditionalInterface {
public:
	CraftingTableInterface();

	void draw(RenderMaster &renderer, bool showInventory) override;
	void update() override;
	void handleGettingFrom(int n) override;

	Slot& getSlot(int n) override;
	int getSlotsCount() override;
	float getAdditionalSize() override;

private:
	Slot m_craftingSlots[9];
	Slot m_craftingResult;
};