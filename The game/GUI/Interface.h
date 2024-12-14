#pragma once
#include "Slot.h"

class Interface {
public:
	virtual void draw(RenderMaster &renderer, bool flag) = 0;

	virtual Slot& getSlot(int n) = 0;
};

class AdditionalInterface : public Interface {
public:
	virtual void update() { }
	virtual void handleGettingFrom(int n) { }

	virtual float getAdditionalSize() = 0;
	virtual int getSlotsCount() = 0;
};