#include "Grass.h"
#include "../../World.h"

Grass::Grass() : Block(ItemID::GRASS) { }

void Grass::onRightClick(sf::Vector3i blockPos, Player &player, World &world) {
	if (!isItemBlock(player.getHeldItem()->getID()))
		world.setBlock(ItemID::DIRT, blockPos);
}

bool Grass::onDestroy(sf::Vector3i blockPos, Player &player, World &world) {
	player.addItem(ItemDatabase::get().getBlock(ItemID::DIRT));
	return true;
}

void Grass::onUpdate(sf::Vector3i blockPos, World &world) {
	if (ItemDatabase::isSolid(world.getBlock(blockPos + sf::Vector3i{ 0, 1, 0 })))
		world.setBlock(ItemID::DIRT, blockPos);
}

float Grass::timeToUpdate() {
	return core::common::Random::global().randf(2.5f, 7.f);
}