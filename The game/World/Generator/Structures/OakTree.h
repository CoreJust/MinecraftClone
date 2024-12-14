#pragma once
#include "Structure.h"

class OakTree : public Structure {
public:
	void build(BlockPos pos, WorldTerrainBuilder &builder) override;
};