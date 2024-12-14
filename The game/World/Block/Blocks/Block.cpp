#include "Block.h"
#include "../../World.h"
#include "../../ItemDatabase.h"

Block::Block(ItemID id) : Item(id) { }

bool Block::onPut(sf::Vector3i clickPos, Player &player, World &world) {
	if (world.trySetBlock(m_id, clickPos, player))
		return true;

	return false;
}

void Block::onRightClick(sf::Vector3i blockPos, Player &player, World &world) { }

bool Block::onDestroy(sf::Vector3i blockPos, Player &player, World &world) {
	player.addItem(ItemDatabase::get().getBlock(m_id));
	return true;
}
