#pragma once
#include "Mesh.h"
#include "ChunkSection.h"
#include "../../Camera.h"
#include "../../Maths/AABB.h"
#include "IChunk.h"

class RenderMaster;
class World;

class Chunk final : public IChunk {
private:
	struct State {
		bool made = false;
		bool buffered = false;
	} m_state;

public:
	Chunk();
	Chunk(World &world, VectorXZ pos);

	int draw(Camera &cam, RenderMaster &renderer);
	void makeMesh();

	ChunkSection& getSection(int index);
	State getState() const;

	void makeColumn(ItemID block, int x, int y, int z, int height) override;
	void setBlock(ItemID block, int x, int y, int z) override;
	void destroyBlock(int x, int y, int z) override;

	ItemID getBlock(int x, int y, int z);
	ItemID at(int x, int y, int z) const;

private:
	void updateMaxHeight(int x, int z, int yMax);

private:
	World *m_pWorld = nullptr;
	ItemID m_blocks[16][16][256];
	ChunkSection m_sections[16];

public:
	unsigned char treeMap[16][16];
	unsigned char heightMap[16][16];
	unsigned char maxHeight;
	bool regenFlag = false;
};