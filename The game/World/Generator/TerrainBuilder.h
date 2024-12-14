#pragma once
#include "../../Maths/BlockPos.h"
#include "../../Maths/VectorXZ.h"
#include "../Items/ItemID.h"

class Chunk;
class World;

class ChunkTerrainBuilder final {
public:
	ChunkTerrainBuilder(Chunk &chunk);

	void fillBlocks(ItemID block, BlockPos pos0, BlockPos pos1);
	void makeColumn(ItemID block, const BlockPos &pos, int height);
	void setBlock(ItemID block, const BlockPos &pos);

	Chunk& getChunk();

private:
	Chunk &m_chunk;
};

class WorldTerrainBuilder final {
public:
	WorldTerrainBuilder(World &world);

	void fillBlocks(ItemID block, BlockPos pos0, BlockPos pos1);
	void makeColumn(ItemID block, const BlockPos &pos, int height);
	void setBlock(ItemID block, const BlockPos &pos);

private:
	World &m_world;
};