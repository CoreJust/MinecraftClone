#include "PreChunk.h"
#include "Chunk.h"

PreChunk::PreChunk(VectorXZ pos) : IChunk(pos, false) { }

void PreChunk::fillTo(Chunk &chunk) {
	for (auto &b : m_blocks) {
		if (b.type == ItemID::LEAVES_OAK)
			if (chunk.getBlock(b.getX(), b.getY(), b.getZ()) != ItemID::AIR)
				continue;

		chunk.setBlock(b.type, b.getX(), b.getY(), b.getZ());
	}

	m_blocks.clear();
	m_blocks.shrink_to_fit();
}

void PreChunk::makeColumn(ItemID block, int x, int y, int z, int height) {
	for (int i = 0; i < height; ++i)
		m_blocks.push_back(PreBlock(block, x, y + i, z));
}

void PreChunk::setBlock(ItemID block, int x, int y, int z) {
	m_blocks.push_back(PreBlock(block, x, y, z));
}

void PreChunk::destroyBlock(int x, int y, int z) {
	m_blocks.push_back(PreBlock(ItemID::AIR, x, y, z));
}

PreBlock::PreBlock(ItemID t, int x, int y, int z) : type(t), pos(cBlockPos(x, y, z)) { }

int PreBlock::getX() {
	return cBlock_x(pos);
}

int PreBlock::getY() {
	return cBlock_y(pos);
}

int PreBlock::getZ() {
	return cBlock_z(pos);
}
