#include "Chunk.h"
#include <cstring>
#include <cmath>
#include "BoundsCheck.h"
#include "MeshBuilder.h"
#include "../../Renderer/RenderMaster.h"
#include "../World.h"

Chunk::Chunk() : IChunk({ 0, 0 }, true) { }

Chunk::Chunk(World &world, VectorXZ pos)
	: m_pWorld(&world), IChunk(pos, true) {
	memset(m_blocks, 0, sizeof(m_blocks));
	memset(heightMap, 0, sizeof(heightMap));

	for (int i = 0; i < 16; ++i)
		m_sections[i].init(m_pos.x, i, m_pos.z);
}

int Chunk::draw(Camera &cam, RenderMaster &renderer) {
	int r = 0;
	for (int i = 0; i < 16; ++i) {
		if (!m_sections[i].isMade)
			break;

		ChunkSection &section = m_sections[i];
		if (section.meshes.hasFaces()) {
			if (!cam.getFrustum().boxInFrustum(section.aabb))
				continue;

			++r;
			if (!section.isBuffered)
				section.buffer();

			renderer.draw(section);
		}
	}

	return r;
}

void Chunk::makeMesh() {
	MeshBuilder(*this).buildMesh(m_sections);
	m_state.made = true;
	m_state.buffered = false;
}

ChunkSection& Chunk::getSection(int index) {
	return m_sections[index];
}

Chunk::State Chunk::getState() const {
	return m_state;
}

void Chunk::makeColumn(ItemID block, int x, int y, int z, int height) {
	memset(&m_blocks[x][z][y], block, height);
	int columnYMax = y + height - 1;
	if (columnYMax > heightMap[x][z]) {
		heightMap[x][z] = columnYMax;
		if (columnYMax > maxHeight)
			maxHeight = columnYMax;
	}
}

void Chunk::setBlock(ItemID block, int x, int y, int z) {
	// Change height map and maxHeight
	if (heightMap[x][z] < y) {
		heightMap[x][z] = y;
		if (maxHeight < y)
			maxHeight = y;
	}

	m_blocks[x][z][y] = block;
}

void Chunk::destroyBlock(int x, int y, int z) {
	if (heightMap[x][z] == y)
		updateMaxHeight(x, z, y - 1);

	m_blocks[x][z][y] = ItemID::AIR;
}

ItemID Chunk::getBlock(int x, int y, int z) {
	VectorXZ chunkPos = Bounds::getChunk(m_pos, x, z);
	if (chunkPos == m_pos)
		return at(x, y, z);

	auto &map = m_pWorld->getChunks();
	if (map.find(chunkPos) != map.end())
		return map[chunkPos].at(x, y, z);

	return ItemID::AIR;
}

ItemID Chunk::at(int x, int y, int z) const {
	if (y < 0 || y > 255)
		return ItemID::AIR;

	return m_blocks[x][z][y];
}

void Chunk::updateMaxHeight(int x, int z, int yMax) {
	for (; yMax >= 0; --yMax) {
		if (at(x, yMax, z) != ItemID::AIR) {
			heightMap[x][z] = yMax;
			break;
		}
	}

	for (int mX = 0; mX < 16; ++mX)
		for (int mZ = 0; mZ < 16; ++mZ)
			if (heightMap[mX][mZ] > maxHeight)
				maxHeight = heightMap[mX][mZ];
}
