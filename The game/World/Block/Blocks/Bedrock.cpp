#include "Bedrock.h"

Bedrock::Bedrock() : Block(ItemID::BEDROCK) { }

bool Bedrock::onDestroy(sf::Vector3i blockPos, Player &player, World &world) {
	return false;
}
