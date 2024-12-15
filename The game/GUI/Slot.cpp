#include "Slot.h"
#include <gl\glew.h>
#include <SFML\Graphics.hpp>
#include "../Utils/Memory.h"
#include "../Renderer/RenderMaster.h"

constexpr GLfloat slotMin = 0.f;
constexpr GLfloat slotMaxX = 0.085f;
constexpr GLfloat slotMaxY = slotMaxX * (1920 / 1080.f);

static std::vector<GLfloat> vertices {
	slotMin,  slotMin,
	slotMin,  slotMaxY,
	slotMaxX, slotMin,

	slotMaxX, slotMaxY,
	slotMin,  slotMaxY,
	slotMaxX, slotMin
};

Slot::Slot(sf::Vector2f position, ItemStack *stack) : m_stack(*stack) {
	setPosition(position);
}

void Slot::draw(RenderMaster &renderer) {
	renderer.draw(this);

	auto id = m_stack.getItem()->getID();
	if (id == 0)
		return;

	m_icon.getIcon().setID(id);
	m_icon.setCount(m_stack.getCount());
	m_icon.draw(renderer);
}

Model2D& Slot::getModel() {
	return m_model;
}

core::gl::Texture* Slot::getTexture() {
	static core::gl::Texture slotTextures[3] = {
		core::gl::Texture("gui/basic_slot.png"),
		core::gl::Texture("gui/active_slot.png"),
		core::gl::Texture("gui/inventory_slot.png")
	};

	return &slotTextures[m_type];
}

void Slot::setPosition(sf::Vector2f pos) {
	m_icon.getIcon().setPosition(pos + sf::Vector2f(0.01f, 0.04f));

	std::vector<GLfloat> slotVertices = vertices;
	for (int i = 0; i < slotVertices.size(); ++i) {
		slotVertices[i] += pos.x;
		slotVertices[++i] += pos.y;
	}

	m_model = Model2D(slotVertices, Model2D::stdTextureCoords);
}

void Slot::setType(SlotType t) {
	m_type = t;
}

void Slot::setProperties(unsigned char properties) {
	m_properties = properties;
}

sf::Vector2f Slot::getPostion() {
	return m_icon.getIcon().getPosition() - sf::Vector2f(0.01f, 0.04f);
}

ItemStack& Slot::getStack() {
	return m_stack;
}

Item* Slot::getItem() {
	return m_stack.getItem();
}

unsigned char Slot::getProperty(SlotProperty property) const {
	return m_properties & property;
}

bool SlotHandler::tryExchange(Slot &slot, ItemStack &to) {
	if (!slot.getProperty(Slot::CANT_PUT_ITEM)) {
		memory::exchange(&slot.getStack(), &to);
		return false;
	} else if (to.getItem()->getID() == 0) {
		memory::exchange(&slot.getStack(), &to);
		return true;
	}

	return false;
}

bool SlotHandler::tryMoveHalfItems(Slot &slot, ItemStack &to) {
	if (slot.getProperty(Slot::CANT_PUT_ITEM)) {
		if (to.getItem()->getID() != slot.getItem()->getID() && to.getItem()->getID() != ItemID::AIR)
			return false;

		if (to.getCount() + slot.getStack().getCount() <= 64)
			moveItems(slot.getStack(), to);

		return true;
	} else {
		ItemStack &s = slot.getStack();
		int count = (s.getCount() + 1) / 2;
		if (count + to.getCount() >= 64)
			count = 64 - to.getCount();

		to.setItem(s.getItem(), count + to.getCount());
		s.deleteItems(count);
		return false;
	}
}

bool SlotHandler::tryMoveItem(ItemStack &from, Slot &to) {
	if (to.getProperty(Slot::CANT_PUT_ITEM))
		return false;

	ItemStack &s = to.getStack();
	if (s.getItem()->getID() == 0) {
		s.setItem(from.getItem(), 1);
		from.deleteItem();
	} else if (s.getItem()->getID() == to.getItem()->getID()) {
		s.addItem();
		from.deleteItem();
	}
}

void SlotHandler::collectAllSameTo(Slot *slots, int count, ItemStack &to) {
	--count;
	for (; count >= 0; --count) {
		if (!slots[count].getProperty(Slot::CANT_PUT_ITEM)) {
			ItemStack &s = slots[count].getStack();
			if (s.getItem()->getID() == to.getItem()->getID())
				moveItems(s, to);
		}
	}
}

void SlotHandler::moveItems(Slot &from, Slot &to) {
	moveItems(from.getStack(), to.getStack());
}

void SlotHandler::moveItems(ItemStack &from, ItemStack &to) {
	if (to.getCount() == 0) {
		memory::exchange(&from, &to);
		return;
	}

	from.setCount(to.addItems(from.getCount()));
}
