#include "ItemDatabase.h"
#include <fstream>
#include "../Textures/TextureAtlas.h"

#include "Block\Blocks\Air.h"
#include "Block\Blocks\Dirt.h"
#include "Block\Blocks\Grass.h"
#include "Block\Blocks\Cobblestone.h"
#include "Block\Blocks\Stone.h"
#include "Block\Blocks\Sand.h"
#include "Block\Blocks\LogOak.h"
#include "Block\Blocks\PlanksOak.h"
#include "Block\Blocks\LeavesOak.h"
#include "Block\Blocks\Bedrock.h"
#include "Block\Blocks\Water.h"
#include "Block\Blocks\CraftingTable.h"
#include "Block\Blocks\IronOre.h"
#include "Items\Items\ItemStick.h"
#include "Items\Items\WoodenPickaxe.h"
#include "Items\Items\WoodenAxe.h"
#include "Items\Items\WoodenShovel.h"
#include "Items\Items\WoodenSword.h"
#include "Items\Items\StonePickaxe.h"
#include "Items\Items\StoneAxe.h"
#include "Items\Items\StoneShovel.h"
#include "Items\Items\StoneSword.h"
#include "Items\Items\IronIngot.h"

ItemDatabase::ItemDatabase() {
	registerItemType(ItemID::AIR, "air", new Air());
	registerItemType(ItemID::DIRT, "dirt", new Dirt());
	registerItemType(ItemID::GRASS, "grass", new Grass());
	registerItemType(ItemID::COBBLESTONE, "cobblestone", new Cobblestone());
	registerItemType(ItemID::STONE, "stone", new Stone());
	registerItemType(ItemID::SAND, "sand", new Sand());
	registerItemType(ItemID::LOG_OAK, "log_oak", new LogOak());
	registerItemType(ItemID::PLANKS_OAK, "planks_oak", new PlanksOak());
	registerItemType(ItemID::LEAVES_OAK, "leaves_oak", new LeavesOak());
	registerItemType(ItemID::BEDROCK, "bedrock", new Bedrock());
	registerItemType(ItemID::WATER, "water", new Water());
	registerItemType(ItemID::CRAFTING_TABLE, "crafting_table", new CraftingTable());
	registerItemType(ItemID::IRON_ORE, "iron_ore", new IronOre());
	registerItemType(ItemID::STICK, "stick", new ItemStick());
	registerItemType(ItemID::WOODEN_PICKAXE, "wooden_pickaxe", new WoodenPickaxe());
	registerItemType(ItemID::WOODEN_AXE, "wooden_axe", new WoodenAxe());
	registerItemType(ItemID::WOODEN_SHOVEL, "wooden_shovel", new WoodenShovel());
	registerItemType(ItemID::WOODEN_SWORD, "wooden_sword", new WoodenSword());
	registerItemType(ItemID::STONE_PICKAXE, "stone_pickaxe", new StonePickaxe());
	registerItemType(ItemID::STONE_AXE, "stone_axe", new StoneAxe());
	registerItemType(ItemID::STONE_SHOVEL, "stone_shovel", new StoneShovel());
	registerItemType(ItemID::STONE_SWORD, "stone_sword", new StoneSword());
	registerItemType(ItemID::IRON_INGOT, "iron_ingot", new IronIngot());
}

void ItemDatabase::registerItemType(ItemID id, std::string name, Item *item) {
	loadItemData(id, name);
	m_items[id] = item;
}

ItemDatabase& ItemDatabase::get() {
	static ItemDatabase ib;
	return ib;
}

bool ItemDatabase::isLiquid(ItemID id) {
	static ItemDatabase &b = get();
	return b.getData(id).isLiquid;
}

bool ItemDatabase::isSolid(ItemID id) {
	static ItemDatabase &b = get();
	return b.getData(id).isSolid;
}

bool ItemDatabase::isUpdatable(ItemID id) {
	static ItemDatabase &b = get();
	return b.getData(id).isUpdatable;
}

const InstrumentLevel& ItemDatabase::getInstrumentType(ItemID id) {
	static ItemDatabase &b = get();
	return b.getData(id).instrumentType;
}

int ItemDatabase::getHardness(ItemID id) {
	static ItemDatabase &b = get();
	return b.getData(id).hardness;
}

int ItemDatabase::computeHardness(ItemID held, ItemID block) {
	int blockHardness = getHardness(block);
	auto blockDestroyable = getInstrumentType(block);
	auto heldStrength = get().getItem(held)->getInstrumentLevel();
	auto heldLevel = heldStrength.level;
	if (blockDestroyable.type == ANY || (blockDestroyable.type == heldLevel.type && blockDestroyable.level <= heldLevel.level)
		|| blockDestroyable.level == 0)
		return (blockDestroyable.type == heldLevel.type ? int(blockHardness / heldStrength.efficiency) : blockHardness);

	int level = (blockDestroyable.type == heldLevel.type ? heldLevel.level : 0);
	return blockHardness * (blockDestroyable.level - level) * 4;
}

const BlockData& ItemDatabase::getData(ItemID id) const {
	return m_data[id];
}

Block* ItemDatabase::getBlock(ItemID id) {
	return (Block*)m_items[id];
}

Item* ItemDatabase::getItem(ItemID id) {
	return m_items[id];
}

void ItemDatabase::loadItemData(ItemID id, std::string text_id) {
	bool isBlock = isItemBlock(id);
	std::ifstream file(isBlock ? ("res/blocks/" + text_id + ".block") : ("res/items/" + text_id + ".item"));
	BlockData &r = m_data[id];
	std::string line;

	r.blockID = id;
	r.frontTextureCoords = { -1, -1 };
	while (std::getline(file, line)) {
		if (isBlock) {
			if (line == "topTextureCoords") {
				file >> r.topTextureCoords.x >> r.topTextureCoords.y;
				r.topTextureCoords_vec = Atlas::getTextureCoords(r.topTextureCoords);
			} else if (line == "bottomTextureCoords") {
				file >> r.bottomTextureCoords.x >> r.bottomTextureCoords.y;
				r.bottomTextureCoords_vec = Atlas::getTextureCoords(r.bottomTextureCoords);
			} else if (line == "sideTextureCoords") {
				file >> r.sideTextureCoords.x >> r.sideTextureCoords.y;
				r.sideTextureCoords_vec = Atlas::getTextureCoords(r.sideTextureCoords);
			} else if (line == "frontTextureCoords") {
				file >> r.frontTextureCoords.x >> r.frontTextureCoords.y;
				r.frontTextureCoords_vec = Atlas::getTextureCoords(r.frontTextureCoords);
			} else if (line == "opaque") {
				file >> r.isOpaque;
			} else if (line == "liquidness") {
				file >> r.isLiquid;
			} else if (line == "solidness") {
				file >> r.isSolid;
			} else if (line == "updatable") {
				file >> r.isUpdatable;
			} else if (line == "instrument") {
				int type;

				file >> type >> r.instrumentType.level;
				r.instrumentType.type = (InstrumentType)type;
			} else if (line == "hardness") {
				float h;

				file >> h;
				r.hardness = int(h * 1000);
			}
		} else {
			if (line == "textureCoords") {
				file >> r.topTextureCoords.x >> r.topTextureCoords.y;
				r.topTextureCoords_vec = Atlas::getTextureCoords(r.topTextureCoords);
			}
		}
	}

	if (r.frontTextureCoords.x == -1) {
		r.frontTextureCoords = r.sideTextureCoords;
		r.frontTextureCoords_vec = r.sideTextureCoords_vec;
	}

	file.close();
}
