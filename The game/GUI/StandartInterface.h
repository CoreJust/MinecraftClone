#pragma once
#include "Interface.h"

class StandartInterface final : public AdditionalInterface {
public:
	StandartInterface();

	void draw(RenderMaster &renderer, bool showInventory) override;
	void update() override;
	void handleGettingFrom(int n) override;

	Slot& getSlot(int n) override;
	int getSlotsCount() override;
	float getAdditionalSize() override;

private:
	Slot m_craftingSlots[4];
	Slot m_craftingResult;
};