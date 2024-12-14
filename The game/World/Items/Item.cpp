#include "Item.h"
#include "../ItemDatabase.h"
#include "../World.h"

Void* itemVoid = new Void();

Item::Item(ItemID id) : m_id(id) { }

bool Item::onDestroyBlock(sf::Vector3i blockPos, Player &player, World &world) {
	ItemID ID = world.getBlock(blockPos);
	auto type = ItemDatabase::getInstrumentType(ID);

	world.destroyBlock(blockPos);
	auto level = m_level.level;
	if (((type.type == level.type || type.type == ANY) && level.level >= type.level) || type.level == 0) {
		if (ItemDatabase::get().getBlock(ID)->onDestroy(blockPos, player, world)) {
			return true;
		}
	}

	return false;
}

InstrumentStrength Item::getInstrumentLevel() {
	return m_level;
}

ItemID Item::getID() {
	return m_id;
}

Void::Void() : Item(ItemID::AIR) { }

bool Void::onPut(sf::Vector3i clickPos, Player &player, World &world) {
	return false;
}
