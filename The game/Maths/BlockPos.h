#pragma once
#include <algorithm>
#include <SFML\System\Vector3.hpp>

typedef sf::Vector3<int> BlockPos;
typedef unsigned short CBlockPos; // Block position in chunk

#define cBlockPos(x, y, z) CBlockPos((x) | ((y) << 4) | ((z) << 12))
#define cBlock_x(pos) ((pos) & 0xf)
#define cBlock_y(pos) (((pos) >> 4) & 0xff)
#define cBlock_z(pos) (((pos) >> 12) & 0xf)

namespace std {
	template<>
	struct hash<BlockPos> {
		size_t operator() (const BlockPos &pos) const;
	};
}