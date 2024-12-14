#pragma once
#include "../Item.h"

class IronIngot final : public Item {
public:
	IronIngot();

	bool onPut(sf::Vector3i clickPos, Player &player, World &world) override;
};