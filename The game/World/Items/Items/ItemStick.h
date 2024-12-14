#pragma once
#include "../Item.h"

class ItemStick final : public Item {
public:
	ItemStick();

	bool onPut(sf::Vector3i clickPos, Player &player, World &world) override;
};