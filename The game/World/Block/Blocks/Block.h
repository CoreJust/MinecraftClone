#pragma once
#include "../../Items/Item.h"
#include "../../Items/ItemID.h"

class Block : public Item {
public:
	Block(ItemID id);

	virtual bool onPut(sf::Vector3i clickPos, Player &player, World &world) override;

	virtual void onRightClick(sf::Vector3i blockPos, Player &player, World &world);
	virtual bool onDestroy(sf::Vector3i blockPos, Player &player, World &world);

	virtual void onUpdate(sf::Vector3i blockPos, World &world) { };

	virtual float timeToUpdate() { return 0.f; };
};