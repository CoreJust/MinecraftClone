#pragma once
#include <Core/GL/Texture.hpp>
#include "Element.h"
#include "../Model2D.h"
#include "../World/Items/ItemStack.h"
#include "../World/Items/StackIcon.h"

class Slot final : public Element {
public:
	enum SlotType : unsigned char {
		BASIC_SLOT,
		ACTIVE_SLOT,
		INVENTORY_SLOT
	};

	enum SlotProperty : unsigned char {
		NO_PROPERTY = 0,
		CANT_PUT_ITEM = 1
	};

public:
	Slot(sf::Vector2f position = { 0.f, 0.f }, ItemStack *stack = new ItemStack());

	void draw(RenderMaster &renderer) override;

	Model2D& getModel() override;
	core::gl::Texture* getTexture() override;

	void setPosition(sf::Vector2f pos);
	void setType(SlotType t);
	void setProperties(unsigned char properties);

	sf::Vector2f getPostion();
	ItemStack& getStack();
	Item* getItem(); // shortly for getStack().getItem()
	unsigned char getProperty(SlotProperty property) const;

private:
	ItemStack m_stack;
	StackIcon m_icon;

	Model2D m_model;
	SlotType m_type = SlotType::BASIC_SLOT;
	unsigned char m_properties = NO_PROPERTY;
};

class SlotHandler final {
public:
	static bool tryExchange(Slot &slot, ItemStack &to);
	static bool tryMoveHalfItems(Slot &slot, ItemStack &to);
	static bool tryMoveItem(ItemStack &from, Slot &to);
	static void collectAllSameTo(Slot *slots, int count, ItemStack &to);

	static void moveItems(Slot &from, Slot &to);
	static void moveItems(ItemStack &from, ItemStack &to);
};