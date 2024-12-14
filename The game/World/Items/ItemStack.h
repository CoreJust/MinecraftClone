#pragma once
#include "Item.h"

class ItemStack {
public:
	ItemStack(Item *item = itemVoid, int count = 0);
	~ItemStack();

	void setItem(Item *item, int count = 1);
	void setCount(int count);

	void addItem();
	int addItems(int count);
	void deleteItem();
	int deleteItems(int count);

	Item *getItem();
	int getCount() const;

private:
	Item *m_item;
	int m_count = 0;
};