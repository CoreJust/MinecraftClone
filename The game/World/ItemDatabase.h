#pragma once
#include <string>
#include "Items/ItemID.h"
#include "Block/BlockData.h"
#include "Block/Blocks\Block.h"

class ItemDatabase final {
public:
	ItemDatabase();

	void registerItemType(ItemID id, std::string name, Item* item);

	static ItemDatabase& get();
	static bool isLiquid(ItemID id);
	static bool isSolid(ItemID id);
	static bool isUpdatable(ItemID id); // returns if block can handle a random tick event
	static const InstrumentLevel& getInstrumentType(ItemID id);
	static int getHardness(ItemID id);

	static int computeHardness(ItemID held, ItemID block);

	const BlockData& getData(ItemID id) const;
	Block* getBlock(ItemID id);
	Item* getItem(ItemID id);

private:
	void loadItemData(ItemID id, std::string text_id);

private:
	Item *m_items[ITEM_TYPES_CNT];
	BlockData m_data[ITEM_TYPES_CNT];
};