#pragma once
#include <SFML/System/Vector3.hpp>
#include "../../Defs.h"
#include "ItemID.h"

class World;
class Player;

class Item {
public:
	Item(ItemID id);

	virtual bool onPut(sf::Vector3i clickPos, Player &player, World &world) { return false; } // Event when u placing the item
	virtual bool onDestroyBlock(sf::Vector3i blockPos, Player &player, World &world);

	InstrumentStrength getInstrumentLevel();
	ItemID getID();

protected:
	InstrumentStrength m_level;
	ItemID m_id;
};

class Void : public Item {
public:
	Void();

	bool onPut(sf::Vector3i clickPos, Player &player, World &world) override;
};

extern Void* itemVoid;