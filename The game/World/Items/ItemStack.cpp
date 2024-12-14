#include "ItemStack.h"

ItemStack::ItemStack(Item* item, int count) : m_item(item), m_count(count) { }

ItemStack::~ItemStack() {
	m_item = nullptr;
}

void ItemStack::setItem(Item *item, int count) {
	m_item = item;
	m_count = count;
}

void ItemStack::setCount(int count) {
	m_count = count;
	if (m_count == 0)
		setItem(itemVoid, 0);
}

void ItemStack::addItem() {
	m_count++;
}

int ItemStack::addItems(int count) {
	int diff = (m_count += count) - 64;

	if (m_count > 64) {
		m_count = 64;
	}

	return diff > 0 ? diff : 0;
}

void ItemStack::deleteItem() {
	if (m_item->getID() != 0)
		if (--m_count <= 0)
			setItem(itemVoid, 0);
}

int ItemStack::deleteItems(int count) {
	if (m_item->getID() != 0)
		if ((m_count -= count) <= 0) {
			int diff = m_count;
			setItem(itemVoid, 0);

			return diff;
		}

	return 0;
}

Item* ItemStack::getItem() {
	return m_item;
}

int ItemStack::getCount() const {
	return m_count;
}
