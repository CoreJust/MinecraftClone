#pragma once
#include "Interface.h"
#include "HotBar.h"
#include "Background.h"

class Inventory final : public Interface {
public:
	Inventory();

	void draw(RenderMaster &renderer, bool showInventory) override;
	void input();

	void addItem(Item *item);

	void setInterface(AdditionalInterface *i);
	HotBar& getHotBar();
	Slot& getSlot(int n) override;

	Slot* getSlotAt(sf::Vector2f pos, int &n);

private:
	Background m_ground;
	HotBar m_hotBar;
	AdditionalInterface *m_interface = nullptr;
	Slot m_slots[27];

	ItemStack m_heldStack; // stack which we are moving in inventory
	StackIcon m_heldIcon;
};