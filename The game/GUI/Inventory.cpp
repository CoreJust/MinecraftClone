#include "Inventory.h"
#include <ctime>
#include "../World/ItemDatabase.h"
#include "../Renderer/RenderMaster.h"

Inventory::Inventory() : m_ground(sf::Vector2f{ -0.52f, -0.825f }, sf::Vector2f{ 0.92f, 0.9f }) {
	sf::Vector2f pos(-0.5f, -0.22f);
	for (int k = 0; k < 3; ++k) {
		for (int i = 0; i < 9; ++i) {
			m_slots[k * 9 + i].setPosition(pos);
			m_slots[k * 9 + i].setType(Slot::INVENTORY_SLOT);
			pos.x += 0.1f;
		}

		pos.x = -0.5f;
		pos.y -= 0.17f;
	}

	m_heldIcon.getIcon().setZ(0);
}

void Inventory::draw(RenderMaster &renderer, bool showInventory) {
	if (showInventory)
		m_ground.draw(renderer);

	m_hotBar.draw(renderer, showInventory);

	if (showInventory) {
		for (int i = 0; i < 27; ++i)
			m_slots[i].draw(renderer);

		if (m_interface != nullptr)
			m_interface->draw(renderer, true);

		m_heldIcon.getIcon().setID(m_heldStack.getItem()->getID());
		m_heldIcon.setCount(m_heldStack.getCount());
		m_heldIcon.draw(renderer);
	}
}

void Inventory::addItem(Item *item) {
	if (m_hotBar.addItem(item))
		return;
	
	for (int i = 0; i < 27; ++i) {
		auto *it = m_slots[i].getItem();
		if (it->getID() == 0) {
			m_slots[i].getStack().setItem(item);
			return;
		}

		if (it->getID() == item->getID() && m_slots[i].getStack().getCount() < 64) {
			m_slots[i].getStack().addItem();
			return;
		}
	}
}

void Inventory::setInterface(AdditionalInterface *i) {
	if (i != nullptr)
		m_ground.setSize(sf::Vector2f{ 0.92f, 0.9f + i->getAdditionalSize() });

	m_interface = i;
}

HotBar& Inventory::getHotBar() {
	return m_hotBar;
}

Slot& Inventory::getSlot(int n) {
	if (n < 9)
		return m_hotBar.getSlot(n);
	else
		return m_slots[n - 9];
}

Slot* Inventory::getSlotAt(sf::Vector2f pos, int &n) {
	static auto testCollision = [](sf::Vector2f slot, const sf::Vector2f &m) -> bool {
		slot.y += 0.065f;
		if (m.x > slot.x && m.x < slot.x + 0.09f
			&& m.y < slot.y && m.y > slot.y - 0.15f)
			return true;

		return false;
	};

	for (int i = 0; i < 36; ++i) {
		Slot &s = getSlot(i);
		if (testCollision(s.getPostion(), pos)) {
			n = i;
			return &s;
		}
	}

	if (m_interface != nullptr) {
		int slotsCount = m_interface->getSlotsCount();
		for (int i = 0; i < slotsCount; ++i) {
			Slot &s = m_interface->getSlot(i);
			if (testCollision(s.getPostion(), pos)) {
				n = i;
				return &s;
			}
		}
	}

	return nullptr;
}

void Inventory::input() {
	static bool wasPressed[2] = { false, false };
	static Slot *lastSlot;

	int n;
	auto pos = sf::Mouse::getPosition();
	m_heldIcon.getIcon().setPosition(sf::Vector2f{ RenderMaster::sfmlCoords2glCoords(pos + sf::Vector2i{ 5, 5 }) });

	if (sf::Mouse::isButtonPressed(sf::Mouse::Left)) {
		if (!wasPressed[sf::Mouse::Left]) {
			Slot *slot = getSlotAt(RenderMaster::sfmlCoords2glCoords(pos), n);

			if (slot != nullptr) {
				if (m_heldStack.getItem()->getID() != 0 && sf::Keyboard::isKeyPressed(sf::Keyboard::C)) {
					SlotHandler::collectAllSameTo(m_slots, 27, m_heldStack);
					SlotHandler::collectAllSameTo(&m_hotBar.getSlot(0), 9, m_heldStack);

					if (m_interface != nullptr)
						SlotHandler::collectAllSameTo(&m_interface->getSlot(0), m_interface->getSlotsCount(), m_heldStack);
				}

				if (slot->getItem()->getID() != m_heldStack.getItem()->getID()) {
					if (SlotHandler::tryExchange(*slot, m_heldStack))
						m_interface->handleGettingFrom(n);
				} else if (slot->getItem()->getID() != 0){
					if (slot->getProperty(Slot::CANT_PUT_ITEM)) {
						SlotHandler::moveItems(slot->getStack(), m_heldStack);
						m_interface->handleGettingFrom(n);
					} else {
						SlotHandler::moveItems(m_heldStack, slot->getStack());
					}
				}
			}
		}

		wasPressed[sf::Mouse::Left] = true;
	} else {
		wasPressed[sf::Mouse::Left] = false;
	}

	static bool isPutting = false;
	if (sf::Mouse::isButtonPressed(sf::Mouse::Right)) {
		static sf::Clock timer;

		Slot *slot = getSlotAt(RenderMaster::sfmlCoords2glCoords(pos), n);
		if (slot != nullptr) {
			ItemStack &slotStack = slot->getStack();
			if (m_heldStack.getCount() && !slot->getProperty(Slot::CANT_PUT_ITEM) && lastSlot != slot) {
				SlotHandler::tryMoveItem(m_heldStack, *slot);
				lastSlot = slot;
				isPutting = true;
			} else if (timer.getElapsedTime().asSeconds() >= 0.4f && slotStack.getCount() && lastSlot != slot && !isPutting) {
				if (SlotHandler::tryMoveHalfItems(*slot, m_heldStack))
					m_interface->handleGettingFrom(n);

				timer.restart();
				lastSlot = slot;
			}
		}

		wasPressed[sf::Mouse::Right] = true;
	} else {
		wasPressed[sf::Mouse::Right] = false;
		lastSlot = nullptr;
		isPutting = false;
	}

	if (m_interface != nullptr)
		m_interface->update();
}
