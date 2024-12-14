#pragma once
#include "../../../Maths/BlockPos.h"

class WorldTerrainBuilder;

class Structure {
public:
	virtual void build(BlockPos pos, WorldTerrainBuilder &builder) = 0;
};