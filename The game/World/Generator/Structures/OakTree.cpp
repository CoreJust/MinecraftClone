#include "OakTree.h"
#include "../../../Utils/Random.h"
#include "../../../Utils/Hasher.h"
#include "../../Chunk/Chunk.h"
#include "../TerrainBuilder.h"

void OakTree::build(BlockPos pos, WorldTerrainBuilder &builder) {
	static Random<std::mt19937> rand;

	rand.setSeed(hash(0, pos.x, pos.z));
	int height = rand.random(5, 7);

	builder.fillBlocks(ItemID::LEAVES_OAK, pos + BlockPos{ 2, height - 2, 2 }, pos + BlockPos{ -2, height, -2 });
	builder.fillBlocks(ItemID::LEAVES_OAK, pos + BlockPos{ 1, height + 1, 1 }, pos + BlockPos{ -1, height + 1, -1 });
	builder.makeColumn(ItemID::LOG_OAK, pos, height);
	builder.setBlock(ItemID::DIRT, pos - BlockPos{ 0, 1, 0 });
}
