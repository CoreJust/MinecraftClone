#pragma once

#define isItemBlock(item) ((item) <= BLOCK_TYPES_CNT)

enum ItemID : unsigned char {
	AIR = 0,
	DIRT = 1,
	GRASS = 2,
	COBBLESTONE = 3,
	STONE = 4,
	SAND = 5,
	LOG_OAK = 6,
	PLANKS_OAK = 7,
	LEAVES_OAK = 8,

	BEDROCK = 9,
	WATER = 10,

	CRAFTING_TABLE = 11,

	IRON_ORE = 12,

	STICK = 128,
	WOODEN_PICKAXE = 129,
	WOODEN_AXE = 130,
	WOODEN_SHOVEL = 131,
	WOODEN_SWORD = 132,
	STONE_PICKAXE = 134,
	STONE_AXE = 135,
	STONE_SHOVEL = 136,
	STONE_SWORD = 137,

	IRON_INGOT = 138,

	ITEM_TYPES_CNT
};

constexpr int BLOCK_TYPES_CNT = 127;

enum InstrumentType : unsigned char {
	ANY = 0,
	PICKAXE,
	AXE,
	SHOVEL,
	SWORD
};

struct InstrumentLevel { // for blocks
	InstrumentLevel() : type(ANY), level(0) { }
	InstrumentLevel(InstrumentType t, int l) : type(t), level(l) { }

	InstrumentType type;
	int level;
};

struct InstrumentStrength { // for tools
	InstrumentStrength() : level(), efficiency(1.f) { }
	InstrumentStrength(InstrumentType t, int l, float e) : level(t, l), efficiency(e) { }

	InstrumentLevel level;
	float efficiency;
};