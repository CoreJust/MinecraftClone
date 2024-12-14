#pragma once
#include <vector>
#include "IChunk.h"
#include "../../Maths/BlockPos.h"

class Chunk;

struct PreBlock {
	PreBlock(ItemID t, int x, int y, int z);

	int getX();
	int getY();
	int getZ();

	CBlockPos pos;
	ItemID type;
};

class PreChunk final : public IChunk {
public:
	PreChunk(VectorXZ pos = { 0, 0 });

	void fillTo(Chunk &chunk);

	void makeColumn(ItemID block, int x, int y, int z, int height) override;
	void setBlock(ItemID block, int x, int y, int z) override;
	void destroyBlock(int x, int y, int z) override;

private:
	std::vector<PreBlock> m_blocks;
};