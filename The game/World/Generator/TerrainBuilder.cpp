#include "TerrainBuilder.h"
#include "../../Maths/Maths.h"
#include "../Chunk/BoundsCheck.h"
#include "../Chunk/Chunk.h"
#include "../World.h"

ChunkTerrainBuilder::ChunkTerrainBuilder(Chunk &chunk) : m_chunk(chunk) { }

void ChunkTerrainBuilder::fillBlocks(ItemID block, BlockPos pos0, BlockPos pos1) {
	static auto fix = [](int &c0, int &c1) -> void {
		c0 = (c0 < 0 ? 0 : (c0 > 15 ? 15 : c0));
		c1 = (c1 < 0 ? 0 : (c1 > 15 ? 15 : c1));
	};

	static auto trySwap = [](int &c0, int &c1) -> void {
		if (c0 > c1) {
			int tmp = c0;
			c0 = c1;
			c1 = tmp;
		}
	};

	trySwap(pos0.x, pos1.x);
	trySwap(pos0.y, pos1.y);
	trySwap(pos0.z, pos1.z);

	fix(pos0.x, pos1.x);
	fix(pos0.z, pos1.z);

	int height = pos1.y - pos0.y + 1;
	if (height == 1) {
		for (int x = pos0.x; x <= pos1.x; ++x) {
			for (int z = pos0.z; z <= pos1.z; ++z) {
				setBlock(block, { x, pos0.y, z });
			}
		}
	} else {
		for (int x = pos0.x; x <= pos1.x; ++x) {
			for (int z = pos0.z; z <= pos1.z; ++z) {
				makeColumn(block, { x, pos0.y, z }, height);
			}
		}
	}
}

void ChunkTerrainBuilder::makeColumn(ItemID block, const BlockPos &pos, int height) {
	m_chunk.makeColumn(block, pos.x, pos.y, pos.z, height);
}

void ChunkTerrainBuilder::setBlock(ItemID block, const BlockPos &pos) {
	m_chunk.setBlock(block, pos.x, pos.y, pos.z);
}

Chunk& ChunkTerrainBuilder::getChunk() {
	return m_chunk;
}

WorldTerrainBuilder::WorldTerrainBuilder(World &world) : m_world(world) { }

void WorldTerrainBuilder::fillBlocks(ItemID block, BlockPos pos0, BlockPos pos1) {
	static auto trySwap = [](int &c0, int &c1) -> void {
		if (c0 > c1) {
			int tmp = c0;
			c0 = c1;
			c1 = tmp;
		}
	};

	trySwap(pos0.x, pos1.x);
	trySwap(pos0.y, pos1.y);
	trySwap(pos0.z, pos1.z);

	int height = pos1.y - pos0.y + 1;
	if (height == 1) {
		for (int x = pos0.x; x <= pos1.x; ++x) {
			for (int z = pos0.z; z <= pos1.z; ++z) {
				setBlock(block, { x, pos0.y, z });
			}
		}
	}
	else {
		for (int x = pos0.x; x <= pos1.x; ++x) {
			for (int z = pos0.z; z <= pos1.z; ++z) {
				makeColumn(block, { x, pos0.y, z }, height);
			}
		}
	}
}

void WorldTerrainBuilder::makeColumn(ItemID block, const BlockPos &pos, int height) {
	m_world.getChunk({ toChunkPos(pos.x), toChunkPos(pos.z) })
		->makeColumn(block, abs_pos(pos.x % 16), pos.y, abs_pos(pos.z % 16), height);
}

void WorldTerrainBuilder::setBlock(ItemID block, const BlockPos &pos) {
	m_world.getChunk({ toChunkPos(pos.x), toChunkPos(pos.z) })
		->setBlock(block, abs_pos(pos.x % 16), pos.y, abs_pos(pos.z % 16));
}